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

#include "phenom/variant.h"
#include "phenom/log.h"
#include "phenom/sysutil.h"
#include "phenom/printf.h"

typedef struct {
  const char *start;
  const char *fmt;
  ph_var_err_t *error;
  char token;
  uint32_t flags;
  int line;
  int column;
} scanner_t;

// Order must match the enum ph_variant_type_t
static const char *type_names[] = {
  "object",
  "array",
  "string",
  "integer",
  "real",
  "true",
  "false",
  "null"
};

#define type_name(x) type_names[ph_var_type(x)]

static const char *unpack_value_starters = "{[siIbfFOon";


static void scanner_init(scanner_t *s, ph_var_err_t *err,
    uint32_t flags, const char *fmt)
{
  s->error = err;
  s->flags = flags;
  s->fmt = s->start = fmt;
  s->line = 1;
  s->column = 0;
}

static void next_token(scanner_t *s)
{
  const char *t = s->fmt;
  s->column++;

  /* skip space and ignored chars */
  while (*t == ' ' || *t == '\t' || *t == '\n' || *t == ',' || *t == ':') {
    if (*t == '\n') {
      s->line++;
      s->column = 1;
    } else {
      s->column++;
    }

    t++;
  }

  s->token = *t;

  t++;
  s->fmt = t;
}

static void set_error(scanner_t *s, const char *source, const char *fmt, ...)
{
  va_list ap;
  size_t pos;

  if (!s->error || s->error->text[0]) {
    return;
  }

  va_start(ap, fmt);

  pos = (size_t)(s->fmt - s->start);

  ph_snprintf(s->error->text, sizeof(s->error->text),
      "%s: `Pv%s%p", source, fmt, ph_vaptr(ap));
  s->error->line = s->line;
  s->error->column = s->column;
  s->error->position = pos;

  va_end(ap);
}

static ph_variant_t *pack(scanner_t *s, va_list *ap);

static ph_variant_t *pack_object(scanner_t *s, va_list *ap)
{
  ph_variant_t *object = ph_var_object(8);
  next_token(s);

  while (s->token != '}') {
    const char *key;
    ph_variant_t *value;

    if (!s->token) {
      set_error(s, "<format>", "Unexpected end of format string");
      goto error;
    }

    if (s->token != 's') {
      set_error(s, "<format>", "Expected format 's', got '%c'", s->token);
      goto error;
    }

    key = va_arg(*ap, const char *);
    if (!key) {
      set_error(s, "<args>", "NULL object key");
      goto error;
    }

    next_token(s);

    value = pack(s, ap);
    if (!value)
      goto error;

    if (ph_var_object_set_claim_cstr(object, key, value) != PH_OK) {
      set_error(s, "<internal>", "Unable to add key \"%s\"", key);
      goto error;
    }

    next_token(s);
  }

  return object;

error:
  ph_var_delref(object);
  return NULL;
}

static ph_variant_t *pack_array(scanner_t *s, va_list *ap)
{
  ph_variant_t *array = ph_var_array(8);
  next_token(s);

  while (s->token != ']') {
    ph_variant_t *value;

    if (!s->token) {
      set_error(s, "<format>", "Unexpected end of format string");
      goto error;
    }

    value = pack(s, ap);
    if (!value)
      goto error;

    if (ph_var_array_append_claim(array, value)) {
      set_error(s, "<internal>", "Unable to append to array");
      goto error;
    }

    next_token(s);
  }
  return array;

error:
  ph_var_delref(array);
  return NULL;
}

static ph_variant_t *pack(scanner_t *s, va_list *ap)
{
  ph_variant_t *v;

  switch (s->token) {
    case '{':
      return pack_object(s, ap);

    case '[':
      return pack_array(s, ap);

    case 'z': /* C-string */
      {
        const char *str = va_arg(*ap, const char *);
        if (!str) {
          set_error(s, "<args>", "NULL string argument");
          return NULL;
        }
        return ph_var_string_make_cstr(str);
      }

    case 's': /* ph_string_t; don't addref */
      {
        ph_string_t *str = va_arg(*ap, ph_string_t*);
        return ph_var_string_claim(str);
      }

    case 'S': /* ph_string_t; addref */
      {
        ph_string_t *str = va_arg(*ap, ph_string_t*);
        return ph_var_string_make(str);
      }

    case 'n': /* null */
      return ph_var_null();

    case 'b': /* boolean */
      return ph_var_bool(va_arg(*ap, int));

    case 'i': /* integer from int */
      return ph_var_int(va_arg(*ap, int));

    case 'I': /* integer from int64_t */
      return ph_var_int(va_arg(*ap, int64_t));

    case 'f': /* real */
      return ph_var_double(va_arg(*ap, double));

    case 'O': /* a ph_variant_t object; increments refcount */
      v = va_arg(*ap, ph_variant_t*);
      ph_var_addref(v);
      return v;

    case 'o': /* a ph_variant_t object; doesn't increment refcount */
      return va_arg(*ap, ph_variant_t *);

    default:
      set_error(s, "<format>", "Unexpected format character '%c'",
          s->token);
      return NULL;
  }
}

static int unpack(scanner_t *s, ph_variant_t *root, va_list *ap);

static int unpack_object(scanner_t *s, ph_variant_t *root, va_list *ap)
{
  int ret = -1;
  int strict = 0;
  /* Use a set to check that all object keys are accessed. Checking that the
   * correct number of keys were accessed is not enough, as the same key can be
   * unpacked multiple times.
   */
  ph_variant_t *key_set = NULL;

  if (root && !ph_var_is_object(root)) {
    set_error(s, "<validation>", "Expected object, got %s",
        type_name(root));
    return PH_ERR;
  }

  key_set = ph_var_object(root ? ph_var_object_size(root) : 0);
  if (!key_set) {
    set_error(s, "<internal>", "Out of memory");
    return PH_ERR;
  }

  next_token(s);

  while (s->token != '}') {
    const char *key;
    ph_variant_t *value;
    int opt = 0;

    if (strict != 0) {
      set_error(s, "<format>", "Expected '}' after '%c', got '%c'",
          (strict == 1 ? '!' : '*'), s->token);
      goto out;
    }

    if (!s->token) {
      set_error(s, "<format>", "Unexpected end of format string");
      goto out;
    }

    if (s->token == '!' || s->token == '*') {
      strict = (s->token == '!' ? 1 : -1);
      next_token(s);
      continue;
    }

    if (s->token != 's') {
      set_error(s, "<format>", "Expected format 's', got '%c'", s->token);
      goto out;
    }

    key = va_arg(*ap, const char *);
    if (!key) {
      set_error(s, "<args>", "NULL object key");
      goto out;
    }

    next_token(s);

    if (s->token == '?') {
      opt = 1;
      next_token(s);
    }

    if (!root) {
      /* skipping */
      value = NULL;
    } else {
      value = ph_var_object_get_cstr(root, key);
      if (!value && !opt) {
        set_error(s, "<validation>", "Object item not found: %s", key);
        goto out;
      }
    }

    if (unpack(s, value, ap))
      goto out;

    ph_var_object_set_claim_cstr(key_set, key, ph_var_null());
    next_token(s);
  }

  if (root && strict == 1 &&
      ph_var_object_size(key_set) != ph_var_object_size(root)) {
    uint32_t diff = ph_var_object_size(root) - ph_var_object_size(key_set);
    set_error(s, "<validation>",
        "%" PRIu32 " object item(s) left unpacked", diff);
    goto out;
  }

  ret = PH_OK;

out:
  if (key_set) {
    ph_var_delref(key_set);
  }
  return ret;
}

