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
/* Derived from work that is:
 * Copyright (c) 2009-2012 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */
#include "phenom/json.h"
#include "phenom/stream.h"
#include "phenom/variant.h"
#include "phenom/sysutil.h"
#include "phenom/printf.h"

static int do_dump(ph_variant_t *json, uint32_t flags,
    int depth, ph_stream_t *stm);

/* 32 spaces (the maximum indentation size) */
static const char whitespace[] = "                                ";

// I prefer to inline, but gcc 4.4.6 on RHEL 6.2 and 6.3 are unhappy, so
// we get to use good old fashioned define
// https://github.com/facebook/libphenom/issues/7
#define dump(buf, len, stm) (ph_stm_write(stm, buf, len, NULL) ? 0 : -1)

static int dump_indent(uint32_t flags, int depth, int space, ph_stream_t *stm)
{
  if (PH_JSON_INDENT(flags) > 0) {
    int i, ws_count = PH_JSON_INDENT(flags);

    if (dump("\n", 1, stm)) {
      return -1;
    }

    for (i = 0; i < depth; i++) {
      if (dump(whitespace, ws_count, stm)) {
        return -1;
      }
    }

    return 0;
  }

  if (space && !(flags & PH_JSON_COMPACT)) {
    return dump(" ", 1, stm);
  }
  return 0;
}

static int dump_string(ph_string_t *str, ph_stream_t *stm, uint32_t flags)
{
  int32_t codepoint;
  uint32_t off = 0, pos = 0;
  ph_result_t res;
  const char *text;
  char seq[13];
  int length;

  if (dump("\"", 1, stm)) {
    return -1;
  }

  while ((res = ph_string_iterate_utf8_as_utf16(str, &off,
          &codepoint)) == PH_OK) {
    bool escape = false;

    /* mandatory escape or control char */
    if (codepoint == '\\' || codepoint == '"' || codepoint < 0x20) {
      escape = true;
    }

    /* slash */
    if ((flags & PH_JSON_ESCAPE_SLASH) && codepoint == '/') {
      escape = true;
    }

    /* non-ASCII */
    if ((flags & PH_JSON_ENSURE_ASCII) && codepoint > 0x7F) {
      escape = true;
    }

    if (!escape) {
      // pass it thru
      if (dump(str->buf + pos, off - pos, stm)) {
        return -1;
      }
      pos = off;
      continue;
    }

    /* handle \, /, ", and control codes */
    length = 2;
    switch (codepoint) {
      case '\\': text = "\\\\"; break;
      case '\"': text = "\\\""; break;
      case '\b': text = "\\b"; break;
      case '\f': text = "\\f"; break;
      case '\n': text = "\\n"; break;
      case '\r': text = "\\r"; break;
      case '\t': text = "\\t"; break;
      case '/':  text = "\\/"; break;
      default:
        /* codepoint is in BMP */
        if (codepoint < 0x10000) {
          ph_snprintf(seq, sizeof(seq), "\\u%04x", codepoint);
          length = 6;
        } else { /* not in BMP -> construct a UTF-16 surrogate pair */
          int32_t first, last;

          codepoint -= 0x10000;
          first = 0xD800 | ((codepoint & 0xffc00) >> 10);
          last = 0xDC00 | (codepoint & 0x003ff);

          ph_snprintf(seq, sizeof(seq), "\\u%04x\\u%04x", first, last);
          length = 12;
        }

        text = seq;
        break;
    }

    if (dump(text, length, stm)) {
      return -1;
    }

    pos = off;
  }

  if (res != PH_DONE) {
    // bad UTF-8 text in string
    return -1;
  }

  return dump("\"", 1, stm);
}

static int dump_real(ph_stream_t *stm, double value)
{
  char buffer[64];
  uint32_t size = sizeof(buffer);
  int ret;
  char *start, *end;
  size_t length;

  ret = ph_snprintf(buffer, size, "%.17g", value);
  if (ret < 0) {
    return -1;
  }

  length = (size_t)ret;
  if (length >= size) {
    return -1;
  }

  /* Make sure there's a dot or 'e' in the output. Otherwise
     a real is converted to an integer when decoding */
  if (strchr(buffer, '.') == NULL &&
      strchr(buffer, 'e') == NULL) {
    if (length + 3 >= size) {
      /* No space to append ".0" */
      return -1;
    }
    buffer[length] = '.';
    buffer[length + 1] = '0';
    buffer[length + 2] = '\0';
    length += 2;
  }

  /* Remove leading '+' from positive exponent. Also remove leading
     zeros from exponents (added by some printf() implementations) */
  start = strchr(buffer, 'e');
  if (start) {
    start++;
    end = start + 1;

    if (*start == '-')
      start++;

    while (*end == '0')
      end++;

    if (end != start) {
      memmove(start, end, length - (size_t)(end - buffer));
      length -= (size_t)(end - start);
    }
  }

  return dump(buffer, length, stm);
}

