/*
 * Copyright 2013-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "phenom/socket.h"
#include "phenom/sysutil.h"
#include "phenom/memory.h"
#include "phenom/job.h"
#include "phenom/log.h"
#include "phenom/dns.h"
#include "phenom/printf.h"
#include "phenom/configuration.h"

struct connect_job {
  ph_job_t job;
  ph_socket_t s;
  ph_sockaddr_t addr;
  int status;
  struct timeval start;
  void *arg;
  ph_socket_connect_func func;
};

struct resolve_and_connect {
  ph_sockaddr_t addr;
  ph_socket_t s;
  int resolve_status;
  int connect_status;
  uint16_t port;
  struct timeval start, timeout, elapsed;
  void *arg;
  ph_sock_connect_func func;
};

static ph_memtype_def_t defs[] = {
  { "socket", "connect_job", sizeof(struct connect_job), PH_MEM_FLAGS_ZERO },
  { "socket", "sock", sizeof(ph_sock_t), PH_MEM_FLAGS_ZERO },
  { "socket", "resolve_and_connect",
    sizeof(struct resolve_and_connect), PH_MEM_FLAGS_ZERO },
};
static struct {
  ph_memtype_t connect_job, sock, resolve_and_connect;
} mt;
static int ssl_sock_idx;

static uint32_t connect_affinity = 0;

ph_socket_t ph_socket_for_addr(const ph_sockaddr_t *addr, int type, int flags)
{
  ph_socket_t s;

  s = socket(addr->family, type, 0);
  if (s == -1) {
    return s;
  }

  if (flags & PH_SOCK_CLOEXEC) {
    fcntl(s, F_SETFD, FD_CLOEXEC);
  }

  if (flags & PH_SOCK_NONBLOCK) {
    ph_socket_set_nonblock(s, true);
  }

  return s;
}

static void connect_complete(ph_job_t *j, ph_iomask_t why, void *data)
{
  struct connect_job *job = data;
  int status = 0;
  struct timeval done;

  ph_unused_parameter(j);

  if (why == PH_IOMASK_TIME) {
    // Timed out waiting for connect
    status = ETIMEDOUT;
  } else {
    // Check status
    socklen_t slen = sizeof(status);

    if (getsockopt(job->s, SOL_SOCKET, SO_ERROR, (void*)&status, &slen) < 0) {
      status = errno;
    }
  }

  done = ph_time_now();
  timersub(&done, &job->start, &done);

  // This defers the free, so we are still safe to access job on
  // the following line
  ph_job_free(&job->job);
  job->job.fd = -1;

  job->func(job->s, &job->addr, status, &done, job->arg);
}

static void sock_dtor(ph_job_t *job)
{
  ph_sock_t *sock = (ph_sock_t*)job;

  if (sock->sslwbuf) {
    ph_bufq_free(sock->sslwbuf);
    sock->sslwbuf = NULL;
  }
  if (sock->ssl) {
    SSL_CTX *ctx = sock->ssl->ctx;

    if (sock->ssl_stream) {
      ph_stm_close(sock->ssl_stream);
      sock->ssl_stream = NULL;
    } else {
      SSL_free(sock->ssl);
    }
    sock->ssl = NULL;

    if (sock->free_ssl_ctx) {
      SSL_CTX_free(ctx);
    }
  }

  if (sock->wbuf) {
    ph_bufq_free(sock->wbuf);
    sock->wbuf = NULL;
  }
  if (sock->rbuf) {
    ph_bufq_free(sock->rbuf);
    sock->rbuf = NULL;
  }
  if (sock->conn) {
    ph_stm_close(sock->conn);
    sock->conn = NULL;
  }
  if (sock->stream) {
    ph_stm_close(sock->stream);
    sock->stream = NULL;
  }
}

static bool try_send(ph_sock_t *sock)
{
  while (ph_bufq_len(sock->wbuf)) {
    if (!ph_bufq_stm_write(sock->wbuf, sock->conn, NULL)) {
      if (ph_stm_errno(sock->conn) != EAGAIN) {
        return false;
      }
      // No room right now
      return true;
    }
  }
  return true;
}

static bool try_ssl_shunt(ph_sock_t *sock)
{
  if (!sock->sslwbuf) {
    return true;
  }
  while (ph_bufq_len(sock->sslwbuf)) {
    if (!ph_bufq_stm_write(sock->sslwbuf, sock->ssl_stream, NULL)) {
      if (ph_stm_errno(sock->conn) != EAGAIN) {
        return false;
      }
      // No room right now
      return true;
    }
  }
  return true;
}

static bool try_read(ph_sock_t *sock)
{
  ph_stream_t *stm = sock->ssl_stream ? sock->ssl_stream : sock->conn;

  if (!ph_bufq_stm_read(sock->rbuf, stm, NULL) &&
      ph_stm_errno(stm) != EAGAIN) {
    return false;
  }
  return true;
}

static void sock_dispatch(ph_job_t *j, ph_iomask_t why, void *data)
{
  ph_sock_t *sock = (ph_sock_t*)j;
  bool had_err = why & PH_IOMASK_ERR;

  if (j->def && j->epoch_entry.function) {
    // We're being woken up after we've been freed.
    // This is fugly.  Really.
    return;
  }

  if (sock->enabled) {
    sock->conn->need_mask = 0;

    // Push data into the SSL stream
    if (sock->sslwbuf) {
      if (try_ssl_shunt(sock)) {
        why |= PH_IOMASK_WRITE;
      } else {
        why |= PH_IOMASK_ERR;
      }
    }

    // If we have data pending write, try to get that sent, and flag
    // errors
    if (!try_send(sock)) {
      why |= PH_IOMASK_ERR;
    }

    if ((why & PH_IOMASK_ERR) == 0) {
      if (!try_read(sock)) {
        why |= PH_IOMASK_ERR;
      }
    }

    if (sock->ssl && SSL_in_init(sock->ssl)
        && SSL_total_renegotiations(sock->ssl) == 0) {
      switch (SSL_get_error(sock->ssl, SSL_do_handshake(sock->ssl))) {
        case SSL_ERROR_NONE:
          break;
        case SSL_ERROR_WANT_READ:
          if (try_send(sock)) {
            ph_job_set_nbio_timeout_in(&sock->job,
                PH_IOMASK_READ,
                sock->timeout_duration);
            return;
          }
          why |= PH_IOMASK_ERR;
          break;
        case SSL_ERROR_WANT_WRITE:
          if (try_send(sock)) {
            ph_job_set_nbio_timeout_in(&sock->job,
                PH_IOMASK_WRITE,
                sock->timeout_duration);
            return;
          }
          why |= PH_IOMASK_ERR;
          break;
        default:
          why |= PH_IOMASK_ERR;
          break;
      }
    }
  }

dispatch_again:
  if (why & PH_IOMASK_ERR) {
    had_err = true;
  }
  if (sock->enabled || (why & PH_IOMASK_WAKEUP) || (why & PH_IOMASK_ERR)) {
    sock->callback(sock, why, data);
  }

  if (!sock->enabled) {
    return;
  }

  if (!had_err) {
    uint64_t rbufsize = ph_bufq_len(sock->rbuf);

    if (!try_ssl_shunt(sock)) {
      why = PH_IOMASK_ERR;
      goto dispatch_again;
    }

    if (!try_send(sock)) {
      why = PH_IOMASK_ERR;
      goto dispatch_again;
    } else if (sock->ssl_stream) {
      // SSL writes can also read; while it remains buffered
      // in the SSL structure, we don't see it in rbuf and won't
      // get woken up by epoll.  If we hit that case, we perform
      // this speculative read, and if the rbuf_len changes we
      // know that we should make another attempt at dispatching
      // to read the remainder
      if (!try_read(sock)) {
        why = PH_IOMASK_ERR;
        goto dispatch_again;
      }

      if (ph_bufq_len(sock->rbuf) > rbufsize) {
        why = PH_IOMASK_READ;
        goto dispatch_again;
      }
    }
  }

  ph_iomask_t mask = sock->conn->need_mask | PH_IOMASK_READ;

  if (sock->ssl_stream) {
    mask |= sock->ssl_stream->need_mask;
  }

  if (ph_bufq_len(sock->wbuf) ||
      (sock->sslwbuf && ph_bufq_len(sock->sslwbuf))) {
    mask |= PH_IOMASK_WRITE;
  }

  ph_log(PH_LOG_DEBUG, "fd=%d setting mask=%x timeout={%d,%d}",
      sock->job.fd, mask, (int)sock->timeout_duration.tv_sec,
      (int)sock->timeout_duration.tv_usec);
  ph_job_set_nbio_timeout_in(&sock->job, mask, sock->timeout_duration);
}

static struct ph_job_def connect_job_template = {
  connect_complete,
  PH_MEMTYPE_INVALID,
  NULL
};

static struct ph_job_def sock_job_template = {
  sock_dispatch,
  PH_MEMTYPE_INVALID,
  sock_dtor
};

static void do_sock_init(void)
{
  ph_memtype_register_block(sizeof(defs)/sizeof(defs[0]), defs,
      &mt.connect_job);
  sock_job_template.memtype = mt.sock;
  connect_job_template.memtype = mt.connect_job;
  ssl_sock_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}
PH_LIBRARY_INIT(do_sock_init, 0)

void ph_socket_connect(ph_socket_t s, const ph_sockaddr_t *addr,
  struct timeval *timeout, ph_socket_connect_func func, void *arg)
{
  struct connect_job *job;
  int res;
  struct timeval default_timeout = { 60, 0 };
  struct timeval done = { 0, 0 };

  job = (struct connect_job*)ph_job_alloc(&connect_job_template);
  if (!job) {
    func(s, addr, ENOMEM, &done, arg);
    return;
  }

  job->s = s;
  job->addr = *addr;
  job->func = func;
  job->arg = arg;
  job->job.emitter_affinity = ck_pr_faa_32(&connect_affinity, 1);

  job->start = ph_time_now();
  res = connect(s, &job->addr.sa.sa, ph_sockaddr_socklen(&job->addr));

  if (res < 0 && errno == EINPROGRESS) {
    // Pending; let's set things up to call us back later

    job->job.fd = s;
    job->job.data = job;
    ph_job_set_nbio_timeout_in(&job->job, PH_IOMASK_WRITE,
        timeout ? *timeout : default_timeout);
    return;
  }

  done = ph_time_now();
  timersub(&done, &job->start, &done);

  // Immediate result
  func(s, addr, res == 0 ? 0 : errno, &done, arg);
  ph_job_free(&job->job);
}

#define MAX_SOCK_BUFFER_SIZE 128*1024

static bool sock_stm_close(ph_stream_t *stm)
{
  ph_unused_parameter(stm);
  return true;
}

static bool sock_stm_readv(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nread)
{
  ph_sock_t *sock = stm->cookie;
  int i;
  uint64_t tot = 0;
  uint64_t avail;
  ph_buf_t *b;
  bool res = true;

  for (i = 0; i < iovcnt; i++) {
    avail = MIN(ph_bufq_len(sock->rbuf), iov[i].iov_len);
    if (avail == 0) {
      continue;
    }

    // This allocates a buf slice; in theory, we can eliminate this
    // allocation, but in practice it's probably fine until profiling
    // tells us otherwise
    b = ph_bufq_consume_bytes(sock->rbuf, avail);
    if (!b) {
      stm->last_err = ENOMEM;
      res = false;
      break;
    }
    memcpy(iov[i].iov_base, ph_buf_mem(b), avail);
    ph_buf_delref(b);
    tot += avail;
  }

  if (nread) {
    *nread = tot;
  }

  if (tot > 0) {
    return true;
  }

  return res;
}

static bool sock_stm_writev(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nwrotep)
{
  int i;
  uint64_t n, total = 0;
  ph_sock_t *sock = stm->cookie;
  ph_bufq_t *bufq = sock->sslwbuf ? sock->sslwbuf : sock->wbuf;

  for (i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) {
      continue;
    }
    if (ph_bufq_append(bufq, iov[i].iov_base,
          iov[i].iov_len, &n) != PH_OK) {
      stm->last_err = EAGAIN;
      break;
    }
    total += n;
  }

  if (total) {
    if (nwrotep) {
      *nwrotep = total;
    }
    return true;
  }
  return false;
}

static bool sock_stm_seek(ph_stream_t *stm, int64_t delta, int whence,
    uint64_t *newpos)
{
  ph_unused_parameter(delta);
  ph_unused_parameter(whence);
  ph_unused_parameter(newpos);
  stm->last_err = ESPIPE;
  return false;
}

static struct ph_stream_funcs sock_stm_funcs = {
  sock_stm_close,
  sock_stm_readv,
  sock_stm_writev,
  sock_stm_seek
};

ph_sock_t *ph_sock_new_from_socket(ph_socket_t s, const ph_sockaddr_t *sockname,
  const ph_sockaddr_t *peername)
{
  ph_sock_t *sock;
  int64_t max_buf;

  sock = (ph_sock_t*)ph_job_alloc(&sock_job_template);
  if (!sock) {
    return NULL;
  }

  sock->free_ssl_ctx = true;

  max_buf = ph_config_query_int("$.socket.max_buffer_size",
              MAX_SOCK_BUFFER_SIZE);

  sock->wbuf = ph_bufq_new(max_buf);
  if (!sock->wbuf) {
    goto fail;
  }

  sock->rbuf = ph_bufq_new(max_buf);
  if (!sock->rbuf) {
    goto fail;
  }

  sock->conn = ph_stm_fd_open(s, 0, 0);
  if (!sock->conn) {
    goto fail;
  }

  sock->stream = ph_stm_make(&sock_stm_funcs, sock, 0, 0);
  if (!sock->stream) {
    goto fail;
  }

  if (sockname) {
    sock->sockname = *sockname;
    sock->via_sockname = *sockname;
  }
  if (peername) {
    sock->peername = *peername;
    sock->via_peername = *peername;
  }

  sock->job.fd = s;
  sock->timeout_duration.tv_sec = 60;

  return sock;

fail:
  ph_job_free(&sock->job);
  return NULL;
}

void ph_sock_free(ph_sock_t *sock)
{
  sock->enabled = false;
  ph_job_free(&sock->job);
}

ph_result_t ph_sock_wakeup(ph_sock_t *sock)
{
  return ph_job_wakeup(&sock->job);
}

void ph_sock_enable(ph_sock_t *sock, bool enable)
{
  if (sock->enabled == enable) {
    return;
  }

  sock->enabled = enable;
  if (enable) {
    // Enable for read AND write here, in case we're in a different context.
    // This may result in a spurious wakeup at the point we enable the sock,
    // but the sock_func will set this according to the buffer status once
    // we get there
    ph_job_set_nbio_timeout_in(&sock->job,
        PH_IOMASK_READ|PH_IOMASK_WRITE,
        sock->timeout_duration);
  } else {
    ph_job_set_nbio(&sock->job, 0, NULL);
  }
}

static inline void calc_elapsed(struct resolve_and_connect *rac)
{
  struct timeval now;

  now = ph_time_now();
  timersub(&now, &rac->start, &rac->elapsed);
}

static void free_rac(struct resolve_and_connect *rac)
{
  ph_mem_free(mt.resolve_and_connect, rac);
}

static void connected_sock(ph_socket_t s, const ph_sockaddr_t *addr,
    int status, struct timeval *elapsed, void *arg)
{
  struct resolve_and_connect *rac = arg;
  ph_sock_t *sock = NULL;

  ph_unused_parameter(elapsed);

  if (status == 0) {
    sock = ph_sock_new_from_socket(s, NULL, addr);

    if (!sock) {
      status = ENOMEM;
    }
  }

  if (status != 0 && rac->s != -1) {
    close(rac->s);
  }

  calc_elapsed(rac);

  if (sock) {
    rac->func(sock, PH_SOCK_CONNECT_SUCCESS, 0, addr,
        &rac->elapsed, rac->arg);
  } else {
    rac->func(NULL, PH_SOCK_CONNECT_ERRNO, status, addr,
        &rac->elapsed, rac->arg);
  }

  free_rac(rac);
}

static void attempt_connect(struct resolve_and_connect *rac)
{
  rac->s = ph_socket_for_addr(&rac->addr, SOCK_STREAM,
      PH_SOCK_CLOEXEC|PH_SOCK_NONBLOCK);

  if (rac->s == -1) {
    calc_elapsed(rac);
    rac->func(NULL, PH_SOCK_CONNECT_ERRNO, errno, &rac->addr,
        &rac->elapsed, rac->arg);
    free_rac(rac);
    return;
  }

  ph_socket_connect(rac->s, &rac->addr, &rac->timeout,
      connected_sock, rac);
}

static void did_sys_resolve(ph_dns_addrinfo_t *info)
{
  struct resolve_and_connect *rac = info->arg;

  if (info->result == 0) {
    ph_sockaddr_set_from_addrinfo(&rac->addr, info->ai);
    ph_sockaddr_set_port(&rac->addr, rac->port);
    attempt_connect(rac);
  } else {
    calc_elapsed(rac);
    rac->func(NULL, PH_SOCK_CONNECT_GAI_ERR, info->result,
        NULL, &rac->elapsed, rac->arg);
    free_rac(rac);
  }

  ph_dns_addrinfo_free(info);
}

void ph_sock_resolve_and_connect(const char *name, uint16_t port,
    struct timeval *timeout,
    int resolver, ph_sock_connect_func func, void *arg)
{
  struct timeval tv = {0, 0};
  struct resolve_and_connect *rac;
  char portstr[8];

  switch (resolver) {
    case PH_SOCK_CONNECT_RESOLVE_SYSTEM:
      break;
    default:
      func(NULL, PH_SOCK_CONNECT_GAI_ERR, EAI_NONAME, NULL, &tv, arg);
      return;
  }

  rac = ph_mem_alloc(mt.resolve_and_connect);
  if (!rac) {
    func(NULL, PH_SOCK_CONNECT_ERRNO, ENOMEM, NULL, &tv, arg);
    return;
  }

  rac->func = func;
  rac->arg = arg;
  rac->start = ph_time_now();
  rac->port = port;

  if (timeout) {
    rac->timeout = *timeout;
  } else {
    rac->timeout.tv_sec = 60;
  }

  if (ph_sockaddr_set_v4(&rac->addr, name, port) == PH_OK ||
      ph_sockaddr_set_v6(&rac->addr, name, port) == PH_OK) {
    // No need to resolve this address; it's a literal
    attempt_connect(rac);
    return;
  }

  switch (resolver) {
    case PH_SOCK_CONNECT_RESOLVE_SYSTEM:
      ph_snprintf(portstr, sizeof(portstr), "%d", port);
      if (ph_dns_getaddrinfo(name, portstr, NULL,
            did_sys_resolve, rac) == PH_OK) {
        return;
      }
      calc_elapsed(rac);
      func(NULL, PH_SOCK_CONNECT_ERRNO, errno, NULL, &rac->elapsed, arg);
      free_rac(rac);
      break;
  }
}

ph_buf_t *ph_sock_read_bytes_exact(ph_sock_t *sock, uint64_t len)
{
  return ph_bufq_consume_bytes(sock->rbuf, len);
}

ph_buf_t *ph_sock_read_record(ph_sock_t *sock, const char *delim,
    uint32_t delim_len)
{
  return ph_bufq_consume_record(sock->rbuf, delim, delim_len);
}

ph_buf_t *ph_sock_read_line(ph_sock_t *sock)
{
  return ph_bufq_consume_record(sock->rbuf, "\r\n", 2);
}

static void ssl_info_callback(const SSL *ssl, int where, int ret)
{
  if (where & SSL_CB_HANDSHAKE_DONE) {
    ph_sock_t *sock = SSL_get_ex_data(ssl, ssl_sock_idx);
    if (sock->handshake_cb) {
      sock->handshake_cb(sock, ret);
    }
  }
}

void ph_sock_openssl_enable(ph_sock_t *sock, SSL *ssl,
    bool is_client, ph_sock_openssl_handshake_func handshake_cb)
{
  BIO *rbio, *wbio;

  if (sock->ssl) {
    return;
  }

  // Read from the socket
  rbio = ph_openssl_bio_wrap_stream(sock->conn);

  // Write to our bufq
  wbio = ph_openssl_bio_wrap_bufq(sock->wbuf);

  sock->ssl = ssl;
  SSL_set_bio(ssl, rbio, wbio);

  // Allow looking up the sock from the SSL object
  SSL_set_ex_data(ssl, ssl_sock_idx, sock);

  sock->ssl_stream = ph_stm_ssl_open(ssl);
  SSL_set_mode(ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

  sock->sslwbuf = ph_bufq_new(ph_config_query_int(
        "$.socket.max_buffer_size", MAX_SOCK_BUFFER_SIZE));
  sock->handshake_cb = handshake_cb;
  if (handshake_cb) {
    SSL_set_info_callback(ssl, ssl_info_callback);
  }

  if (is_client) {
    SSL_set_connect_state(ssl);
  } else {
    SSL_set_accept_state(ssl);
  }
}

/* vim:ts=2:sw=2:et:
 */

