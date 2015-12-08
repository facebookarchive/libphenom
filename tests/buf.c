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
#include "phenom/printf.h"
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

// Build a buffer with contents that vary such that we stand
// a decent chance of recognizing corruption in this test case
static char *build_big_buf(uint32_t big_buf_size)
{
  char *big_buf;
  uint32_t x, i, j;

  big_buf = malloc(big_buf_size);
  i = 0;
  x = 0;
  while (i < big_buf_size) {
    big_buf[i++] = x++;

    for (j = 0; i < big_buf_size && j < 8191; j++) {
      big_buf[i++] = j;
    }
  }

  return big_buf;
}

struct drain_check {
  const char *buf;
  uint32_t size;
  uint32_t position;
  uint32_t drain_size;
  bool broken;
};

static bool drain_stm_seek(ph_stream_t *stm, int64_t delta, int whence,
    uint64_t *newpos)
{
  ph_unused_parameter(delta);
  ph_unused_parameter(whence);
  ph_unused_parameter(newpos);
  stm->last_err = ESPIPE;
  return false;
}

static bool drain_stm_close(ph_stream_t *stm)
{
  ph_unused_parameter(stm);
  return true;
}

static bool drain_stm_readv(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nread)
{
  ph_unused_parameter(iov);
  ph_unused_parameter(iovcnt);
  ph_unused_parameter(nread);
  stm->last_err = ENOSYS;
  return false;
}

static bool memcmp_dump(const char *buf, const char *expect, uint32_t len)
{
  uint32_t i, j, dlen;
  char dbuf[64];

  for (i = 0; i < len; i++) {
    if (buf[i] == expect[i]) {
      continue;
    }

    diag("buffers differ starting at byte %" PRIu32 " got %02x expect %02x",
        i, (uint8_t)buf[i], (uint8_t)expect[i]);

    dlen = MIN(i, 24);
    for (j = 0; j <= dlen; j++) {
      ph_snprintf(dbuf + (2*j), sizeof(dbuf), "%02x",
          (uint8_t)buf[j + (i - dlen)]);
    }
    diag("buf holds %s expected last byte to be %02x",
        dbuf, (uint8_t)expect[i]);
    return false;
  }

  return true;
}

static bool drain_stm_writev(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nwrotep)
{
  int i;
  uint64_t n, total = 0;
  struct drain_check *check = (struct drain_check*)stm->cookie;
  uint32_t remain = check->drain_size;
  bool res;

  if (check->broken) {
    diag("call to writev on a broken stream");
    stm->last_err = EFAULT;
    return false;
  }

  for (i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) {
      continue;
    }

    n = MIN(remain, iov[i].iov_len);
    res = memcmp_dump(iov[i].iov_base, check->buf + check->position, n);
    ok(res == true, "memcmp at offset %" PRIu32 " for size %" PRIu64,
        check->position, n);

    if (!res) {
      diag("recording that stream is broken");
      check->broken = true;
      stm->last_err = EFAULT;
      return false;
    }

    check->position += n;
    remain -= n;
    total += n;
    if (remain == 0) {
      break;
    }
  }

  if (total) {
    if (nwrotep) {
      *nwrotep = total;
    }
    return true;
  }
  return false;
}

static struct ph_stream_funcs drain_stm_funcs = {
  drain_stm_close,
  drain_stm_readv,
  drain_stm_writev,
  drain_stm_seek
};

static void test_drain_with_size(const char *big_buf, uint32_t big_buf_size,
    uint32_t drain_size)
{
  ph_bufq_t *q = ph_bufq_new(128 * 1024);
  uint64_t n;
  ph_result_t res;
  struct drain_check check;
  ph_stream_t *stm;

  res = ph_bufq_append(q, big_buf, big_buf_size, &n);
  is_int(PH_OK, res);
  is_int(big_buf_size, n);

  // Now drain the data out in chunks
  check.buf = big_buf;
  check.size = big_buf_size;
  check.position = 0;
  check.drain_size = drain_size;
  check.broken = false;

  stm = ph_stm_make(&drain_stm_funcs, &check, 0, 0);
  while (ph_bufq_len(q)) {
    if (!ph_bufq_stm_write(q, stm, NULL)) {
      diag("write failed with errno %d %s", ph_stm_errno(stm),
          strerror(ph_stm_errno(stm)));
      break;
    }
  }
}

static void test_drain_and_gc(uint32_t big_buf_size)
{
  char *big_buf = build_big_buf(big_buf_size);

  test_drain_with_size(big_buf, big_buf_size, 8192);
}

static void test_record_size_overflow(uint32_t max_size,
    uint32_t data_chunk_size, uint32_t data_count) {
  ph_bufq_t *q = ph_bufq_new(0);
  ph_bufq_set_max_record_size(q, max_size);

  uint64_t data_size = data_chunk_size * data_count;
  char *data = malloc(data_size);

  // Fill 'data' with bytes.
  for (uint64_t i = 0; i < data_size; i++) {
    data[i] = i % 256;
  }

  char delim[16] = "Our Test Delim";
  uint64_t delim_size = sizeof(delim);

  // put the delimiter in the last delim_size bytes.
  memcpy(data + (data_size - delim_size), delim, delim_size);

  // Simulate a protocol loop
  for (uint64_t i = 0; i < data_size; i += data_chunk_size) {
    ph_bufq_append(q, data + i, data_chunk_size, NULL);

    // We should be continuously overflowing past this point.
    bool expect_overflow = ((i + data_chunk_size) > max_size);

    // Expect the buffer on the last iteration.
    bool expect_buffer = ((i + data_chunk_size) == data_size);

    ph_buf_t *buf = ph_bufq_consume_record(q, delim, delim_size);

    if (expect_overflow) {
      ok(errno == EOVERFLOW,
          "errno (%d) not set by consume_record for iteration %lu!",
          errno, i);
    } else {
      ok(errno == 0,
          "errno (%d) was set by consume_record for iteration %lu!",
          errno, i);
    }

    if (expect_buffer) {
      ok(buf != NULL, "buffer %p returned", (void *) buf);

      // Ensure our buffer actually got truncated.
      ok(ph_buf_len(buf) < data_size,
          "buffer size %lu", ph_buf_len(buf));

      // Ensure buffer ends with the delimiter.
      void *bufptr = ph_buf_mem(buf) + (ph_buf_len(buf) - delim_size);

      int found = memcmp(bufptr, delim, delim_size);
      ok(found == 0, "delimiter found (%d)", found == 0);
    } else {
      ok(buf == NULL, "buffer %p returned", (void *) buf);
    }
  }

  free(data);
  ph_bufq_free(q);
}

int main(int argc, char **argv)
{
  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  plan_tests(372);

  test_straddle_edges();

  test_drain_and_gc(8    * 1024);
  test_drain_and_gc(16   * 1024);
  test_drain_and_gc(32   * 1024);
  test_drain_and_gc(64   * 1024);
  test_drain_and_gc(128  * 1024);
  test_drain_and_gc(1024 * 1024);

  // Test with several large chunks.
  test_record_size_overflow(8  * 1024, 8  * 1024, 4);
  test_record_size_overflow(16 * 1024, 16 * 1024, 4);
  test_record_size_overflow(32 * 1024, 32 * 1024, 4);
  test_record_size_overflow(64 * 1024, 64 * 1024, 4);

  // Now test with lots of smaller buffers to append
  test_record_size_overflow(8  * 1024, 2  * 1024, 16);
  test_record_size_overflow(16 * 1024, 4  * 1024, 16);
  test_record_size_overflow(32 * 1024, 8  * 1024, 16);
  test_record_size_overflow(64 * 1024, 16 * 1024, 16);

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

