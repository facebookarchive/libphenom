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
#include "phenom/socket.h"
#include "phenom/feature_test.h"
#include <netdb.h>

#ifdef PH_HAVE_ARES
#include <ares.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * # DNS Resolution facilities
 *
 * libPhenom provides two sets of DNS resolution facilities.
 *
 * The first is an asynchronous wrapper around the system resolver that
 * delegates to the system provided getaddrinfo() library function but
 * runs in a thread pool.
 *
 * This is provided for the common case of resolving a target hostname
 * and then setting up an async connect.
 *
 * The second set of functions wrap around the c-ares library and are
 * intended to be used when you need to interrogate MX or SRV records.
 *
 * Both sets of resolver functions operate in the same way: they
 * schedule a resolution and arrange to invoke a callback when the
 * results are available.  The same callback is invoked in both
 * the success and error cases.  The callback may be triggered on
 * the same thread that scheduled the resolution, but the common
 * case is for the resolution to complete on some other thread and
 * invoke the callback in that context.
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

/** Release async addrinfo results */
void ph_dns_addrinfo_free(ph_dns_addrinfo_t *info);

struct ph_dns_channel;
typedef struct ph_dns_channel ph_dns_channel_t;

#ifdef PH_HAVE_ARES
/** Create a new DNS resolving channel
 *
 * The channel encapsulates a c-ares
 * eventloop and manages its queries.
 */
ph_dns_channel_t *ph_dns_channel_create(void);

typedef void (*ph_dns_channel_raw_query_func)(void *arg,
    int status,
    int timeouts,
    unsigned char *abuf,
    unsigned int alen);

/** Represents an individual answer to an A, AAAA, MX or SRV query */
struct ph_dns_query_response_answer {
  // For A and AAAA responses, the address with no port set
  ph_sockaddr_t addr;
  // For A and AAAA responses, the name that was queried,
  // for MX and SRV responses, the name of the MX or SRV entry
  char *name;
  // For MX and SRV records, the priority
  uint16_t priority;
  // For SRV records, the relative weight and the port
  uint16_t weight, port;
  // For A and AAAA records, the TTL for this entry
  int ttl;
};

/** Represents the answers to an A, AAAA, MX or SRV query.
 *
 * This is a variable sized structure and must be freed using
 * ph_dns_query_response_free().
 *
 * MX and SRV responses are sorted per priority order.
 */
struct ph_dns_query_response {
  char *name;
  uint16_t num_answers;
  struct ph_dns_query_response_answer answer[1];
};

/** Free a DNS query response */
void ph_dns_query_response_free(struct ph_dns_query_response *resp);

// Called to handle DNS query completion */
typedef void (*ph_dns_channel_query_func)(
    void *arg,
    int status,
    int timeouts,
    unsigned char *abuf,
    unsigned int alen,
    struct ph_dns_query_response *resp);

#define PH_DNS_QUERY_NONE 0
#define PH_DNS_QUERY_A    1
#define PH_DNS_QUERY_AAAA 2
#define PH_DNS_QUERY_SRV  3
#ifdef PH_HAVE_ARES_MX
# define PH_DNS_QUERY_MX   4
#endif

/** Schedule a DNS query
 *
 * The query will be presented against `chan`.  If chan is NULL, then
 * a default channel that consults the local resolv.conf will be used.
 *
 * The success/failure status of the query is communicated solely to
 * the callback function, which will be executed either in the currently
 * running thread for error conditions that are detected immediately,
 * or in the context of an NBIO thread in the case that we're resolving
 * the query (either successfuly or with error).
 *
 * The callback will be executed under a lock that will block other
 * DNS queries against the channel.  It is recommended that your callback
 * do the minimum amount of work to parse/copy the result and schedule
 * follow-up work.
 *
 * It is safe to schedule additional queries from the callback.
 *
 * The `query_type` parameter specifies how the result is parsed and
 * filled out in the `resp` parameter of your callback; valid values
 * are: `PH_DNS_QUERY_A`, `PH_DNS_QUERY_AAAA`, `PH_DNS_QUERY_SRV` or
 * `PH_DNS_QUERY_MX` to select `A`, `AAAA`, `SRV` or `MX` records
 * respectively.
 */
void ph_dns_channel_query(
    ph_dns_channel_t *chan,
    const char *name,
    int query_type,
    ph_dns_channel_query_func func,
    void *arg);

/** Schedule a raw DNS query
 *
 * The query will be presented against `chan`.  If chan is NULL, then
 * a default channel that consults the local resolv.conf will be used.
 *
 * The success/failure status of the query is communicated solely to
 * the callback function, which will be executed either in the currently
 * running thread for error conditions that are detected immediately,
 * or in the context of an NBIO thread in the case that we're resolving
 * the query (either successfuly or with error).
 *
 * The callback will be executed under a lock that will block other
 * DNS queries against the channel.  It is recommended that your callback
 * do the minimum amount of work to parse/copy the result and schedule
 * follow-up work.
 *
 * It is safe to schedule additional queries from the callback.
 *
 * This is the raw interface that facilitates parsing of any DNS response.
 * If you need this functionality, you will also need to include `ares.h`
 * and use its DNS response parsing functions, or provide your own.
 *
 * You probably want to use ph_dns_channel_query() instead of this function.
 */
void ph_dns_channel_query_raw(
    ph_dns_channel_t *chan,
    const char *name,
    int dnsclass,
    int type,
    ph_dns_channel_raw_query_func func,
    void *arg);

/** Perform a hostname resolve using ares
 *
 * initiates a host query by name on the name service channel identified by
 * channel. The parameter name gives the hostname as a NUL-terminated C
 * string, and family gives the desired type of address for the resulting host
 * entry use `AF_UNSPEC` to express no preference, `AF_INET` for IPv4 or `AF_INET6` for
 * IPv6.
 *
 * When the query is complete or has failed, the callback will be invoked.
 *
 * The callback has the form:
 *
 * ```
 * typedef void (*ares_host_callback)(void *arg, int status, int timeouts,
 *    struct hostent *hostent);
 * ```
 *
 * The hostent is owned by the library and will be freed once the callback returns.
 * */

void ph_dns_channel_gethostbyname(
    ph_dns_channel_t *chan,
    const char *name,
    int family,
    ares_host_callback func,
    void *arg);
#endif // PH_HAVE_ARES


#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

