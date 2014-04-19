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

#include "phenom/sysutil.h"
#include "phenom/string.h"
#include "phenom/dns.h"
#include "phenom/log.h"
#include "tap.h"

static struct addr_test {
  const char *node;
  const char *service;
  const char *expect;
  int result;
} addr_tests[] = {
  { "a.test.phenom.wezfurlong.org", "80", "[127.0.0.4]:80", 0 },
  { "aaaa.test.phenom.wezfurlong.org", "80", "[::a:a:a:a]:80", 0 },
  { "bbbb.test.phenom.wezfurlong.org", "80", NULL, EAI_NONAME },
};

static uint32_t num_left = 0;
static pthread_mutex_t single = PTHREAD_MUTEX_INITIALIZER;

static void check_addrinfo_result(ph_dns_addrinfo_t *info)
{
  struct addr_test *t = info->arg;
  ph_sockaddr_t sa;
  PH_STRING_DECLARE_STACK(str, 128);

  pthread_mutex_lock(&single);

  is(info->result, t->result);
  if (info->result) {
    ok(1, "%s: expected fail: '%s' got '%s'",
        t->node,
        gai_strerror(t->result), gai_strerror(info->result));
  } else {
    ph_sockaddr_set_from_addrinfo(&sa, info->ai);
    ph_sockaddr_print(&sa, &str, true);
    ok(ph_string_equal_cstr(&str, t->expect),
        "%s -> %s", t->node, t->expect);
  }

  if (--num_left == 0) {
    ph_sched_stop();
  }

  pthread_mutex_unlock(&single);

  ph_dns_addrinfo_free(info);
}

int main(int argc, char **argv)
{
  uint8_t i;

  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  ph_nbio_init(0);
  plan_tests(2 * (sizeof(addr_tests)/sizeof(addr_tests[0])));

  for (i = 0; i < sizeof(addr_tests)/sizeof(addr_tests[0]); i++) {
    ph_dns_getaddrinfo(addr_tests[i].node, addr_tests[i].service,
        NULL, check_addrinfo_result, &addr_tests[i]);
    ck_pr_inc_32(&num_left);
  }

  ph_sched_run();

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */
