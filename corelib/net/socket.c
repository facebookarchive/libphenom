/*
 * Copyright 2013 Facebook, Inc.
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
static pthread_once_t done_sock_init = PTHREAD_ONCE_INIT;

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
  job->func(job->s, &job->addr, status, &done, job->arg);
  ph_job_free(&job->job);
}

static void sock_dtor(ph_job_t *job)
{
  ph_sock_t *sock = (ph_sock_t*)job;

  if (sock->wbuf) {
    ph_bufq_free(sock->wbuf);
  }
  if (sock->rbuf) {
    ph_bufq_free(sock->rbuf);
  }
  if (sock->conn) {
    ph_stm_close(sock->conn);
  }
  if (sock->stream) {
    ph_stm_close(sock->stream);
  }
}

static bool try_send(ph_sock_t *sock)
{
  if (ph_bufq_len(sock->wbuf) &&
      !ph_bufq_stm_write(sock->wbuf, sock->conn, NULL) &&
      ph_stm_errno(sock->conn) != EAGAIN) {
    return false;
  }
  return true;
}

static bool try_read(ph_sock_t *sock)
{
  if (!ph_bufq_stm_read(sock->rbuf, sock->conn, NULL) &&
      ph_stm_errno(sock->conn) != EAGAIN) {
    return false;
  }
  return true;
}

static void sock_dispatch(ph_job_t *j, ph_iomask_t why, void *data)
{
  ph_sock_t *sock = (ph_sock_t*)j;

  sock->conn->need_mask = 0;

  if ((why & (PH_IOMASK_WRITE|PH_IOMASK_ERR)) == PH_IOMASK_WRITE) {
    // If we have data pending write, try to get that sent, and flag
    // errors
    if (!try_send(sock)) {
      why |= PH_IOMASK_ERR;
    }
  }
  if ((why & (PH_IOMASK_READ|PH_IOMASK_ERR)) == PH_IOMASK_READ) {
    if (!try_read(sock)) {
      why |= PH_IOMASK_ERR;
    }
  }

  sock->callback(sock, why, data);

  if (sock->enabled) {
    ph_iomask_t mask = sock->conn->need_mask | PH_IOMASK_READ;

    if (ph_bufq_len(sock->wbuf)) {
      mask |= PH_IOMASK_WRITE;
    }

    ph_job_set_nbio_timeout_in(&sock->job, mask, sock->timeout_duration);
  }
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
  ph_memtype_register_block(sizeof(defs)/sizeof(defs[0]), defs, &mt.connect_job);
  sock_job_template.memtype = mt.sock;
  connect_job_template.memtype = mt.connect_job;
}

void ph_socket_connect(ph_socket_t s, const ph_sockaddr_t *addr,
  struct timeval *timeout, ph_socket_connect_func func, void *arg)
{
  struct connect_job *job;
  int res;
  struct timeval default_timeout = { 60, 0 };
  struct timeval done = { 0, 0 };

  pthread_once(&done_sock_init, do_sock_init);

  job = (struct connect_job*)ph_job_alloc(&connect_job_template);
  if (!job) {
    func(s, addr, ENOMEM, &done, arg);
    return;
  }

  job->s = s;
  job->addr = *addr;
  job->func = func;
  job->arg = arg;

  job->start = ph_time_now();
  res = connect(s, &job->addr.sa.sa, ph_sockaddr_socklen(&job->addr));

  if (res < 0 && errno == EINPROGRESS) {
    // Pending; let's set things up to call us back later

    job->job.fd = s;
    job->job.callback = connect_complete;
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

// Should probably be configurable
#define MAX_SOCK_BUFFER_SIZE 128*1024

static bool sock_stm_close(ph_stream_t *stm)
{
  ph_unused_parameter(stm);
  return false;
}

static bool sock_stm_readv(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nread)
{
  ph_unused_parameter(stm);
  ph_unused_parameter(iov);
  ph_unused_parameter(iovcnt);
  ph_unused_parameter(nread);
  stm->last_err = ENOSYS;
  return false;
}

static bool sock_stm_writev(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nwrotep)
{
  int i;
  uint64_t n, total = 0;
  ph_sock_t *sock = stm->cookie;

  for (i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) {
      continue;
    }
    if (ph_bufq_append(sock->wbuf, iov[i].iov_base,
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

  pthread_once(&done_sock_init, do_sock_init);

  sock = (ph_sock_t*)ph_job_alloc(&sock_job_template);
  if (!sock) {
    return NULL;
  }

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
  ph_sock_enable(sock, false);
  ph_job_free(&sock->job);
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

  calc_elapsed(rac);

  if (sock) {
    rac->func(sock, PH_SOCK_CONNECT_SUCCESS, 0, addr, &rac->elapsed, rac->arg);
  } else {
    rac->func(NULL, PH_SOCK_CONNECT_ERRNO, status, addr, &rac->elapsed, rac->arg);
  }

  free_rac(rac);
}

static void attempt_connect(struct resolve_and_connect *rac)
{
  rac->s = ph_socket_for_addr(&rac->addr, SOCK_STREAM,
      PH_SOCK_CLOEXEC|PH_SOCK_NONBLOCK);

  if (rac->s == -1) {
    calc_elapsed(rac);
    rac->func(NULL, PH_SOCK_CONNECT_ERRNO, errno, &rac->addr, &rac->elapsed, rac->arg);
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

static void resolve_ares(void *arg, int status, int timeouts, struct hostent *hostent)
{
  struct resolve_and_connect *rac = arg;

  ph_unused_parameter(timeouts);

  if (status != ARES_SUCCESS) {
    calc_elapsed(rac);
    rac->func(NULL, PH_SOCK_CONNECT_ARES_ERR, status,
        NULL, &rac->elapsed, rac->arg);
    free_rac(rac);
    return;
  }

  ph_sockaddr_set_from_hostent(&rac->addr, hostent);
  ph_sockaddr_set_port(&rac->addr, rac->port);
  attempt_connect(rac);
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
    case PH_SOCK_CONNECT_RESOLVE_ARES:
      break;
    default:
      func(NULL, PH_SOCK_CONNECT_GAI_ERR, EAI_NONAME, NULL, &tv, arg);
      return;
  }

  pthread_once(&done_sock_init, do_sock_init);

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
      if (ph_dns_getaddrinfo(name, portstr, NULL, did_sys_resolve, rac) == PH_OK) {
        return;
      }
      calc_elapsed(rac);
      func(NULL, PH_SOCK_CONNECT_ERRNO, errno, NULL, &rac->elapsed, arg);
      free_rac(rac);
      break;

    case PH_SOCK_CONNECT_RESOLVE_ARES:
      ph_dns_channel_gethostbyname(NULL, name, AF_UNSPEC, resolve_ares, rac);
  }
}

ph_buf_t *ph_sock_read_bytes_exact(ph_sock_t *sock, uint64_t len)
{
  return ph_bufq_consume_bytes(sock->rbuf, len);
}

ph_buf_t *ph_sock_read_record(ph_sock_t *sock, const char *delim, uint32_t delim_len)
{
  return ph_bufq_consume_record(sock->rbuf, delim, delim_len);
}

ph_buf_t *ph_sock_read_line(ph_sock_t *sock)
{
  return ph_bufq_consume_record(sock->rbuf, "\r\n", 2);
}


/* vim:ts=2:sw=2:et:
 */

