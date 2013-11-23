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

#include "phenom/dns.h"
#include "phenom/hashtable.h"
#include "phenom/log.h"

#ifdef PH_HAVE_ARES
#include <ares_dns.h>
#include <arpa/nameser.h>

struct ph_dns_channel {
  ares_channel chan;
  pthread_mutex_t chanlock;
  ph_ht_t sock_map;
};

struct ph_dns_query {
  ph_dns_channel_t *chan;
  char *name;
  int qtype;
  void *arg;
  union {
    ph_dns_channel_raw_query_func raw;
    ph_dns_channel_query_func func;
  } func;
};

static struct {
  ph_memtype_t chan, query, job, string, aresp;
} mt;
static ph_memtype_def_t defs[] = {
  { "ares", "channel", sizeof(struct ph_dns_channel), PH_MEM_FLAGS_ZERO },
  { "ares", "query", sizeof(struct ph_dns_query), PH_MEM_FLAGS_ZERO },
  { "ares", "socket", sizeof(ph_job_t), PH_MEM_FLAGS_ZERO },
  { "ares", "string", 0, 0 },
  { "ares", "query_response", 0, PH_MEM_FLAGS_ZERO },
};

static ph_dns_channel_t *default_channel = NULL;
static void process_ares(ph_job_t *job, ph_iomask_t why, void *data);
static struct ph_job_def ares_job_template = {
  process_ares,
  PH_MEMTYPE_INVALID,
  NULL
};

static inline void apply_mask(ph_dns_channel_t *chan, ph_job_t *job,
    ph_iomask_t mask)
{
  struct timeval maxtv = {60, 0}, tv, *tvp;
  tvp = ares_timeout(chan->chan, &maxtv, &tv);
  ph_job_set_nbio_timeout_in(job, mask, *tvp);
}

// Called when an ARES socket is ready
static void process_ares(ph_job_t *job, ph_iomask_t why, void *data)
{
  ph_dns_channel_t *chan = data;
  ares_socket_t fd;
  ares_socket_t rfd = ARES_SOCKET_BAD;
  ares_socket_t wfd = ARES_SOCKET_BAD;

  fd = job->fd;

  if (why & PH_IOMASK_READ) {
    rfd = job->fd;
  }
  if (why & PH_IOMASK_WRITE) {
    wfd = job->fd;
  }

  pthread_mutex_lock(&chan->chanlock);
  ares_process_fd(chan->chan, rfd, wfd);
  if (ph_ht_lookup(&chan->sock_map, &fd, &job, false) == PH_OK &&
      ph_job_get_kmask(job) == 0) {
    // Didn't delete it, but didn't reschedule it, so do that now
    apply_mask(chan, job,
        job->mask ? job->mask : PH_IOMASK_READ);
  }
  pthread_mutex_unlock(&chan->chanlock);
}

static uint32_t hash_sock(const void *key) {
  return (uint32_t)*(ares_socket_t*)key;
}

static const struct ph_ht_key_def sock_key = {
  sizeof(ares_socket_t),
  hash_sock,
  NULL,
  NULL,
  NULL
};

/* called when ares wants to change the event mask */
static void sock_state_cb(void *data, ares_socket_t socket_fd,
                          int readable, int writable)
{
  ph_dns_channel_t *chan = data;
  ph_job_t *job;
  ph_iomask_t mask = 0;

  if (readable) {
    mask |= PH_IOMASK_READ;
  }
  if (writable) {
    mask |= PH_IOMASK_WRITE;
  }

  if (ph_ht_lookup(&chan->sock_map, &socket_fd, &job, false) != PH_OK) {
    ph_panic("job for socket %d was not found in ares sock_state_cb",
        socket_fd);
  }

  if (mask) {
    apply_mask(chan, job, mask);
  } else {
    ph_job_set_nbio(job, 0, NULL);
    // We're done with this guy, remove it
    ph_ht_del(&chan->sock_map, &socket_fd);
    ph_job_free(job);
  }
}

/* called when ares creates a new socket */
static int sock_create_cb(ares_socket_t socket_fd, int type, void *data)
{
  ph_dns_channel_t *chan = data;
  ph_job_t *job = ph_job_alloc(&ares_job_template);

  ph_unused_parameter(type);

  if (!job) {
    return ARES_ENOMEM;
  }

  job->data = chan;
  job->fd = socket_fd;

  if (ph_ht_set(&chan->sock_map, &socket_fd, &job) != PH_OK) {
    ph_mem_free(mt.job, job);
    return ARES_ENOMEM;
  }

  return ARES_SUCCESS;
}

