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


#ifndef PHENOM_SOCKET_H
#define PHENOM_SOCKET_H

#include "phenom/defs.h"
#include "phenom/job.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "phenom/string.h"
#include "phenom/buffer.h"
#include "phenom/openssl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Represents a socket address */
struct phenom_sockaddr {
  sa_family_t family;
  union {
    struct sockaddr sa;
    struct sockaddr_un nix;
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
  } sa;
};
typedef struct phenom_sockaddr ph_sockaddr_t;

/** Returns the length of the underlying sockaddr
 *
 * This is useful when passing the sockaddr to lower level
 * socket syscalls
 */
static inline int ph_sockaddr_socklen(const ph_sockaddr_t *addr) {
  switch (addr->family) {
    case AF_UNIX:
      return sizeof(addr->sa.nix);
    case AF_INET:
      return sizeof(addr->sa.v4);
    case AF_INET6:
      return sizeof(addr->sa.v6);
    default:
      return sizeof(addr->sa.sa);
  }
}

/** Set a sockaddr to the specified IPv4 address string and port.
 * The address string must be an IPv4 address.  This function
 * does *not* perform DNS resolution.  It uses inet_pton() under
 * the covers.
 *
 * If addr == NULL, then sa is initialized to INADDR_ANY
 * */
ph_result_t ph_sockaddr_set_v4(ph_sockaddr_t *sa,
    const char *addr, uint16_t port);

/** Set a sockaddr to the specified IPv6 address string and port.
 * The address string must be an IPv6 address.  This function
 * does *not* perform DNS resolution.  It uses getaddrinfo() under
 * the covers, with AI_NUMERICHOST|AI_V4MAPPED as flags.
 * The sockaddr may be set to a v4 mapped address depending on
 * the configuration of the system.
 *
 * If addr == NULL, then sa is initialized to in6addr_any
 */
ph_result_t ph_sockaddr_set_v6(
    ph_sockaddr_t *sa,
    const char *addr,
    uint16_t port);

/** Set a sockaddr to the specified UNIX domain address.
 * The address string must be a valid UNIX domain socket path.
 * This function does not support the Linux specific abstract
 * namespace feature, and only supports paths that fit directly
 * in sa->sa.nix.sun_path.
 *
 * If pathlen == 0, strlen(path) will be assumed.
 */
ph_result_t ph_sockaddr_set_unix(
    ph_sockaddr_t *addr,
    const char *path,
    unsigned int pathlen);

/** Set a sockaddr to the first entry from an addrinfo */
ph_result_t ph_sockaddr_set_from_addrinfo(
    ph_sockaddr_t *addr,
    struct addrinfo *ai);

/** Set a sockaddr to the first address from a hostent */
ph_result_t ph_sockaddr_set_from_hostent(
    ph_sockaddr_t *addr,
    struct hostent *ent);

/** Set the port number of a sockaddr */
void ph_sockaddr_set_port(ph_sockaddr_t *addr, uint16_t port);

/** Print a human readable version of a sockaddr to a string */
ph_result_t ph_sockaddr_print(ph_sockaddr_t *addr,
    ph_string_t *str, bool want_port);

/** Set or disable non-blocking mode for a file descriptor */
void ph_socket_set_nonblock(ph_socket_t fd, bool enable);

#define PH_SOCK_CLOEXEC  1
#define PH_SOCK_NONBLOCK 2

/** Creates a socket that can be used to connect to a sockaddr
 *
 * The socket is created of the appropriate type and the flags
 * are applied:
 *
 * * `PH_SOCK_CLOEXEC` causes the socket to have CLOEXEC set
 * * `PH_SOCK_NONBLOCK` causes the socket to be set to nonblocking mode
 *
 *```
 * ph_socket_t s = ph_socket_for_addr(&addr, SOCK_STREAM,
 *                    PH_SOCK_CLOEXEC|PH_SOCK_NONBLOCK);
 *```
 */
ph_socket_t ph_socket_for_addr(const ph_sockaddr_t *addr, int type, int flags);

/** Handles the results of ph_socket_connect()
 *
 * If successful, `s != -1` and `status == 0`.
 *
 * `addr` points to the address that we attempted to connect.
 *
 * `status` holds the errno value from the connect syscall.
 *
 * `elapsed` holds the amount of time that elapsed since the connect
 * call was started.
 *
 * `arg` passes through the `arg` parameter from ph_socket_connect().
 */
