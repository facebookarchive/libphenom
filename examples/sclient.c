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

/* This builds a very simple SSL client program.
 * It connects to the specified SSL host using TCP, then connects
 * stdin to the remote; anything on stdin is encrypted and sent to the
 * remote, and anything from the remote is decrypted and printed to
 * stdout.  This is a little bit like `openssl s_client` but with far
 * fewer features.
 */

#include "phenom/defs.h"
#include "phenom/configuration.h"
#include "phenom/job.h"
#include "phenom/log.h"
#include "phenom/sysutil.h"
#include "phenom/printf.h"
#include "phenom/listener.h"
#include "phenom/socket.h"
#include <sysexits.h>
#include <unistd.h>
#include <stdlib.h>

struct timeval timeout = { 60, 0 };
uint16_t portno = 443;
char *addrstring = NULL;
ph_job_t stdin_job;
ph_sock_t *remote_sock;

static void read_stdin(ph_job_t *job, ph_iomask_t why, void *data)
{
  char buf[128];
  int x, i;

  ph_unused_parameter(why);
  ph_unused_parameter(data);

  x = read(job->fd, buf, sizeof(buf));
  if (x <= 0) {
    if (x == -1) {
      ph_log(PH_LOG_ERR, "read(stdin): `Pe%d", errno);
    }
    ph_sched_stop();
    return;
  }

  // Writing to the other job is safe here because we have the
  // same affinity: we know that it is not executing and mutating
  // its state
  for (i = 0; i < x; i++) {
    if (buf[i] == '\n') {
      ph_stm_write(remote_sock->stream, "\r\n", 2, NULL);
    } else {
      ph_stm_write(remote_sock->stream, buf + i, 1, NULL);
    }
  }

  // Force the sock to wakeup and send the buffer.
  ph_sock_wakeup(remote_sock);

  ph_job_set_nbio_timeout_in(job, PH_IOMASK_READ, timeout);
}

static void done_handshake(ph_sock_t *sock, int res)
{
  ph_unused_parameter(sock);
  ph_log(PH_LOG_ERR, "handshake completed with res=%d", res);
}

static void read_remote(ph_sock_t *sock, ph_iomask_t why, void *data)
{
  ph_buf_t *buf;

  ph_unused_parameter(data);

  if (why & (PH_IOMASK_TIME|PH_IOMASK_ERR)) {
    ph_log(PH_LOG_ERR, "disconnecting `P{sockaddr:%p}",
        (void*)&sock->peername);
    ph_sock_shutdown(sock, PH_SOCK_SHUT_RDWR);
    ph_sock_free(sock);
    remote_sock = NULL;
    ph_sched_stop();
    return;
  }

  while (1) {
    buf = ph_sock_read_line(sock);
    if (!buf) {
      // Not available yet, we'll try again later
      return;
    }

    ph_ignore_result(write(STDOUT_FILENO, ph_buf_mem(buf), ph_buf_len(buf)));
    ph_buf_delref(buf);
  }
}

static void connected(ph_sock_t *sock, int overall_status,
    int errcode, const ph_sockaddr_t *addr,
    struct timeval *elapsed, void *arg)
{
  SSL_CTX *ctx;
  SSL *ssl;

  ph_unused_parameter(arg);
  ph_unused_parameter(elapsed);

  switch (overall_status) {
    case PH_SOCK_CONNECT_GAI_ERR:
      ph_log(PH_LOG_ERR, "resolve %s:%d failed %s",
          addrstring, portno, gai_strerror(errcode));
      ph_sched_stop();
      return;

    case PH_SOCK_CONNECT_ERRNO:
      ph_log(PH_LOG_ERR, "connect %s:%d (`P{sockaddr:%p}) failed: `Pe%d",
          addrstring, portno, (void*)addr, errcode);
      ph_sched_stop();
      return;
  }

  sock->callback = read_remote;
  remote_sock = sock;

  // Now set up stdin to feed into this new sock
  ph_job_init(&stdin_job);
  stdin_job.fd = STDIN_FILENO;
  stdin_job.callback = read_stdin;
  // Ensure that we have the same affinity as the other job
  stdin_job.emitter_affinity = sock->job.emitter_affinity;
  ph_socket_set_nonblock(STDIN_FILENO, true);
  ph_job_set_nbio_timeout_in(&stdin_job, PH_IOMASK_READ, timeout);

  ctx = SSL_CTX_new(SSLv23_client_method());
  SSL_CTX_set_cipher_list(ctx, "ALL");
  SSL_CTX_set_options(ctx, SSL_OP_ALL);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  ssl = SSL_new(ctx);
  ph_sock_openssl_enable(sock, ssl, true, done_handshake);

  ph_sock_enable(sock, true);
}

static void usage(void)
{
  ph_fdprintf(STDERR_FILENO,
      " -h HOST     - which address to connect\n"
      " -p PORTNO   - which port to connect\n"
      );
  exit(EX_USAGE);
}

int main(int argc, char **argv)
{
  int c;

  // Must be called prior to calling any other phenom functions
  ph_library_init();
  // Initialize OpenSSL and make it safe for multi-threaded use
  ph_library_init_openssl();

  while ((c = getopt(argc, argv, "p:h:")) != -1) {
    switch (c) {
      case 'h':
        addrstring = optarg;
        break;
      case 'p':
        portno = atoi(optarg);
        break;
      default:
        usage();
    }
  }

  if (!addrstring) {
    usage();
  }

  ph_nbio_init(0);
  ph_sock_resolve_and_connect(addrstring, portno,
      NULL, PH_SOCK_CONNECT_RESOLVE_SYSTEM,
      connected, NULL);

  ph_debug_console_start("/tmp/phenom-debug-console");

  // Run
  ph_sched_run();

  return 0;
}

/* vim:ts=2:sw=2:et:
 */

