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

#ifndef PHENOM_DNS_H
#define PHENOM_DNS_H

#include "phenom/job.h"
#include <netdb.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ph_dns_addrinfo;
typedef struct ph_dns_addrinfo ph_dns_addrinfo_t;
typedef void (*ph_dns_addrinfo_func)(ph_dns_addrinfo_t *info);

struct ph_dns_addrinfo {
  ph_job_t job;
  char *node;
  char *service;
  struct addrinfo hints;
  int result;
  struct addrinfo *ai;
  void *arg;
  ph_dns_addrinfo_func func;
};

/** Initiate an async getaddrinfo(3) call
 * The call is dispatched by a DNS thread pool.  When it completes,
 * your ph_dns_addrinfo_func function is invoked in the context of that
 * thread.  You own the resultant ph_dns_addrinfo struct and must call
 * ph_dns_addrinfo_free() to release its resources when you are done
 * with them.
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

void ph_dns_addrinfo_free(ph_dns_addrinfo_t *info);


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

