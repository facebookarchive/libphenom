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
#include "phenom/printf.h"
#include <ctype.h>

static ph_memtype_t mt_string = PH_MEMTYPE_INVALID;
static ph_memtype_def_t string_def = {
  "string", "string", sizeof(ph_string_t), 0
};

static void do_string_init(void)
{
  mt_string = ph_memtype_register(&string_def);
}

PH_LIBRARY_INIT(do_string_init, 0)

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

ph_string_t *ph_string_make_slice(ph_string_t *str,
    uint32_t start, uint32_t len)
{
  ph_string_t *slice;

  if (start == 0 && len == str->len) {
    ph_string_addref(str);
    return str;
  }

  slice = ph_mem_alloc(mt_string);
  if (!slice) {
    return NULL;
  }

  ph_string_init_slice(slice, str, start, len);
  return slice;
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

      if (str->mt < 0) {
        // Promote from static growable to heap allocated growable
        memcpy(nbuf, str->buf, str->len);
        str->mt = -str->mt;
      }

      str->buf = nbuf;
      str->alloc = nsize;
    }
  }

  memcpy(str->buf + str->len, buf, len);
  str->len += len;
  return PH_OK;
}

bool ph_string_equal_caseless(const ph_string_t *a, const ph_string_t *b)
{
  uint32_t i;

  if (a == b) {
    return true;
  }
  if (a->len != b->len) {
    return false;
  }

  for (i = 0; i < a->len; i++) {
    if (tolower(a->buf[i]) != tolower(b->buf[i])) {
      return false;
    }
  }

  return true;
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
  uint32_t len;

  if (b == NULL) {
    return false;
  }

  len = strlen(b);

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
  ph_unused_parameter(arg);
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

ph_result_t ph_string_append_utf16_as_utf8(
    ph_string_t *str, int32_t *codepoints, uint32_t numpoints,
    uint32_t *bytes)
{
  uint32_t appended = 0;
  ph_result_t res = PH_OK;
  char buf[4];

  while (numpoints--) {
    int32_t cp = *codepoints;
    int buflen;
    codepoints++;

    if (cp < 0) {
      res = PH_ERR;
      break;
    }

    if (cp < 0x80) {
      buf[0] = (char)cp;
      buflen = 1;
    } else if (cp < 0x800) {
      buf[0] = 0xc0 + ((cp & 0x7c0) >> 6);
      buf[1] = 0x80 + (cp & 0x03f);
      buflen = 2;
    } else if (cp < 0x10000) {
      buf[0] = 0xE0 + ((cp & 0xF000) >> 12);
      buf[1] = 0x80 + ((cp & 0x0FC0) >> 6);
      buf[2] = 0x80 + (cp & 0x003F);
      buflen = 3;
    } else if (cp <= 0x10FFFF) {
      buf[0] = 0xF0 + ((cp & 0x1C0000) >> 18);
      buf[1] = 0x80 + ((cp & 0x03F000) >> 12);
      buf[2] = 0x80 + ((cp & 0x000FC0) >> 6);
      buf[3] = 0x80 + (cp & 0x00003F);
      buflen = 4;
    } else {
      res = PH_ERR;
      break;
    }

    res = ph_string_append_buf(str, buf, buflen);
    if (res != PH_OK) {
      break;
    }
    appended += buflen;
  }

  if (bytes) {
    *bytes = appended;
  }

  return res;
}

ph_result_t ph_string_iterate_utf8_as_utf16(
    ph_string_t *str, uint32_t *offset, int32_t *codepoint)
{
  uint32_t p = *offset;
  uint32_t len, i;
  uint8_t *b, first;
  int32_t cp;

  if (p >= str->len) {
    return PH_DONE;
  }

  // Figure out how long the UTF-8 sequence is
  b = (uint8_t*)str->buf + p;
  first = *b;

  if (first < 0x80) {
    len = 1;
    cp = first;
  } else if (first <= 0xc1) {
    // not valid as the first character
    return PH_ERR;
  } else if (first <= 0xdf) {
    len = 2;
    cp = first & 0x1f;
  } else if (first <= 0xef) {
    len = 3;
    cp = first & 0xf;
  } else if (first <= 0xf4) {
    len = 4;
    cp = first & 0x7;
  } else {
    return PH_ERR;
  }

  // Do we have enough space to read the rest of the sequence?
  if (p + len > str->len) {
    return PH_ERR;
  }

  for (i = 1; i < len; i++) {
    uint8_t u = b[i];

    if (u < 0x80 || u > 0xbf) {
      // Not a valid continuation
      return PH_ERR;
    }

    cp = (cp << 6) + (u & 0x3f);
  }

  if (cp > 0x10ffff) {
    // Out of range
    return PH_ERR;
  }

  if (cp >= 0xd800 && cp <= 0xdfff) {
    // Surrogate pair
    return PH_ERR;
  }

  if ((len == 2 && cp < 0x80) ||
      (len == 3 && cp < 0x800) ||
      (len == 4 && cp < 0x10000)) {
    // over long encoding
    return PH_ERR;
  }

  *codepoint = cp;
  *offset = p + len;
  return PH_OK;
}

bool ph_string_is_valid_utf8(ph_string_t *str)
{
  uint32_t off = 0;
  int32_t cp;
  ph_result_t res;

  for (res = ph_string_iterate_utf8_as_utf16(str, &off, &cp);
      res == PH_OK;
      res = ph_string_iterate_utf8_as_utf16(str, &off, &cp)) {
    ;
  }

  if (res == PH_DONE) {
    return true;
  }

  return false;
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

// Helper for PH_STRING_DECLARE_CSTR_AVOID_COPY
bool _ph_string_nul_terminated(ph_string_t *str) {
  if (str->len >= str->alloc) {
    return false;
  }
  if (str->buf[str->len] == '\0') {
    return true;
  }
  if (str->slice) {
    return false;
  }
  if (str->len < str->alloc) {
    str->buf[str->len] = '\0';
    return true;
  }
  return false;
}

/* vim:ts=2:sw=2:et:
 */

