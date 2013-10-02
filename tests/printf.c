/*
 * Copyright 2012 Facebook, Inc.
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
#include "phenom/printf.h"
#include "tap.h"

static int test_va_list(char *buf, size_t len, const char *fmt, ...)
{
  va_list ap;
  int ret;

  va_start(ap, fmt);
  ret = ph_snprintf(buf, len, "prefix: %s `Pv%s%p suffix: %s",
          "PREFIX", fmt, ph_vaptr(ap), "SUFFIX");
  va_end(ap);
  return ret;
}

int main(int argc, char **argv)
{
  char buf[128];
  char verify[128];
  char bigbuf[132];
  int len;
  char *strp;
  int i;

  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  plan_tests(16);

  len = ph_snprintf(buf, 10, "12345678901");
  // Returns the length required
  ok(len == 11, "snprintf gave %d", len);
  // But is constrained to the size we indicated
  ok(!strcmp(buf, "123456789"), "produced %s", buf);

  memset(buf, 0, sizeof(buf));
  len = ph_snprintf(buf, sizeof(buf), "double:%.1f", 1.2);
  ok(!strcmp(buf, "double:1.2"), "produced %d %s", len, buf);

  /* check the error formatter */
  ph_snprintf(buf, sizeof(buf), "`Pe%d:%d", EAGAIN, EAGAIN);
  snprintf(verify, sizeof(verify), "%s:%d", strerror(EAGAIN), EAGAIN);
  ok(!strcmp(buf, verify), "expected %s got %s", verify, buf);

  /* check invalid backtick escape */
  ph_snprintf(buf, sizeof(buf), "`boo");
  ok(!strcmp(buf, "`boo"), "got %s", buf);

  /* check backtick escape */
  ph_snprintf(buf, sizeof(buf), "``boo");
  ok(!strcmp(buf, "`boo"), "got %s", buf);

  ph_snprintf(buf, sizeof(buf), "``Pboo");
  ok(!strcmp(buf, "`Pboo"), "got %s", buf);

  ph_snprintf(buf, sizeof(buf), "`P{not-registered:%p}", NULL);
  ok(!strcmp(buf, "INVALID:not-registered"), "got %s", buf);

  len = test_va_list(buf, sizeof(buf), "inside %d %d", 42, 1);

#define EXPECTED "prefix: PREFIX inside 42 1 suffix: SUFFIX"
  ok(!strcmp(buf, EXPECTED), "got %s", buf);
  ok(len == (int)strlen(buf), "got len=%d buflen=%zd expected=%zd",
      len, strlen(buf), strlen(EXPECTED));
  ok(len == (int)strlen(EXPECTED), "got len=%d", len);


  len = ph_asprintf(&strp, "testing %s", "basic");
  is(len, 13);
  is_string(strp, "testing basic");
  free(strp);

  /* test buffer boundary for realloc case */
  for (i = 0; i < (int)sizeof(bigbuf) - 1; i++) {
    bigbuf[i] = i + 1;
  }
  bigbuf[sizeof(bigbuf)-1] = '\0';
  len = ph_asprintf(&strp, "prefix: %s", bigbuf);
  is(len, 8 + sizeof(bigbuf) - 1);
  ok(!strncmp(strp, "prefix: ", 8), "got prefix %.*s", 8, strp);
  ok(!strcmp(strp + 8, bigbuf), "bigbuf compared ok");
  free(strp);


  /* uncomment this to check against your system supplied
   * snprintf.  Note that some snprintfs have broken return
   * values! */
#if 0
  len = snprintf(buf, 10, "12345678901");
  ok(len == 11, "snprintf gave %d", len);
  ok(!strcmp(buf, "123456789"), "produced %s", buf);
#endif


  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