typedef void (*ph_socket_connect_func)(
    ph_socket_t s, const ph_sockaddr_t *addr, int status,
    struct timeval *elapsed, void *arg);

/** Initiate an async connect() call
 *
 * The results will be delivered to your connect func.
 * In some immediate failure cases, this will be called before
 * ph_socket_connect has returned, but in the common case, this
 * will happen asynchronously from an NBIO thread once the connect
 * call has resolved.
 *
 * The `timeout` parameter allows you to specify an upper bound on
 * the time spent waiting for the connect.  If `timeout` is NULL
 * a implementation dependent default timeout will be used.
 */
void ph_socket_connect(ph_socket_t s, const ph_sockaddr_t *addr,
  struct timeval *timeout, ph_socket_connect_func func, void *arg);

struct ph_sock;
typedef struct ph_sock ph_sock_t;

/** The socket object callback function */
typedef void (*ph_sock_func)(ph_sock_t *sock, ph_iomask_t why, void *data);

/** Called when we complete a handshake */
typedef void (*ph_sock_openssl_handshake_func)(
    ph_sock_t *sock, int res);

/** Socket Object
 *
 * A socket object is a higher level representation of an underlying
 * socket descriptor.
 *
 * It is the preferred way to build higher level socket clients and
 * servers, as it takes the boilerplate of managing read/write buffers
 * and async dispatch away from you.
 *
 * A socket object is either enabled or disabled; when enabled, the
 * underlying descriptor is managed by the NBIO pool and any pending
 * write data will be sent as and when it is ready to go.  Any pending
 * reads will trigger a wakup and you can use the sock functions to
 * read chunks or delimited records (such as lines).
 *
 * If your client/server needs to perform some blocking work, you may
 * simply disable the sock until that work is complete.
 */
struct ph_sock {
  // Embedded job so we can participate in NBIO
  ph_job_t job;

  // Buffers for output, input
  ph_bufq_t *wbuf, *rbuf;

  // The per IO operation timeout duration
  struct timeval timeout_duration;

  // A stream for writing to the underlying connection
  ph_stream_t *conn;
  // A stream representation of myself.  Writing bytes into the
  // stream causes the data to be buffered in wbuf
  ph_stream_t *stream;

  // Dispatcher
  ph_sock_func callback;
  bool enabled;

  // sockname, peername as seen from this host.
  // These correspond to the raw connection we see; if we are
  // proxied, these are the names of our connection to the proxy.
  // If we are not proxied, these are the same as the equivalents below
  ph_sockaddr_t via_sockname, via_peername;
  // sockname, peername as seen from the connected peer
  // These are the actual outgoing address endpoints, independent of
  // any proxying that may be employed
  ph_sockaddr_t sockname, peername;

  // If we've switched up to SSL, holds our SSL context
  SSL *ssl;
  ph_stream_t *ssl_stream;
  ph_sock_openssl_handshake_func handshake_cb;
  ph_bufq_t *sslwbuf;
};

/** Create a new sock object from a socket descriptor
 *
 * Creates and initialize a socket object using the specified descriptor,
 * sockname and peername.
 */
ph_sock_t *ph_sock_new_from_socket(ph_socket_t s, const ph_sockaddr_t *sockname,
  const ph_sockaddr_t *peername);

/** Enable or disable IO dispatching for a socket object
 *
 * While enabled, the sock will trigger callbacks when it is readable/writable
 * or timed out.  While disabled, none of these conditions will trigger.
 */
void ph_sock_enable(ph_sock_t *sock, bool enable);

/** Wakeup the socket object
 *
 * Queues an PH_IOMASK_WAKEUP to the sock.  This is primarily useful in cases
 * where some asynchronous processing has completed and you wish to ping the
 * sock job so that it can consume the results.
 *
 * Delegates to ph_job_wakeup() and can fail for the same reasons as that
 * function.
 */
ph_result_t ph_sock_wakeup(ph_sock_t *sock);

/** Release all resources associated with a socket object
 *
 * Implicitly disables the socket.
 */
void ph_sock_free(ph_sock_t *sock);

/** Read exactly the specified number of bytes
 *
 * Returns a buffer containing the requested number of bytes, or NULL if they
 * are not available.
 *
 * Never returns a partial read.
 */
