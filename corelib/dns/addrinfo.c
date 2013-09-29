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
#include "phenom/memory.h"

static struct {
  ph_memtype_t ainfo, string;
} mt;
static ph_memtype_def_t defs[] = {
  { "dns", "addrinfo", sizeof(ph_dns_addrinfo_t), PH_MEM_FLAGS_ZERO },
  { "dns", "string", 0, 0 },
};

static ph_thread_pool_t *dns_pool = NULL;

static void do_free_addrinfo(ph_job_t *job)
{
  ph_dns_addrinfo_t *info = (ph_dns_addrinfo_t*)job;

  if (info->node) {
    ph_mem_free(mt.string, info->node);
  }
  if (info->service) {
    ph_mem_free(mt.string, info->service);
  }
  if (info->ai) {
    freeaddrinfo(info->ai);
  }
}

void ph_dns_addrinfo_free(ph_dns_addrinfo_t *info)
{
  ph_job_free(&info->job);
}

static void dns_addrinfo(ph_job_t *job, ph_iomask_t why, void *data)
{
  ph_dns_addrinfo_t *info = data;

  ph_unused_parameter(job);
  ph_unused_parameter(why);

  info->result = getaddrinfo(info->node, info->service,
      &info->hints, &info->ai);

  info->func(info);
}

static struct ph_job_def addrinfo_job_def = {
  dns_addrinfo,
  PH_MEMTYPE_INVALID,
  do_free_addrinfo
};

static void do_dns_init(void)
{
  ph_memtype_register_block(sizeof(defs)/sizeof(defs[0]), defs, &mt.ainfo);
  addrinfo_job_def.memtype = mt.ainfo;
  dns_pool = ph_thread_pool_define("dns", 256, 2);
}

PH_LIBRARY_INIT(do_dns_init, 0)

ph_result_t ph_dns_getaddrinfo(const char *node, const char *service,
    const struct addrinfo *hints, ph_dns_addrinfo_func func, void *arg)
{
  ph_dns_addrinfo_t *info;

  if (!func) {
    return PH_ERR;
  }

  info = (ph_dns_addrinfo_t*)ph_job_alloc(&addrinfo_job_def);
  if (!info) {
    return PH_NOMEM;
  }

  if (node) {
    info->node = ph_mem_strdup(mt.string, node);
    if (!info->node) {
      goto fail;
    }
  }

  if (service) {
    info->service = ph_mem_strdup(mt.string, service);
    if (!info->service) {
      goto fail;
    }
  }

  if (hints) {
    info->hints = *hints;
  } else {
    info->hints.ai_family = AF_UNSPEC;
  }


  info->arg = arg;
  info->func = func;

  info->job.callback = dns_addrinfo;
  info->job.data = info;

  ph_job_set_pool(&info->job, dns_pool);

  return PH_OK;

fail:
  ph_dns_addrinfo_free(info);

  return PH_NOMEM;
}

/* vim:ts=2:sw=2:et:
 */