static ph_dns_channel_t *create_chan(void)
{
  ph_dns_channel_t *chan;
  struct ares_options opts;
  int res;
  pthread_mutexattr_t attr;

  chan = ph_mem_alloc(mt.chan);
  if (!chan) {
    return NULL;
  }

  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&chan->chanlock, &attr);
  pthread_mutexattr_destroy(&attr);

  if (ph_ht_init(&chan->sock_map, 4, &sock_key, &ph_ht_ptr_val_def) != PH_OK) {
    ph_panic("failed to init sock map");
  }

  memset(&opts, 0, sizeof(opts));
  opts.sock_state_cb_data = chan;
  opts.sock_state_cb = sock_state_cb;
  opts.flags = ARES_FLAG_STAYOPEN;

  res = ares_init_options(&chan->chan, &opts,
      ARES_OPT_SOCK_STATE_CB|ARES_OPT_FLAGS);

  if (res != ARES_SUCCESS) {
    ph_panic("failed to ares_init_options: %s", ares_strerror(res));
  }

  ares_set_socket_callback(chan->chan, sock_create_cb, chan);

  return chan;
}

static void free_chan(ph_dns_channel_t *chan)
{
  ares_destroy(chan->chan);
  ph_ht_destroy(&chan->sock_map);
  pthread_mutex_destroy(&chan->chanlock);
  ph_mem_free(mt.chan, chan);
}

static void do_ares_fini(void)
{
  free_chan(default_channel);
}

static void do_ares_init(void)
{
  int res = ares_library_init(ARES_LIB_INIT_ALL);

  if (res) {
    ph_panic("ares_library_init failed: %s", ares_strerror(res));
  }

  ph_memtype_register_block(sizeof(defs)/sizeof(defs[0]), defs, &mt.chan);

  // This must be the last thing we do in this function
  default_channel = create_chan();
  if (!default_channel) {
    ph_panic("failed to create default DNS channel");
  }

  ares_job_template.memtype = mt.job;
}

PH_LIBRARY_INIT(do_ares_init, do_ares_fini)

ph_dns_channel_t *ph_dns_channel_create(void)
{
  return create_chan();
}

void ph_dns_query_response_free(struct ph_dns_query_response *resp)
{
  int i;

  for (i = 0; i < resp->num_answers; i++) {
    if (resp->answer[i].name != resp->name) {
      ph_mem_free(mt.string, resp->answer[i].name);
    }
  }

  ph_mem_free(mt.aresp, resp);
}

static struct ph_dns_query_response *make_a_resp(unsigned char *abuf, int alen)
{
  int ancount = DNS_HEADER_ANCOUNT(abuf);
  struct ph_dns_query_response *resp;
  uint32_t size;
  struct ares_addrttl *ttls = NULL;
  int res;
  struct hostent *host;
  int nttls;
  int i;
  char *name;

  ttls = malloc(ancount * sizeof(*ttls));
  if (!ttls) {
    return NULL;
  }
  nttls = ancount;

  res = ares_parse_a_reply(abuf, alen, &host, ttls, &nttls);
  if (res) {
    free(ttls);
    return NULL;
  }

  size = sizeof(*resp) + ((nttls - 1) * sizeof(resp->answer[0]));
  resp = ph_mem_alloc_size(mt.aresp, size + strlen(host->h_name) + 1);
  if (!resp) {
    free(ttls);
    return NULL;
  }
  // this is where we'll stash the name
  name = ((char*)resp) + size;
  strcpy(name, host->h_name); // NOLINT(runtime/printf)

  resp->num_answers = ancount;
  resp->name = name;

  for (i = 0; i < nttls; i++) {
    resp->answer[i].name = name;
    resp->answer[i].ttl = ttls[i].ttl;
    resp->answer[i].addr.family = AF_INET;
    resp->answer[i].addr.sa.v4.sin_family = AF_INET;
    resp->answer[i].addr.sa.v4.sin_addr = ttls[i].ipaddr;
  }

  free(ttls);
  ares_free_hostent(host);

  return resp;
}

