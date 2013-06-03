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
#include "phenom/stream.h"
#include "tap.h"

int main(int argc, char **argv)
{
  ph_stream_t *stm;
  char namebuf[128];
  int fd;
  char buf[BUFSIZ];
  uint64_t amount;
  int len;

  unused_parameter(argc);
  unused_parameter(argv);

  plan_tests(19);

  is(PH_OK, ph_stm_init());
  strcpy(namebuf, "/tmp/phenomXXXXXX");
  fd = ph_mkostemp(namebuf, 0);
  diag("opened %s -> %d", namebuf, fd);
  unlink(namebuf);

  stm = ph_stm_fd_open(fd, 0, PH_STM_BUFSIZE);
  ph_stm_write(stm, "lemon\n", 6, &amount);
  is(amount, 6);

  // Shouldn't see it yet
  is(0, pread(fd, buf, sizeof(buf), 0));

  // Should see it now
  ph_stm_flush(stm);
  is(6, pread(fd, buf, sizeof(buf), 0));

  ok(!memcmp("lemon\n", buf, 6), "right content");
  ok(ph_stm_seek(stm, 0, SEEK_SET, NULL), "seeked");
  memset(buf, 0, sizeof(buf));

  ok(ph_stm_read(stm, buf, 3, &amount), "read ok");
  ok(amount == 3, "amount is %" PRIu64, amount);
  ok(!memcmp("lem", buf, 3), "got prefix");

  ok(ph_stm_read(stm, buf, 3, &amount), "read ok");
  ok(amount == 3, "amount is %" PRIu64, amount);
  ok(!memcmp("on\n", buf, 3), "got remainder");

  ok(ph_stm_seek(stm, 0, SEEK_SET, NULL), "seeked");
  len = ph_stm_printf(stm, "testing %d %s!", 1, "two");
  ok(14 == len, "printed len %d", len);
  ok(ph_stm_seek(stm, 0, SEEK_SET, NULL), "seeked");
  memset(buf, 0, sizeof(buf));
  ok(ph_stm_read(stm, buf, 14, &amount), "read ok");
  ok(amount == 14, "len was %" PRIu64, amount);
  ok(!memcmp("testing 1 two!", buf, 14), "got formatted");

  ok(ph_stm_close(stm), "closed");

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

