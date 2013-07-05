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
#include "phenom/defs.h"
#include "phenom/string.h"
#include "phenom/sysutil.h"

static ph_memtype_t mt_string = PH_MEMTYPE_INVALID;
static ph_memtype_def_t string_def = {
  "string", "string", sizeof(ph_string_t), 0
};
static pthread_once_t done_string_init = PTHREAD_ONCE_INIT;

static void do_string_init(void)
{
  mt_string = ph_memtype_register(&string_def);
}

static inline void init_string(void)
{
  if (unlikely(mt_string == PH_MEMTYPE_INVALID)) {
    pthread_once(&done_string_init, do_string_init);
  }
}

void ph_string_init_slice(ph_string_t *str,
    ph_string_t *slice, uint32_t start, uint32_t len)
{
  str->ref = 1;
  str->slice = slice;
  str->mt = PH_MEMTYPE_INVALID;
  str->onstack = true;
  str->buf = slice->buf + start;
  str->len = len;
  str->alloc = len;

  ph_string_addref(slice);
}

void ph_string_init_claim(ph_string_t *str,
    ph_memtype_t mt, char *buf, uint32_t len, uint32_t size)
{
  str->ref = 1;
  str->buf = buf;
  str->len = len;
  str->alloc = size;
  str->slice = 0;
  str->mt = mt;
  str->onstack = true;
}

ph_string_t *ph_string_make_claim(ph_memtype_t mt,
    char *buf, uint32_t len, uint32_t size)
{
  ph_string_t *str;

  init_string();

  str = ph_mem_alloc(mt_string);
  if (!str) {
    return NULL;
  }

  str->ref = 1;
  str->buf = buf;
  str->len = len;
  str->alloc = size;
  str->slice = 0;
  str->mt = mt;
  str->onstack = false;

  return str;
}

ph_string_t *ph_string_make_empty(ph_memtype_t mt,
    uint32_t size)
{
  char *buf = ph_mem_alloc_size(mt, size);
  ph_string_t *str;

  if (!buf) {
    return NULL;
  }

  str = ph_string_make_claim(mt, buf, 0, size);
  if (!str) {
    ph_mem_free(mt, buf);
  }

  return str;
}

ph_string_t *ph_string_make_copy(ph_memtype_t mt,
    const char *buf, uint32_t len, uint32_t size)
{
  ph_string_t *str = ph_string_make_empty(mt, MAX(len, size));

  if (!str) {
    return NULL;
  }

  ph_string_append_buf(str, buf, len);
  return str;
}

void ph_string_delref(ph_string_t *str)
{
  if (!ph_refcnt_del(&str->ref)) {
    return;
  }

  if (str->mt >= 0) {
    ph_mem_free(str->mt, str->buf);
    str->mt = PH_MEMTYPE_INVALID;
  }
  if (str->slice) {
    ph_string_delref(str->slice);
    str->slice = 0;
  }
  str->buf = 0;
  if (!str->onstack) {
    ph_mem_free(mt_string, str);
  }
}

ph_result_t ph_string_append_buf(ph_string_t *str,
    const char *buf, uint32_t len)
{
  if (len + str->len > str->alloc) {
    // Not enough room
    if (str->mt == PH_STRING_STATIC) {
      // Just clamp to the available space
      len = str->alloc - str->len;
    } else {
      // Grow it
      uint32_t nsize = ph_power_2(str->len + len);
      char *nbuf;

      // Negative memtypes encode the desired memtype as the negative
      // value.  Allocate a buffer from scratch using the desired memtype
      if (str->mt < 0) {
        nbuf = ph_mem_alloc_size(-str->mt, nsize);
      } else {
        nbuf = ph_mem_realloc(str->mt, str->buf, nsize);
      }

      if (nbuf == NULL) {
        return PH_NOMEM;
      }

      str->buf = nbuf;
      str->alloc = nsize;

      // Promote from static growable to heap allocated growable
      if (str->mt < 0) {
        str->mt = -str->mt;
      }
    }
  }

  memcpy(str->buf + str->len, buf, len);
  str->len += len;
  return PH_OK;
}

bool ph_string_equal(const ph_string_t *a, const ph_string_t *b)
{
  if (a == b) {
    return true;
  }
  if (a->len != b->len) {
    return false;
  }
  return memcmp(a->buf, b->buf, a->len) == 0;
}

bool ph_string_equal_cstr(ph_string_t *a, const char *b)
{
  uint32_t len = strlen(b);

  if (len != a->len) {
    return false;
  }

  return memcmp(a->buf, b, len) == 0;
}

int ph_string_compare(const ph_string_t *a, const ph_string_t *b)
{
  uint32_t len;
  int res;

  if (a == b) {
    return 0;
  }

  len = MIN(a->len, b->len);
  res = memcmp(a->buf, b->buf, len);
  if (res != 0 || a->len == b->len) {
    return res;
  }
  if (a->len > b->len) {
    return 1;
  }
  return -1;
}

static bool str_print(void *arg, const char *buf, size_t len)
{
  return ph_string_append_buf(arg, buf, len) == PH_OK;
}

static bool str_flush(void *arg)
{
  unused_parameter(arg);
  return true;
}

static struct ph_vprintf_funcs str_funcs = {
  str_print,
  str_flush
};

int ph_string_vprintf(ph_string_t *a, const char *fmt, va_list ap)
{
  return ph_vprintf_core(a, &str_funcs, fmt, ap);
}

int ph_string_printf(ph_string_t *a, const char *fmt, ...)
{
  int ret;
  va_list ap;

  va_start(ap, fmt);
  ret = ph_string_vprintf(a, fmt, ap);
  va_end(ap);

  return ret;
}

ph_string_t *ph_string_make_printf(ph_memtype_t mt, uint32_t size,
    const char *fmt, ...)
{
  ph_string_t *str = ph_string_make_empty(mt, size);
  va_list ap;

  if (!str) {
    return NULL;
  }

  va_start(ap, fmt);
  ph_string_vprintf(str, fmt, ap);
  va_end(ap);

  return str;
}

ph_result_t ph_string_append_cstr(
    ph_string_t *str, const char *cstr)
{
  return ph_string_append_buf(str, cstr, strlen(cstr));
}

ph_string_t *ph_string_make_cstr(ph_memtype_t mt, const char *str)
{
  return ph_string_make_copy(mt, str, strlen(str), 0);
}

/* vim:ts=2:sw=2:et:
 */

