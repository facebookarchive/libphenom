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
#include "tap.h"

int main(int argc, char **argv)
{
  char buf[128];
  char verify[128];
  int len;
  unused_parameter(argc);
  unused_parameter(argv);

  plan_tests(7);

  len = phenom_snprintf(buf, 10, "12345678901");
  // Returns the length required
  ok(len == 11, "snprintf gave %d", len);
  // But is constrained to the size we indicated
  ok(!strcmp(buf, "123456789"), "produced %s", buf);

  memset(buf, 0, sizeof(buf));
  len = phenom_snprintf(buf, sizeof(buf), "double:%.1f", 1.2);
  ok(!strcmp(buf, "double:1.2"), "produced %d %s", len, buf);

  /* check the error formatter */
  phenom_snprintf(buf, sizeof(buf), "`Pe%d:%d", EAGAIN, EAGAIN);
  snprintf(verify, sizeof(verify), "%s:%d", strerror(EAGAIN), EAGAIN);
  ok(!strcmp(buf, verify), "expected %s got %s", verify, buf);

  /* check invalid backtick escape */
  phenom_snprintf(buf, sizeof(buf), "`boo");
  ok(!strcmp(buf, "`boo"), "got %s", buf);

  /* check backtick escape */
  phenom_snprintf(buf, sizeof(buf), "``boo");
  ok(!strcmp(buf, "`boo"), "got %s", buf);

  phenom_snprintf(buf, sizeof(buf), "``Pboo");
  ok(!strcmp(buf, "`Pboo"), "got %s", buf);

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

