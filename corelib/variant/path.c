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
/* Portions are Copyright (c) 2012 Rogerz Zhang <rogerz.zhang@gmail.com>
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include "phenom/variant.h"

ph_variant_t *ph_var_jsonpath_get(ph_variant_t *var, const char *path)
{
  static const char root_chr = '$', array_open = '[';
  static const char *path_delims = ".[", *array_close = "]";
  ph_variant_t *cursor = 0;
  char *token, *buf, *peek, *endptr, delim = '\0';
  const char *expect;
  char local_buf[128];
  uint32_t pathlen;

  if (!var || !path || path[0] != root_chr) {
    return NULL;
  }
  pathlen = strlen(path);
  // Avoid malloc in the common case.  If your queries are longer
  // than 128 characters you might be doing something weird
  if (pathlen < sizeof(local_buf)-1) {
    strcpy(local_buf, path); // NOLINT(runtime/printf)
    buf = local_buf;
  } else {
    buf = strdup(path);
  }

  peek = buf + 1;
  cursor = var;
  token = NULL;
  expect = path_delims;

  while (peek && *peek && cursor) {
    char *last_peek = peek;
    peek = strpbrk(peek, expect);
    if (peek) {
      if (!token && peek != last_peek) {
        cursor = 0;
        goto done;
      }
      delim = *peek;
      *peek++ = '\0';
    } else if (expect != path_delims || !token) {
      cursor = 0;
      goto done;
    }

    if (expect == path_delims) {
      if (token) {
        cursor = ph_var_object_get_cstr(cursor, token);
      }
      expect = (delim == array_open ? array_close : path_delims);
      token = peek;
    } else if (expect == array_close) {
      size_t idx = strtol(token, &endptr, 0);
      if (*endptr) {
        cursor = 0;
        goto done;
      }
      cursor = ph_var_array_get(cursor, idx);
      token = NULL;
      expect = path_delims;
    } else {
      cursor = 0;
      goto done;
    }
  }

done:
  if (buf != local_buf) {
    free(buf);
  }
  return cursor;
}


/* vim:ts=2:sw=2:et:
 */

