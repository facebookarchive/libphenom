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


#ifndef PHENOM_SOCKET_H
#define PHENOM_SOCKET_H

#include "phenom/defs.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "phenom/string.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Holds a socket descriptor */
typedef int ph_socket_t;

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

static inline int ph_sockaddr_socklen(ph_sockaddr_t *addr) {
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


ph_result_t ph_sockaddr_set_from_addrinfo(
    ph_sockaddr_t *addr,
    struct addrinfo *ai);

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
ph_socket_t ph_socket_for_addr(ph_sockaddr_t *addr, int type, int flags);

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



#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