static struct ph_dns_query_response *make_aaaa_resp(unsigned char *abuf,
    int alen)
{
  int ancount = DNS_HEADER_ANCOUNT(abuf);
  struct ph_dns_query_response *resp;
  uint32_t size;
  struct ares_addr6ttl *ttls = NULL;
  int res;
  struct hostent *host;
  int nttls;
  int i;
  char *name;

  ttls = malloc(ancount * sizeof(*ttls));
  if (!ttls) {
    return NULL;
  }
  nttls = ancount;

  res = ares_parse_aaaa_reply(abuf, alen, &host, ttls, &nttls);
  if (res) {
    free(ttls);
    return NULL;
  }

  size = sizeof(*resp) + ((nttls - 1) * sizeof(resp->answer[0]));
  resp = ph_mem_alloc_size(mt.aresp, size + strlen(host->h_name) + 1);
  if (!resp) {
    free(ttls);
    return NULL;
  }
  // this is where we'll stash the name
  name = ((char*)resp) + size;
  strcpy(name, host->h_name); // NOLINT(runtime/printf)

  resp->num_answers = ancount;
  resp->name = name;

  for (i = 0; i < nttls; i++) {
    resp->answer[i].name = name;
    resp->answer[i].ttl = ttls[i].ttl;
    resp->answer[i].addr.family = AF_INET6;
    resp->answer[i].addr.sa.v6.sin6_family = AF_INET6;
    memcpy(&resp->answer[i].addr.sa.v6.sin6_addr, &ttls[i].ip6addr,
        sizeof(ttls[i].ip6addr));
  }

  free(ttls);
  ares_free_hostent(host);

  return resp;
}

static int compare_mx_ent(const void *aptr, const void *bptr)
{
  struct ph_dns_query_response_answer *a = (void*)aptr;
  struct ph_dns_query_response_answer *b = (void*)bptr;

  if (a->priority == b->priority) {
    return strcmp(a->name, b->name);
  }
  return a->priority - b->priority;
}

static struct ph_dns_query_response *make_srv_resp(unsigned char *abuf,
    int alen)
{
  struct ph_dns_query_response *resp;
  uint32_t size;
  struct ares_srv_reply *srv, *tmp;
  uint32_t nres = 0, i;

  if (ares_parse_srv_reply(abuf, alen, &srv) != ARES_SUCCESS) {
    return NULL;
  }

  for (nres = 0, tmp = srv; tmp; tmp = tmp->next) {
    nres++;
  }

  size = sizeof(*resp) + ((nres - 1) * sizeof(resp->answer[0]));
  resp = ph_mem_alloc_size(mt.aresp, size);
  if (!resp) {
    ares_free_data(srv);
    return NULL;
  }

  resp->num_answers = nres;

  for (tmp = srv, i = 0; tmp; tmp = tmp->next, i++) {
    resp->answer[i].name = ph_mem_strdup(mt.string, tmp->host);
    resp->answer[i].priority = tmp->priority;
    resp->answer[i].weight = tmp->weight;
    resp->answer[i].port = tmp->port;
  }

  // Sort in priority order
  qsort(resp->answer, nres, sizeof(resp->answer[0]), compare_mx_ent);

  ares_free_data(srv);

  return resp;
}

#ifdef PH_HAVE_ARES_MX
static struct ph_dns_query_response *make_mx_resp(unsigned char *abuf, int alen)
{
  struct ph_dns_query_response *resp;
  uint32_t size;
  struct ares_mx_reply *mx, *tmp;
  uint32_t nmxs = 0, i;

  if (ares_parse_mx_reply(abuf, alen, &mx) != ARES_SUCCESS) {
    return NULL;
  }

  for (nmxs = 0, tmp = mx; tmp; tmp = tmp->next) {
    nmxs++;
  }

  size = sizeof(*resp) + ((nmxs - 1) * sizeof(resp->answer[0]));
  resp = ph_mem_alloc_size(mt.aresp, size);
  if (!resp) {
    ares_free_data(mx);
    return NULL;
  }

  resp->num_answers = nmxs;

  for (tmp = mx, i = 0; tmp; tmp = tmp->next, i++) {
    resp->answer[i].name = ph_mem_strdup(mt.string, tmp->host);
    resp->answer[i].priority = tmp->priority;
  }

