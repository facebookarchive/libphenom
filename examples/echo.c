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


/* Implements an echo server.
 * It reads CRLF separated lines and repeats them back to you with a prefix
 * of "you said: "
 * We also demonstrate how to associate state with the session by tracking
 * the count of lines that were read against it.
 *
 * In one terminal:
 * `./examples/echo`
 *
 * In another:
 * `telnet ::1 8080`
 * and type in text.
 * You can use CTRL-] and then type `quit` to terminate the telnet session.
 *
 * If you're using the `-s` option; it enables SSL for the listener.
 * This code expects to be run from the root of the repo so that it can
 * find the `examples/server.pem` file.
 *
 * You can then connect to the server using `./examples/sclient -h ::1 -p 8080`
 */

static bool enable_ssl = false;
static SSL_CTX *ssl_ctx = NULL;

// Each connected session will have one of these structs associated with it.
// We don't really do anything useful with it here, it's just to show how
// to associate data with a session
struct echo_state {
  int num_lines;
};

// We'll track our state instances using our own typed memory.
// Use the debug console to inspect it; this is useful to figure out if
// or where you might be leaking memory
static ph_memtype_t mt_state;
static struct ph_memtype_def mt_state_def = {
  "example", "echo_state", sizeof(struct echo_state), PH_MEM_FLAGS_ZERO
};

// Called each time the session wakes up.
// The `why` parameter indicates why we were woken up
static void echo_processor(ph_sock_t *sock, ph_iomask_t why, void *arg)
{
  struct echo_state *state = arg;
  ph_buf_t *buf;

  // If the socket encountered an error, or if the timeout was reached
  // (there's a default timeout, even if we didn't override it), then
  // we tear down the session
  if (why & (PH_IOMASK_ERR|PH_IOMASK_TIME)) {
    ph_log(PH_LOG_ERR, "disconnecting `P{sockaddr:%p}", (void*)&sock->peername);
    ph_sock_shutdown(sock, PH_SOCK_SHUT_RDWR);
    ph_mem_free(mt_state, state);
    ph_sock_free(sock);
    return;
  }

  // We loop because echo_processor is only triggered by newly arriving
  // data or events from the kernel.  If we have data buffered and only
  // partially consume it, we won't get woken up until the next data
  // arrives, if ever.
  while (1) {
    // Try to read a line of text.
    // This returns a slice over the underlying buffer (if the line was
    // smaller than a buffer) or a freshly made contiguous buffer (if the
    // line was larger than our buffer segment size).  Either way, we
    // own a reference to the returned buffer and should treat it as
    // a read-only slice.
    buf = ph_sock_read_line(sock);
    if (!buf) {
      // Not available yet, we'll try again later
      return;
    }

    // We got a line; update our state
    state->num_lines++;

    // Send our response.  The data is buffered and automatically sent
    // to the client as it becomes writable, so we don't need to handle
    // partial writes or EAGAIN here.

    // If this was a "real" server, we would still check the return value
    // from the writes and proceed to tear down the session if things failed.

    // Note that buf includes the trailing CRLF, so our response
    // will implicitly end with CRLF too.
    ph_stm_printf(sock->stream, "You said [%d]: ", state->num_lines);
    ph_stm_write(sock->stream, ph_buf_mem(buf), ph_buf_len(buf), NULL);

    // We're done with buf, so we must release it
    ph_buf_delref(buf);
  }
}

static void done_handshake(ph_sock_t *sock, int res)
{
  ph_unused_parameter(sock);
  ph_log(PH_LOG_ERR, "handshake completed with res=%d", res);
}

// Called each time the listener has accepted a client connection
static void acceptor(ph_listener_t *lstn, ph_sock_t *sock)
{
  ph_unused_parameter(lstn);

  // Allocate an echo_state instance and stash it.
  // This is set to be zero'd on creation and will show up as the
  // `arg` parameter in `echo_processor`
  sock->job.data = ph_mem_alloc(mt_state);

  // Tell it how to dispatch
  sock->callback = echo_processor;

  ph_log(PH_LOG_ERR, "accepted `P{sockaddr:%p}", (void*)&sock->peername);

  if (enable_ssl) {
    SSL *ssl;

    // Tell the sock that we're using a global SSL_CTX
    sock->free_ssl_ctx = false;

    ssl = SSL_new(ssl_ctx);
    ph_sock_openssl_enable(sock, ssl, false, done_handshake);
  }

  ph_sock_enable(sock, true);
}

int main(int argc, char **argv)
{
  int c;
  uint16_t portno = 8080;
  char *addrstring = NULL;
  ph_sockaddr_t addr;
  ph_listener_t *lstn;
  bool use_v4 = false;

  // Must be called prior to calling any other phenom functions
  ph_library_init();

  while ((c = getopt(argc, argv, "p:l:4s")) != -1) {
    switch (c) {
      case '4':
        use_v4 = true;
        break;
      case 's':
        enable_ssl = true;
        break;
      case 'l':
        addrstring = optarg;
        break;
      case 'p':
        portno = atoi(optarg);
        break;
      default:
        ph_fdprintf(STDERR_FILENO,
            "Invalid parameters\n"
            " -4          - interpret address as an IPv4 address\n"
            " -l ADDRESS  - which address to listen on\n"
            " -p PORTNO   - which port to listen on\n"
            " -s          - enable SSL\n"
        );
        exit(EX_USAGE);
    }
  }

  if (enable_ssl) {
    ph_library_init_openssl();

    ssl_ctx = SSL_CTX_new(SSLv23_server_method());
    SSL_CTX_set_cipher_list(ssl_ctx, "ALL");
    SSL_CTX_use_RSAPrivateKey_file(ssl_ctx, "examples/server.pem", SSL_FILETYPE_PEM);
    SSL_CTX_use_certificate_file(ssl_ctx, "examples/server.pem", SSL_FILETYPE_PEM);
    SSL_CTX_set_options(ssl_ctx, SSL_OP_ALL);
  }

  // Set up the address that we're going to listen on
  if ((use_v4 && ph_sockaddr_set_v4(&addr, addrstring, portno) != PH_OK) ||
      (!use_v4 && ph_sockaddr_set_v6(&addr, addrstring, portno) != PH_OK)) {
    ph_fdprintf(STDERR_FILENO,
        "Invalid address [%s]:%d",
        addrstring ? addrstring : "*",
        portno
    );
    exit(EX_USAGE);
  }

  // Register our memtype
  mt_state = ph_memtype_register(&mt_state_def);

  // Optional config file for tuning internals
  ph_config_load_config_file("/path/to/my/config.json");

  // Enable the non-blocking IO manager
  ph_nbio_init(0);

  ph_log(PH_LOG_ERR, "will listen on `P{sockaddr:%p}", (void*)&addr);

  // This enables a very simple request/response console
  // that allows you to run diagnostic commands:
  // `echo memory | nc -UC /tmp/phenom-debug-console`
  // (on BSD systems, use `nc -Uc`!)
  // The code behind this is in
  // https://github.com/facebook/libphenom/blob/master/corelib/debug_console.c
  ph_debug_console_start("/tmp/phenom-debug-console");

  lstn = ph_listener_new("echo-server", acceptor);
  ph_listener_bind(lstn, &addr);
  ph_listener_enable(lstn, true);

  // Run
  ph_sched_run();

  return 0;
}

/* vim:ts=2:sw=2:et:
 */

