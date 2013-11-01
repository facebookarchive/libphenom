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
#include "phenom/stream.h"

struct string_stream {
  ph_string_t *str;
  uint32_t pos;
};

static ph_memtype_t mt_string_stream;

static struct ph_memtype_def def = {
  "stream", "string", sizeof(struct string_stream), 0
};

static void init_string_stream(void)
{
  mt_string_stream = ph_memtype_register(&def);
}
PH_LIBRARY_INIT(init_string_stream, 0)

static bool str_close(ph_stream_t *stm)
{
  struct string_stream *ss = stm->cookie;

  ph_string_delref(ss->str);
  ph_mem_free(mt_string_stream, ss);
  stm->cookie = 0;

  return true;
}

static bool str_readv(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nread)
{
  struct string_stream *ss = stm->cookie;
  int r = 0;
  int i;

  for (i = 0; ss->pos < ss->str->len && i < iovcnt; i++) {
    int n = MIN(iov[i].iov_len, ss->str->len - ss->pos);

    if (!n) {
      continue;
    }

    memcpy(iov[i].iov_base, ss->str->buf + ss->pos, n);
    ss->pos += n;
    r += n;
  }

  if (nread) {
    *nread = r;
  }
  return true;
}


static bool str_writev(ph_stream_t *stm, const struct iovec *iov,
    int iovcnt, uint64_t *nwrote)
{
  struct string_stream *ss = stm->cookie;
  int i;
  uint64_t w = 0;
  ph_result_t res;

  for (i = 0; i < iovcnt; i++) {
    char *buf = iov[i].iov_base;
    uint32_t len = iov[i].iov_len;
    uint32_t n = MIN(len, ss->str->len - ss->pos);

    if (n) {
      // Copy direct to buffer space
      memcpy(ss->str->buf + ss->pos, buf, n);
      buf += n;
      len -= n;
      ss->pos += n;
      w += n;
    }

    if (len) {
      // Append
      res = ph_string_append_buf(ss->str, buf, len);
      if (res != PH_OK) {
        break;
      }
      w += len;
      ss->pos += len;
    }
  }

  if (w == 0) {
    stm->last_err = ENOMEM;
    return false;
  }

  if (nwrote) {
    *nwrote = w;
  }
  return true;
}

static bool str_seek(ph_stream_t *stm, int64_t delta,
    int whence, uint64_t *newpos)
{
  struct string_stream *ss = stm->cookie;
  off_t off = ss->pos;

  switch (whence) {
    case SEEK_SET:
      off = delta;
      break;
    case SEEK_CUR:
      off += delta;
      break;
    case SEEK_END:
      off = ss->str->len + delta;
      break;
    default:
      stm->last_err = EINVAL;
      return false;
  }

  if (off < 0 || off > ss->str->len) {
    stm->last_err = EINVAL;
    return false;
  }

  ss->pos = off;

  if (newpos) {
    *newpos = off;
  }
  return true;
}

static struct ph_stream_funcs str_funcs = {
  str_close,
  str_readv,
  str_writev,
  str_seek
};

ph_stream_t *ph_stm_string_open(ph_string_t *str)
{
  ph_stream_t *stm;
  struct string_stream *ss;

  if (!str) {
    return 0;
  }

  ss = ph_mem_alloc(mt_string_stream);
  if (!ss) {
    return 0;
  }

  ss->str = str;
  ss->pos = 0;

  // No sense having a buffer over strings!
  stm = ph_stm_make(&str_funcs, ss, 0, 0);

  if (!stm) {
    ph_mem_free(mt_string_stream, ss);
    return 0;
  }

  ph_string_addref(str);
  return stm;
}

/* vim:ts=2:sw=2:et:
 */