static int unpack_array(scanner_t *s, ph_variant_t *root, va_list *ap)
{
  uint32_t i = 0;
  int strict = 0;

  if (root && !ph_var_is_array(root)) {
    set_error(s, "<validation>", "Expected array, got %s", type_name(root));
    return -1;
  }
  next_token(s);

  while (s->token != ']') {
    ph_variant_t *value;

    if (strict != 0) {
      set_error(s, "<format>", "Expected ']' after '%c', got '%c'",
          (strict == 1 ? '!' : '*'),
          s->token);
      return -1;
    }

    if (!s->token) {
      set_error(s, "<format>", "Unexpected end of format string");
      return -1;
    }

    if (s->token == '!' || s->token == '*') {
      strict = (s->token == '!' ? 1 : -1);
      next_token(s);
      continue;
    }

    if (!strchr(unpack_value_starters, s->token)) {
      set_error(s, "<format>", "Unexpected format character '%c'",
          s->token);
      return -1;
    }

    if (!root) {
      /* skipping */
      value = NULL;
    } else {
      value = ph_var_array_get(root, i);
      if (!value) {
        set_error(s, "<validation>", "Array index %"PRIu32" out of range", i);
        return -1;
      }
    }

    if (unpack(s, value, ap))
      return -1;

    next_token(s);
    i++;
  }

  if (strict == 0 && (s->flags & PH_VAR_STRICT))
    strict = 1;

  if (root && strict == 1 && i != ph_var_array_size(root)) {
    uint32_t diff = ph_var_array_size(root) - i;
    set_error(s, "<validation>", "%"PRIu32" array item(s) left unpacked", diff);
    return -1;
  }

  return 0;
}

static int unpack(scanner_t *s, ph_variant_t *root, va_list *ap)
{
  switch (s->token) {
    case '{':
      return unpack_object(s, root, ap);

    case '[':
      return unpack_array(s, root, ap);

    case 's':
    case 'S':
      if (root && !ph_var_is_string(root)) {
        set_error(s, "<validation>", "Expected string, got %s",
            type_name(root));
        return -1;
      }

      if (!(s->flags & PH_VAR_VALIDATE_ONLY)) {
        ph_string_t **target;

        target = va_arg(*ap, ph_string_t **);
        if (!target) {
          set_error(s, "<args>", "NULL string argument");
          return -1;
        }

        if (root) {
          *target = root->u.sval;
          if (s->token == 'S') {
            ph_string_addref(root->u.sval);
          }
        }
      }
      return 0;

    case 'i':
      if (root && !ph_var_is_int(root)) {
        set_error(s, "<validation>", "Expected integer, got %s",
            type_name(root));
        return -1;
      }

      if (!(s->flags & PH_VAR_VALIDATE_ONLY)) {
        int *target = va_arg(*ap, int*);
        if (root)
          *target = (int)ph_var_int_val(root);
      }

      return 0;

    case 'I':
      if (root && !ph_var_is_int(root)) {
        set_error(s, "<validation>", "Expected integer, got %s",
            type_name(root));
        return -1;
      }

      if (!(s->flags & PH_VAR_VALIDATE_ONLY)) {
        int64_t *target = va_arg(*ap, int64_t*);
        if (root)
          *target = ph_var_int_val(root);
      }

      return 0;

    case 'b':
      if (root && !ph_var_is_boolean(root)) {
        set_error(s, "<validation>", "Expected true or false, got %s",
            type_name(root));
        return -1;
      }

      if (!(s->flags & PH_VAR_VALIDATE_ONLY)) {
        bool *target = va_arg(*ap, bool*);
        if (root)
          *target = ph_var_bool_val(root);
      }

      return 0;

    case 'f':
      if (root && !ph_var_is_double(root)) {
        set_error(s, "<validation>", "Expected real, got %s",
            type_name(root));
        return -1;
      }

      if (!(s->flags & PH_VAR_VALIDATE_ONLY)) {
        double *target = va_arg(*ap, double*);
        if (root)
          *target = ph_var_double_val(root);
      }

      return 0;

    case 'F':
      if (root && !ph_var_is_double(root) && !ph_var_is_int(root)) {
        set_error(s, "<validation>", "Expected real or integer, got %s",
            type_name(root));
        return -1;
      }

      if (!(s->flags & PH_VAR_VALIDATE_ONLY)) {
        double *target = va_arg(*ap, double*);
        if (root) {
          if (ph_var_is_double(root)) {
            *target = ph_var_double_val(root);
          } else {
            *target = (double)ph_var_int_val(root);
          }
        }
      }

      return 0;

    case 'O':
      if (root && !(s->flags & PH_VAR_VALIDATE_ONLY))
        ph_var_addref(root);
      /* Fall through */

    case 'o':
      if (!(s->flags & PH_VAR_VALIDATE_ONLY)) {
        ph_variant_t **target = va_arg(*ap, ph_variant_t**);
        if (root)
          *target = root;
      }

      return 0;

    case 'n':
      /* Never assign, just validate */
      if (root && !ph_var_is_null(root)) {
        set_error(s, "<validation>", "Expected null, got %s",
            type_name(root));
        return -1;
      }
      return 0;

    default:
      set_error(s, "<format>", "Unexpected format character '%c'",
          s->token);
      return -1;
  }
}