  // Sort in priority order
  qsort(resp->answer, nmxs, sizeof(resp->answer[0]), compare_mx_ent);

  ares_free_data(mx);

  return resp;
}
#endif

static void result_cb(void *arg, int status, int timeouts,
  unsigned char *abuf, int alen)
{
  struct ph_dns_query *q = arg;
  struct ph_dns_query_response *resp = NULL;

  switch (q->qtype) {
    case PH_DNS_QUERY_NONE:
      q->func.raw(q->arg, status, timeouts, abuf, alen);
      break;

#ifdef PH_HAVE_ARES_MX
    case PH_DNS_QUERY_MX:
      if (status == ARES_SUCCESS) {
        resp = make_mx_resp(abuf, alen);
      }
      q->func.func(q->arg, status, timeouts, abuf, alen, resp);
      break;
#endif

    case PH_DNS_QUERY_A:
      if (status == ARES_SUCCESS) {
        resp = make_a_resp(abuf, alen);
      }
      q->func.func(q->arg, status, timeouts, abuf, alen, resp);
      break;

    case PH_DNS_QUERY_SRV:
      if (status == ARES_SUCCESS) {
        resp = make_srv_resp(abuf, alen);
      }
      q->func.func(q->arg, status, timeouts, abuf, alen, resp);
      break;

    case PH_DNS_QUERY_AAAA:
      if (status == ARES_SUCCESS) {
        resp = make_aaaa_resp(abuf, alen);
      }
      q->func.func(q->arg, status, timeouts, abuf, alen, resp);
      break;

    default:
      ph_panic("invalid qtype %d", q->qtype);
  }

  ph_mem_free(mt.query, q);
}

static inline ph_dns_channel_t *fixup_chan(ph_dns_channel_t *chan)
{
  if (!chan) {
    chan = default_channel;
  }
  return chan;
}

void ph_dns_channel_query_raw(
    ph_dns_channel_t *chan,
    const char *name,
    int dnsclass,
    int type,
    ph_dns_channel_raw_query_func func,
    void *arg)
{
  struct ph_dns_query *q;

  chan = fixup_chan(chan);

  q = ph_mem_alloc(mt.query);
  if (!q) {
    func(arg, ARES_ENOMEM, 0, NULL, 0);
    return;
  }

  q->chan = chan;
  q->arg = arg;
  q->qtype = PH_DNS_QUERY_NONE;
  q->func.raw = func;

  pthread_mutex_lock(&chan->chanlock);
  ares_search(chan->chan, name, dnsclass, type, result_cb, q);
  pthread_mutex_unlock(&chan->chanlock);
}

void ph_dns_channel_query(
    ph_dns_channel_t *chan,
    const char *name,
    int query_type,
    ph_dns_channel_query_func func,
    void *arg)
{
  struct ph_dns_query *q;
  int dnsclass = ns_c_in, type;

  chan = fixup_chan(chan);

  q = ph_mem_alloc(mt.query);
  if (!q) {
    func(arg, ARES_ENOMEM, 0, NULL, 0, NULL);
    return;
  }

  q->chan = chan;
  q->arg = arg;
  q->qtype = query_type;
  q->func.func = func;

  switch (query_type) {
    case PH_DNS_QUERY_A:
      type = ns_t_a;
      break;
    case PH_DNS_QUERY_AAAA:
      type = ns_t_aaaa;
      break;
    case PH_DNS_QUERY_SRV:
      type = ns_t_srv;
      break;
#ifdef PH_HAVE_ARES_MX
    case PH_DNS_QUERY_MX:
      type = ns_t_mx;
      break;
#endif
    default:
      ph_panic("invalid query type %d", query_type);
  }

  pthread_mutex_lock(&chan->chanlock);
  ares_search(chan->chan, name, dnsclass, type, result_cb, q);
  pthread_mutex_unlock(&chan->chanlock);
}

void ph_dns_channel_gethostbyname(
    ph_dns_channel_t *chan,
    const char *name,
    int family,
    ares_host_callback func,
    void *arg)
{
  chan = fixup_chan(chan);
  pthread_mutex_lock(&chan->chanlock);
  ares_gethostbyname(chan->chan, name, family, func, arg);
  pthread_mutex_unlock(&chan->chanlock);
}

#endif // PH_HAVE_ARES

/* vim:ts=2:sw=2:et:
 */

