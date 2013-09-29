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

#ifndef PHENOM_LISTENER_H
#define PHENOM_LISTENER_H

#include "phenom/socket.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ph_listener;
typedef struct ph_listener ph_listener_t;

typedef void (*ph_listener_accept_func)(
    ph_listener_t *listener, ph_sock_t *sock);

struct ph_listener {
  // Embedded job so we can participate in NBIO
  ph_job_t job;

  // When accepting, we default to setting the next
  // emitter in a round robin fashion.  This holds
  // our state
  uint32_t emitter_affinity;

  // Local address (to which we are bound)
  ph_sockaddr_t addr;

  // `PH_SOCK_CLOEXEC`, `PH_SOCK_NONBLOCK`
  int flags;

  bool enabled;
  bool listening;

  // Desired backlog
  int backlog;

  // Called as new connections are accepted
  ph_listener_accept_func acceptor;

  // Name: used for accounting
  char name[64];
};

/** Create a new listener */
ph_listener_t *ph_listener_new(const char *name,
    ph_listener_accept_func acceptor);

/** Bind a listener to a local address
 *
 * If no socket has been associated, associates one prior to calling bind(),
 * and enables address/port reuse socket options
 */
ph_result_t ph_listener_bind(ph_listener_t *lstn, const ph_sockaddr_t *addr);

/** Returns the underlying socket descriptor for a listener */
ph_socket_t ph_listener_get_fd(ph_listener_t *lstn);

/** Set the listen backlog.
 *
 * If `backlog` is <= 0, attempts to determine the current kernel setting
 * for the `somaxconn` system parameter and uses that, otherwise falls back
 * to the `SOMAXCONN` value from your system header, or if that is undefined,
 * uses the value `128`.
 */
void ph_listener_set_backlog(ph_listener_t *lstn, int backlog);

/** Enable/Disable the listener
 *
 * When enabled for the first time, calls listen(2) with the backlog that you
 * set.  If you have not set a backlog, a default value will be computed per
 * the description in ph_listener_set_backlog().
 *
 * While enabled, the acceptor function will be invoked from any NBIO thread
 * each time a client connection is accepted.
 */
void ph_listener_enable(ph_listener_t *lstn, bool enable);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

