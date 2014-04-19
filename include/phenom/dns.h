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

#ifndef PHENOM_DNS_H
#define PHENOM_DNS_H

#include "phenom/job.h"
#include "phenom/socket.h"
#include "phenom/feature_test.h"
#include <netdb.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * # DNS Resolution facilities
 *
 * libPhenom provides an asynchronous wrapper around the system resolver that
 * delegates to the system provided getaddrinfo() library function but
 * runs in a thread pool.
 *
 * This is provided for the common case of resolving a target hostname
 * and then setting up an async connect.
 *
 * The resolver functions operate by schedling a resolution and arrange to
 * invoke a callback when the results are available.  The same callback is
 * invoked in both the success and error cases.  The callback may be triggered
 * on the same thread that scheduled the resolution, but the common case is for
 * the resolution to complete on some other thread and invoke the callback in
 * that context.
 *
 * The intended usage is that you schedule a job to continue your
 * processing in some other context.
 */

/* */

struct ph_dns_addrinfo;
typedef struct ph_dns_addrinfo ph_dns_addrinfo_t;
typedef void (*ph_dns_addrinfo_func)(ph_dns_addrinfo_t *info);

/** Holds the results of an async getaddrinfo call */
struct ph_dns_addrinfo {
  // The job used to schedule the call
  ph_job_t job;
  // The node being queried
  char *node;
  // The service or port string
  char *service;
  // The hints that were provided
  struct addrinfo hints;
  // Holds the result of getaddrinfo()
  int result;
  // Holds the addrinfo returned from getaddrinfo().
  // Do not pass this to freeaddrinfo(), instead, pass the entire
  // structure to ph_dns_addrinfo_free() to release its resources
  struct addrinfo *ai;
  // Your arg pointer
  void *arg;
  // Your callback function
  ph_dns_addrinfo_func func;
};

/** Initiate an async getaddrinfo(3) call
 * The call is dispatched by a DNS thread pool, which is started alongside
 * the scheduler. When it completes, your ph_dns_addrinfo_func function is
 * invoked in the context of that thread.  You own the resultant ph_dns_addrinfo
 * struct and must call ph_dns_addrinfo_free() to release its resources when you
 * are done with them.
 *
 * Arguments are the same as getaddrinfo(3), with the addition of `func`
 * and `arg`.  `func` is called after getaddrinfo() completes.  `arg`
 * is copied into the ph_dns_addrinfo struct and allows you to pass through
 * some context.
 *
 * The expected usage is to schedule some additional work, such as setting
 * up a job to connect to the resolved host.
 */
ph_result_t ph_dns_getaddrinfo(const char *node, const char *service,
    const struct addrinfo *hints, ph_dns_addrinfo_func func, void *arg);

/** Release async addrinfo results */
void ph_dns_addrinfo_free(ph_dns_addrinfo_t *info);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

