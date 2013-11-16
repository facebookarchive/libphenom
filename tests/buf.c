/*
 * Copyright 2013-present Facebook, Inc.
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
#include "phenom/buffer.h"
#include "tap.h"

static void test_straddle_edges(void)
{
  const char *delim = "\r\n";
  int delim_len = strlen(delim);
  int default_buf_size = 8192;
  int i;
  char pad[8192];

  memset(pad, 'x', sizeof(pad));
#define PAD_IT(__n) { \
  uint64_t n = __n; \
  while (n > 0) { \
    ph_bufq_append(q, pad, MIN(n, sizeof(pad)), 0); \
    n -= MIN(n, sizeof(pad)); \
  } \
}

  // We want two buffers: [8192][8192]
  // And then to place our delimiter around the first boundary to verify
  // that the delimiter matching operates correctly
  // We define a cursor offset relative to the end of the first buffer
  // (0 means the last byte of the delimiter is in the last byte of the
  // first buffer, 1 means that the last delimiter byte is in the first
  // byte of the second buffer)

  for (i = - 2 * delim_len; i < 2 * delim_len; i++) {
    ph_bufq_t *q;

    q = ph_bufq_new(16*1024);

    // Fill up the start of the buffer
    uint64_t num_first = default_buf_size + i - delim_len;

    // first data
    PAD_IT(num_first);
    is(num_first, ph_bufq_len(q));
    // first delim
    ph_bufq_append(q, delim, delim_len, 0);

    // second data
    PAD_IT(16);
    // second delim
    ph_bufq_append(q, delim, delim_len, 0);

    ph_buf_t *first = ph_bufq_consume_record(q, delim, delim_len);
    is_int(num_first + 2, ph_buf_len(first));

    ph_buf_t *second = ph_bufq_consume_record(q, delim, delim_len);
    is_int(18, ph_buf_len(second));

    diag("for i = %d, num_first = %d.  first->len=%d second->len=%d",
        i, (int)num_first, (int)ph_buf_len(first), (int)ph_buf_len(second));

    ph_buf_delref(first);
    ph_buf_delref(second);

    ph_bufq_free(q);
  }

  // Now, test the case where we have a partial match at a boundary, but not
  // the true match until later

  ph_bufq_t *q;

  q = ph_bufq_new(24*1024);
  PAD_IT(8191);
  ph_bufq_append(q, "\r", 1, 0);
  PAD_IT(8192);
  ph_bufq_append(q, delim, delim_len, 0);

  ph_buf_t *first = ph_bufq_consume_record(q, delim, delim_len);
  is_int(16386, ph_buf_len(first));

  ph_buf_delref(first);
  ph_bufq_free(q);
}

int main(int argc, char **argv)
{
  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  plan_tests(25);

  test_straddle_edges();

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

