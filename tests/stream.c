/*
 * Copyright 2012-present Facebook, Inc.
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
#include "phenom/printf.h"
#include "tap.h"
#include "sys/stat.h"

int main(int argc, char **argv)
{
  ph_stream_t *stm;
  char namebuf[128];
  int fd;
  char buf[BUFSIZ];
  uint64_t amount;
  int len;

  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  plan_tests(33);

  strcpy(namebuf, "/tmp/phenomXXXXXX");
  fd = ph_mkostemp(namebuf, 0);
  diag("opened %s -> %d", namebuf, fd);

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

  ok(ph_stm_rewind(stm), "rewound");
  ph_ignore_result(ftruncate(fd, 0));

  ph_stream_t *src = ph_stm_file_open("aclocal.m4", O_RDONLY, 0);
  ok(src, "opened aclocal.m4");

  struct stat st;
  stat("aclocal.m4", &st);

  ok(src->rpos == NULL, "nothing in the read buffer");
  ok(ph_stm_readahead(src, PH_STM_BUFSIZE), "readahead ok");
  ok(src->rpos == src->buf, "looks like we got data in the buffer");
  ok(src->rend == src->rpos + PH_STM_BUFSIZE, "filled up to bufsize");

  // do it again to prove that it doesn't screw things up
  ok(ph_stm_readahead(src, PH_STM_BUFSIZE), "2nd readahead ok");
  ok(src->rpos == src->buf, "data in buffer still ok");
  ok(src->rend == src->rpos + PH_STM_BUFSIZE, "data in buffer same size");

  uint64_t nwrote, nread;
  ok(ph_stm_copy(src, stm, PH_STREAM_READ_ALL, &nread, &nwrote), "copy data");
  is_int(nread, nwrote);
  is_int(nread, (uint64_t)st.st_size);
  is(nwrote, (uint64_t)st.st_size);

  ph_stm_flush(stm);
  char cmd[1024];
  ph_snprintf(cmd, sizeof(cmd), "cmp -l aclocal.m4 %s", namebuf);
  int res = system(cmd);
  ok(res == 0, "cmp on file data shows no differences: res=%d", res);

  ok(ph_stm_close(stm), "closed");
  ok(ph_stm_close(src), "closed");

  unlink(namebuf);

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

