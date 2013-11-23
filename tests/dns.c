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

#ifdef PH_HAVE_ARES

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

static void lookup_aaaa(
    void *arg,
    int status,
    int timeouts,
    unsigned char *abuf,
    unsigned int alen,
    struct ph_dns_query_response *resp)
{
  PH_STRING_DECLARE_STACK(str, 128);

  ph_unused_parameter(timeouts);
  ph_unused_parameter(arg);
  ph_unused_parameter(abuf);
  ph_unused_parameter(alen);

  pthread_mutex_lock(&single);
  ok(ARES_SUCCESS == status, "expected aaaa lookup to succeed: %s", ares_strerror(status));

  is_string("aaaa.test.phenom.wezfurlong.org", resp->name);
  ph_sockaddr_print(&resp->answer[0].addr, &str, false);
  ok(ph_string_equal_cstr(&str, "::a:a:a:a"), "right addr");
  ph_dns_query_response_free(resp);

  if (--num_left == 0) {
    ph_sched_stop();
  }
  pthread_mutex_unlock(&single);
}

static void lookup_mx(
    void *arg,
    int status,
    int timeouts,
    unsigned char *abuf,
    unsigned int alen,
    struct ph_dns_query_response *resp)
{
  ph_unused_parameter(timeouts);
  ph_unused_parameter(arg);
  ph_unused_parameter(abuf);
  ph_unused_parameter(alen);

  pthread_mutex_lock(&single);
  ok(ARES_SUCCESS == status, "expected mx lookup to succeed: %s", ares_strerror(status));

  is_int(resp->num_answers, 2);
  is_string("a.test.phenom.wezfurlong.org", resp->answer[0].name);
  is_int(10, resp->answer[0].priority);
  is_string("b.test.phenom.wezfurlong.org", resp->answer[1].name);
  is_int(20, resp->answer[1].priority);
  ph_dns_query_response_free(resp);

  if (--num_left == 0) {
    ph_sched_stop();
  }
  pthread_mutex_unlock(&single);
}

static void lookup_a(
    void *arg,
    int status,
    int timeouts,
    unsigned char *abuf,
    unsigned int alen,
    struct ph_dns_query_response *resp)
{
  PH_STRING_DECLARE_STACK(str, 128);

  ph_unused_parameter(timeouts);
  ph_unused_parameter(arg);
  ph_unused_parameter(abuf);
  ph_unused_parameter(alen);

  pthread_mutex_lock(&single);
  ok(ARES_SUCCESS == status, "expected a lookup to succeed: %s", ares_strerror(status));

  is_string("a.test.phenom.wezfurlong.org", resp->name);
  ph_sockaddr_print(&resp->answer[0].addr, &str, false);
  ok(ph_string_equal_cstr(&str, "127.0.0.4"), "right addr");

  ph_dns_query_response_free(resp);

  if (--num_left == 0) {
    ph_sched_stop();
  }
  pthread_mutex_unlock(&single);
}

int main(int argc, char **argv)
{
  uint8_t i;

  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  ph_nbio_init(0);
  plan_tests(
      (2 * (sizeof(addr_tests)/sizeof(addr_tests[0]))) +
      (2 * 3) /* A and AAAA */
#ifdef PH_HAVE_ARES_MX
      + (6) /* MX */
#endif
  );

  ck_pr_inc_32(&num_left);
  ph_dns_channel_query(NULL, "a.test.phenom.wezfurlong.org.",
      PH_DNS_QUERY_A, lookup_a, NULL);

  ck_pr_inc_32(&num_left);
  ph_dns_channel_query(NULL, "aaaa.test.phenom.wezfurlong.org.",
      PH_DNS_QUERY_AAAA, lookup_aaaa, NULL);

#ifdef PH_HAVE_ARES_MX
  ck_pr_inc_32(&num_left);
  ph_dns_channel_query(NULL, "mx1.test.phenom.wezfurlong.org.",
      PH_DNS_QUERY_MX, lookup_mx, NULL);
#endif

  for (i = 0; i < sizeof(addr_tests)/sizeof(addr_tests[0]); i++) {
    ph_dns_getaddrinfo(addr_tests[i].node, addr_tests[i].service,
        NULL, check_addrinfo_result, &addr_tests[i]);
    ck_pr_inc_32(&num_left);
  }

  ph_sched_run();

  return exit_status();
}

#else

int main(int argc, char **argv)
{
  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  plan_tests(1);
  ok(1, "no ares support");
  return exit_status();
}

#endif


/* vim:ts=2:sw=2:et:
 */