ph_buf_t *ph_sock_read_bytes_exact(ph_sock_t *sock, uint64_t len);


/** Read a delimited record
 *
 * Search for the delimiter in the buffer; if found, returns a buffer containing
 * the record and its delimiter text.
 */
ph_buf_t *ph_sock_read_record(ph_sock_t *sock, const char *delim,
    uint32_t delim_len);

/** Read a CRLF delimited line
 *
 * Search for the canonical CRLF in the buffer.  If found, returns a buffer
 * containing the line and its CRLF delimiter.
 */
ph_buf_t *ph_sock_read_line(ph_sock_t *sock);

#define PH_SOCK_CONNECT_RESOLVE_SYSTEM 0
#define PH_SOCK_CONNECT_RESOLVE_ARES   1

// Succeeded
#define PH_SOCK_CONNECT_SUCCESS  0
// Failed; errcode per gai_strerror
#define PH_SOCK_CONNECT_GAI_ERR  1
// Failed; errcode per ares_strerror
#define PH_SOCK_CONNECT_ARES_ERR 2
// Failed; errcode per strerror
#define PH_SOCK_CONNECT_ERRNO    3

/** Indicates the results of an async sock connect
 *
 * If successful, `sock` will be non-NULL and `overall_status` will be set
 * to `PH_SOCK_CONNECT_SUCCESS`.
 *
 * On failure `overall_status` will be set to one of `PH_SOCK_CONNECT_GAI_ERR`,
 * `PH_SOCK_CONNECT_ARES_ERR` or `PH_SOCK_CONNECT_ERRNO` to indicate how to
 * interpret the `errcode` parameter; they indicate that the errcode can be
 * rendered to human readable form via gai_strerror(), ares_strerror() or strerror()
 * respectively.
 *
 * `addr` may be NULL.  If it is not NULL, it holds the address that we attempted
 * to connect to.  It may be set if we didn't successfully connect.
 *
 * `elapsed` holds the amount of time that has elapsed since we attempted to initiate
 * the connection.
 *
 * `arg` passes through the arg parameter from ph_sock_resolve_and_connect().
 */
typedef void (*ph_sock_connect_func)(
    ph_sock_t *sock, int overall_status, int errcode,
    const ph_sockaddr_t *addr,
    struct timeval *elapsed, void *arg);

/** Given a name and port, resolve and connect a socket object to it
 *
 * This convenience function resolves the name using the specified resolver
 * (`PH_SOCK_CONNECT_RESOLVE_SYSTEM` for getaddrinfo, `PH_SOCK_CONNECT_RESOLVE_ARES`
 * for the ares resolver) and attempts to connect to the first address resolved.
 *
 * Success or failure is communicated to your ph_sock_connect_func.
 *
 * `timeout` specifies an upper bound on the connection attempt.  If left as NULL,
 * a default value will be used.
 */
void ph_sock_resolve_and_connect(const char *name, uint16_t port,
    struct timeval *timeout, int resolver, ph_sock_connect_func func,
    void *arg);

#define PH_SOCK_SHUT_RD   0
#define PH_SOCK_SHUT_WR   1
#define PH_SOCK_SHUT_RDWR 2

/** Perform a shutdown operation on a socket object */
static inline int ph_sock_shutdown(ph_sock_t *sock, int how) {
  return shutdown(sock->job.fd, how);
}

/** Enable SSL for the sock
 *
 * Configures the sock to act as an SSL client or server.
 *
 * This will amend the struct to reference SSL variants of the
 * streams and set the session to act as an SSL client or server,
 * depending on the value of the `is_client` parameter.
 *
 * This puts the socket into a pending connect or accept state;
 * the socket will work to satisfy this state before dispatching
 * any further calls to the sock callback function.
 *
 * Once the handshake is complete, the dispatcher will call
 * handshake_cb with the result of the completed SSL_do_handshake()
 * function call.
 *
 * You may use this opportunity to perform additional validation
 * of the session.
 *
 * You must supply the SSL object for use by this function and ensure
 * that it is correctly configured (certificates and keys loaded, ciphers
 * selected and so on).
 */
void ph_sock_openssl_enable(ph_sock_t *sock, SSL *ssl,
    bool is_client, ph_sock_openssl_handshake_func handshake_cb);


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