ph_variant_t *ph_var_vpack(ph_var_err_t *error, const char *fmt, va_list ap)
{
  scanner_t s;
  va_list ap_copy;
  ph_variant_t *value;

  if (error) {
    error->text[0] = 0;
    error->line = -1;
    error->column = -1;
    error->position = 0;
  }

  if (!fmt || !*fmt) {
    if (error) {
      ph_snprintf(error->text, sizeof(error->text),
          "<format>: NULL or empty format string");
    }
    return 0;
  }

  scanner_init(&s, error, 0, fmt);
  next_token(&s);

  va_copy(ap_copy, ap);
  value = pack(&s, &ap_copy);
  va_end(ap_copy);

  if (!value)
    return NULL;

  next_token(&s);
  if (s.token) {
    ph_var_delref(value);
    set_error(&s, "<format>", "Garbage after format string");
    return NULL;
  }

  return value;
}

ph_variant_t *ph_var_pack(ph_var_err_t *error, const char *fmt, ...)
{
  ph_variant_t *value;
  va_list ap;

  va_start(ap, fmt);
  value = ph_var_vpack(error, fmt, ap);
  va_end(ap);

  return value;
}

ph_result_t ph_var_vunpack(ph_variant_t *root, ph_var_err_t *error,
    uint32_t flags, const char *fmt, va_list ap)
{
  scanner_t s;
  va_list ap_copy;

  if (error) {
    error->text[0] = 0;
    error->line = -1;
    error->column = -1;
    error->position = 0;
  }

  if (!root) {
    if (error) {
      ph_snprintf(error->text, sizeof(error->text), "<root>: NULL root value");
    }
    return PH_ERR;
  }

  if (!fmt || !*fmt) {
    if (error) {
      ph_snprintf(error->text, sizeof(error->text),
          "<format>: NULL or empty format string");
    }
    return PH_ERR;
  }

  scanner_init(&s, error, flags, fmt);
  next_token(&s);

  va_copy(ap_copy, ap);
  if (unpack(&s, root, &ap_copy)) {
    va_end(ap_copy);
    return PH_ERR;
  }
  va_end(ap_copy);

  next_token(&s);
  if (s.token) {
    set_error(&s, "<format>", "Garbage after format string");
    return PH_ERR;
  }

  return PH_OK;
}

ph_result_t ph_var_unpack(ph_variant_t *root, ph_var_err_t *error,
    uint32_t flags, const char *fmt, ...)
{
  int ret;
  va_list ap;

  va_start(ap, fmt);
  ret = ph_var_vunpack(root, error, flags, fmt, ap);
  va_end(ap);

  return ret;
}


/* vim:ts=2:sw=2:et:
 */