static int dump_array(ph_variant_t *json, uint32_t flags,
    int depth, ph_stream_t *stm)
{
  uint32_t i, n;

  n = ph_var_array_size(json);

  if (dump("[", 1, stm)) {
    return -1;
  }

  if (n == 0) {
    return dump("]", 1, stm);
  }

  if (dump_indent(flags, depth + 1, 0, stm)) {
    return -1;
  }

  for (i = 0; i < n; ++i) {
    if (do_dump(ph_var_array_get(json, i), flags, depth + 1, stm)) {
      return -1;
    }

    if (i < n - 1) {
      if (dump(",", 1, stm) ||
          dump_indent(flags, depth + 1, 1, stm)) {
        return -1;
      }
    } else {
      if (dump_indent(flags, depth, 0, stm)) {
        return -1;
      }
    }
  }

  return dump("]", 1, stm);
}

static int dump_obj_elem(ph_string_t *key, ph_variant_t *value,
    uint32_t flags, int depth, ph_stream_t *stm,
    const char *separator, int separator_length, bool last)
{
  if (dump_string(key, stm, flags)) {
    return -1;
  }

  if (dump(separator, separator_length, stm) ||
      do_dump(value, flags, depth + 1, stm)) {
    return -1;
  }

  if (last) {
    return dump_indent(flags, depth, 0, stm);
  }

  if (dump(",", 1, stm) ||
      dump_indent(flags, depth + 1, 1, stm)) {
    return -1;
  }

  return 0;
}

static int dump_obj(ph_variant_t *json, uint32_t flags,
    int depth, ph_stream_t *stm)
{
  const char *separator;
  int separator_length;
  uint32_t n, i;
  bool last;
  ph_string_t *key;
  ph_variant_t *val;

  n = ph_var_object_size(json);
  if (ph_var_object_size(json) == 0) {
    return dump("{}", 2, stm);
  }

  if (flags & PH_JSON_COMPACT) {
    separator = ":";
    separator_length = 1;
  } else {
    separator = ": ";
    separator_length = 2;
  }

  if (dump("{", 1, stm)) {
    return -1;
  }

  if (dump_indent(flags, depth + 1, 0, stm)) {
    return -1;
  }

  i = 0;
  if (flags & PH_JSON_SORT_KEYS) {
    ph_ht_ordered_iter_t oiter;

    if (ph_var_object_ordered_iter_first(json, &oiter, &key, &val)) do {
      last = ++i >= n;
      if (dump_obj_elem(key, val, flags, depth + 1, stm,
            separator, separator_length, last)) {
        return -1;
      }
    } while (ph_var_object_ordered_iter_next(json, &oiter, &key, &val));
    ph_var_object_ordered_iter_end(json, &oiter);

  } else {
    /* Don't sort keys */
    ph_ht_iter_t iter;

    if (ph_var_object_iter_first(json, &iter, &key, &val)) do {
      last = ++i >= n;
      if (dump_obj_elem(key, val, flags, depth + 1, stm,
            separator, separator_length, last)) {
        return -1;
      }
    } while (ph_var_object_iter_next(json, &iter, &key, &val));
  }

  return dump("}", 1, stm);
}

static int do_dump(ph_variant_t *json, uint32_t flags,
    int depth, ph_stream_t *stm)
{
  switch (ph_var_type(json)) {
    case PH_VAR_NULL:
      return dump("null", 4, stm);

    case PH_VAR_TRUE:
      return dump("true", 4, stm);

    case PH_VAR_FALSE:
      return dump("false", 5, stm);

    case PH_VAR_INTEGER:
      return ph_stm_printf(stm, "%" PRIi64,
          ph_var_int_val(json)) <= 0 ? -1 : 0;

    case PH_VAR_REAL:
      return dump_real(stm, ph_var_double_val(json));

    case PH_VAR_STRING:
      return dump_string(ph_var_string_val(json), stm, flags);

    case PH_VAR_ARRAY:
      return dump_array(json, flags, depth, stm);

    case PH_VAR_OBJECT:
      return dump_obj(json, flags, depth, stm);

    default:
      /* not reached */
      return -1;
  }
}

ph_result_t ph_json_dump_stream(ph_variant_t *var, ph_stream_t *stm,
    uint32_t flags)
{
  if (do_dump(var, flags, 0, stm)) {
    return PH_ERR;
  }
  return PH_OK;
}

ph_result_t ph_json_dump_string(ph_variant_t *var, ph_string_t *str,
    uint32_t flags)
{
  ph_stream_t *stm;
  ph_result_t res;

  stm = ph_stm_string_open(str);
  if (!stm) {
    return PH_NOMEM;
  }
  ph_stm_seek(stm, 0, SEEK_END, NULL);
  res = ph_json_dump_stream(var, stm, flags);

  ph_stm_close(stm);

  return res;
}

/* vim:ts=2:sw=2:et:
 */

