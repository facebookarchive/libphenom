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
#include "phenom/log.h"
#include "phenom/printf.h"
#include <assert.h>

#define STREAM_STATE_OK        0
#define STREAM_STATE_EOF      -1
#define STREAM_STATE_ERROR    -2

#define TOKEN_INVALID         -1
#define TOKEN_EOF              0
#define TOKEN_STRING         256
#define TOKEN_INTEGER        257
#define TOKEN_REAL           258
#define TOKEN_TRUE           259
#define TOKEN_FALSE          260
#define TOKEN_NULL           261

/* Locale independent versions of isxxx() functions */
#define l_isupper(c)  ('A' <= (c) && (c) <= 'Z')
#define l_islower(c)  ('a' <= (c) && (c) <= 'z')
#define l_isalpha(c)  (l_isupper(c) || l_islower(c))
#define l_isdigit(c)  ('0' <= (c) && (c) <= '9')
#define l_isxdigit(c) \
    (l_isdigit(c) || 'A' <= (c) || (c) <= 'F' || 'a' <= (c) || (c) <= 'f')

typedef struct {
  ph_stream_t *stm;
  char buffer[5];
  size_t buffer_pos;
  int state;
  int line;
  int column, last_column;
  size_t position;
} stream_t;

typedef struct {
  stream_t stream;
  ph_string_t saved_text;
  int token;
  union {
    char *string;
    int64_t integer;
    double real;
  } value;
  char saved_buf[512];
} lex_t;

#define stream_to_lex(stream) ph_container_of(stream, lex_t, stream)

static ph_memtype_t mt_json;
static struct ph_memtype_def def = {
  "variant", "json", 0, 0
};


/*** error reporting ***/

static void error_set(ph_var_err_t *error, const lex_t *lex,
                      const char *msg, ...)
{
  va_list ap;
  char msg_text[PH_VAR_ERROR_TEXT_LENGTH];
  char msg_with_context[PH_VAR_ERROR_TEXT_LENGTH];
  int line = -1, col = -1;
  size_t pos = 0;
  const char *result = msg_text;
  uint32_t len;

  // Don't change it if it is already set
  if (!error || error->text[0]) {
    return;
  }

  va_start(ap, msg);
  ph_vsnprintf(msg_text, sizeof(msg_text), msg, ap);
  va_end(ap);

  line = lex->stream.line;
  col = lex->stream.column;
  pos = lex->stream.position;

  len = ph_string_len(&lex->saved_text);
  // If the first byte is NUL, just pretend it has no length,
  // otherwise we end up with weird error messages
  if (len && lex->saved_text.buf[0] == 0) {
    len = 0;
  }

  if (len) {
    if (len <= 20) {
      ph_snprintf(msg_with_context, sizeof(msg_with_context),
          "%s near '`Ps%p'", msg_text, (void*)&lex->saved_text);
      result = msg_with_context;
    }
  } else {
    if (lex->stream.state == STREAM_STATE_ERROR) {
      /* No context for UTF-8 decoding errors */
      result = msg_text;
    } else {
      ph_snprintf(msg_with_context, sizeof(msg_with_context),
          "%s near end of file", msg_text);
      result = msg_with_context;
    }
  }

  error->line = line;
  error->column = col;
  error->position = pos;

  ph_snprintf(error->text, sizeof(error->text), "%s", result);
}

static void error_set_oom(ph_var_err_t *error, const lex_t *lex)
{
  if (!error || error->text[0]) {
    return;
  }
  error->transient = true;
  if (lex) {
    error_set(error, lex, "out of memory");
  } else {
    strcpy(error->text, "out of memory"); // NOLINT(runtime/printf)
  }
}

/*** lexical analyzer ***/

static void stream_init(stream_t *stream, ph_stream_t *stm)
{
  stream->stm = stm;
  stream->buffer[0] = '\0';
  stream->buffer_pos = 0;

  stream->state = STREAM_STATE_OK;
  stream->line = 1;
  stream->column = 0;
  stream->position = 0;
}

static int stream_getc(stream_t *stream)
{
  uint8_t buf;
  uint64_t nread = 0;

  if (!ph_stm_read(stream->stm, &buf, 1, &nread) || nread == 0) {
    stream->state = STREAM_STATE_EOF;
    return STREAM_STATE_EOF;
  }
  return (int)buf;
}

static int utf8_check_full(const char *buffer, int size)
{
  int i;
  int32_t value = 0;
  unsigned char u = (unsigned char)buffer[0];

  if (size == 2) {
    value = u & 0x1F;
  } else if (size == 3) {
    value = u & 0xF;
  } else if (size == 4) {
    value = u & 0x7;
  } else {
    return 0;
  }

  for (i = 1; i < size; i++) {
    u = (unsigned char)buffer[i];

    if (u < 0x80 || u > 0xBF) {
      /* not a continuation byte */
      return 0;
    }
    value = (value << 6) + (u & 0x3F);
  }

  if (value > 0x10FFFF) {
    /* not in Unicode range */
    return 0;
  } else if (0xD800 <= value && value <= 0xDFFF) {
    /* invalid code point (UTF-16 surrogate halves) */
    return 0;
  } else if ((size == 2 && value < 0x80) ||
      (size == 3 && value < 0x800) ||
      (size == 4 && value < 0x10000)) {
    /* overlong encoding */
    return 0;
  }

  return 1;
}

static int stream_get(stream_t *stream, ph_var_err_t *error)
{
  int c;

  if (stream->state != STREAM_STATE_OK) {
    return stream->state;
  }

  if (!stream->buffer[stream->buffer_pos]) {
    uint8_t count;

    c = stream_getc(stream);
    if (c == STREAM_STATE_EOF) {
      return c;
    }

    stream->buffer[0] = c;
    stream->buffer_pos = 0;

    count = ph_utf8_seq_len(c);
    if (!count) {
      goto out;
    }

    if (count > 1) {
      /* multi-byte UTF-8 sequence */
      int i;

      assert(count >= 2);

      for (i = 1; i < count; i++) {
        stream->buffer[i] = stream_getc(stream);
      }

      if (!utf8_check_full(stream->buffer, count)) {
        goto out;
      }

      stream->buffer[count] = '\0';
    } else {
      stream->buffer[1] = '\0';
    }
  }

  c = stream->buffer[stream->buffer_pos++];

  stream->position++;
  if (c == '\n') {
    stream->line++;
    stream->last_column = stream->column;
    stream->column = 0;
  } else if (ph_utf8_seq_len(c) > 0) {
    /* track the Unicode character column, so increment only if
       this is the first character of a UTF-8 sequence */
    stream->column++;
  }

  return c;

out:
  stream->state = STREAM_STATE_ERROR;
  error_set(error, stream_to_lex(stream),
      "unable to decode byte 0x%02x", (uint8_t)c);
  return STREAM_STATE_ERROR;
}

static void stream_unget(stream_t *stream, int c)
{
  if (c == STREAM_STATE_EOF || c == STREAM_STATE_ERROR) {
    return;
  }

  stream->position--;
  if (c == '\n') {
    stream->line--;
    stream->column = stream->last_column;
  } else if (ph_utf8_seq_len(c) > 0) {
    stream->column--;
  }

  assert(stream->buffer_pos > 0);
  stream->buffer_pos--;
  assert(stream->buffer[stream->buffer_pos] == c);
}


static int lex_get(lex_t *lex, ph_var_err_t *error)
{
  return stream_get(&lex->stream, error);
}

static void lex_save(lex_t *lex, int c)
{
  char b = (uint8_t)c;

  ph_string_append_buf(&lex->saved_text, &b, 1);
}

static int lex_get_save(lex_t *lex, ph_var_err_t *error)
{
  int c = stream_get(&lex->stream, error);
  if (c != STREAM_STATE_EOF && c != STREAM_STATE_ERROR)
    lex_save(lex, c);
  return c;
}

static void lex_unget(lex_t *lex, int c)
{
  stream_unget(&lex->stream, c);
}

static void lex_unget_unsave(lex_t *lex, int c)
{
  if (c != STREAM_STATE_EOF && c != STREAM_STATE_ERROR) {
    uint32_t len;

    len = ph_string_len(&lex->saved_text);
    assert(len > 0 && lex->saved_text.buf[len-1] == c);
    lex->saved_text.len--;

    stream_unget(&lex->stream, c);
  }
}

static void lex_save_cached(lex_t *lex)
{
  while (lex->stream.buffer[lex->stream.buffer_pos] != '\0') {
    lex_save(lex, lex->stream.buffer[lex->stream.buffer_pos]);
    lex->stream.buffer_pos++;
    lex->stream.position++;
  }
}

/* assumes that str points to 'u' plus at least 4 valid hex digits */
static int32_t decode_unicode_escape(const char *str)
{
  int i;
  int32_t value = 0;

  assert(str[0] == 'u');

  for (i = 1; i <= 4; i++) {
    char c = str[i];
    value <<= 4;
    if (l_isdigit(c))
      value += c - '0';
    else if (l_islower(c))
      value += c - 'a' + 10;
    else if (l_isupper(c))
      value += c - 'A' + 10;
    else
      ph_panic("unpossible unicode escape c=%d", c);
  }

  return value;
}

static int utf8_encode(int32_t codepoint, char *buffer, int *size)
{
  if (codepoint < 0) {
    return -1;
  } else if (codepoint < 0x80) {
    buffer[0] = (char)codepoint;
    *size = 1;
  } else if (codepoint < 0x800) {
    buffer[0] = 0xC0 + ((codepoint & 0x7C0) >> 6);
    buffer[1] = 0x80 + ((codepoint & 0x03F));
    *size = 2;
  } else if (codepoint < 0x10000) {
    buffer[0] = 0xE0 + ((codepoint & 0xF000) >> 12);
    buffer[1] = 0x80 + ((codepoint & 0x0FC0) >> 6);
    buffer[2] = 0x80 + ((codepoint & 0x003F));
    *size = 3;
  } else if (codepoint <= 0x10FFFF) {
    buffer[0] = 0xF0 + ((codepoint & 0x1C0000) >> 18);
    buffer[1] = 0x80 + ((codepoint & 0x03F000) >> 12);
    buffer[2] = 0x80 + ((codepoint & 0x000FC0) >> 6);
    buffer[3] = 0x80 + ((codepoint & 0x00003F));
    *size = 4;
  } else {
    return -1;
  }

  return 0;
}

static void lex_scan_string(lex_t *lex, ph_var_err_t *error)
{
  int c;
  const char *p;
  char *t;
  int i;

  lex->value.string = NULL;
  lex->token = TOKEN_INVALID;

  c = lex_get_save(lex, error);

  while (c != '"') {
    if (c == STREAM_STATE_ERROR) {
      goto out;
    } else if (c == STREAM_STATE_EOF) {
      error_set(error, lex, "premature end of input");
      goto out;
    } else if (0 <= c && c <= 0x1F) {
      /* control character */
      lex_unget_unsave(lex, c);
      if (c == '\n') {
        error_set(error, lex, "unexpected newline", c);
      } else {
        error_set(error, lex, "control character 0x%x", c);
      }
      goto out;
    } else if (c == '\\') {
      c = lex_get_save(lex, error);
      if (c == 'u') {
        c = lex_get_save(lex, error);
        for (i = 0; i < 4; i++) {
          if (!l_isxdigit(c)) {
            error_set(error, lex, "invalid escape");
            goto out;
          }
          c = lex_get_save(lex, error);
        }
      } else if (c == '"' || c == '\\' || c == '/' || c == 'b' ||
          c == 'f' || c == 'n' || c == 'r' || c == 't') {
        c = lex_get_save(lex, error);
      } else {
        error_set(error, lex, "invalid escape");
        goto out;
      }
    } else {
      c = lex_get_save(lex, error);
    }
  }

  /* the actual value is at most of the same length as the source
     string, because:
     - shortcut escapes (e.g. "\t") (length 2) are converted to 1 byte
     - a single \uXXXX escape (length 6) is converted to at most 3 bytes
     - two \uXXXX escapes (length 12) forming an UTF-16 surrogate pair
     are converted to 4 bytes
     */
  lex->value.string = ph_mem_alloc_size(mt_json,
      ph_string_len(&lex->saved_text) + 1);
  if (!lex->value.string) {
    error_set_oom(error, lex);
    /* this is not very nice, since TOKEN_INVALID is returned */
    goto out;
  }

  /* the target */
  t = lex->value.string;

  /* + 1 to skip the " */
  p = lex->saved_text.buf + 1;

  while (*p != '"') {
    if (*p == '\\') {
      p++;
      if (*p == 'u') {
        char buffer[4];
        int length;
        int32_t value;

        value = decode_unicode_escape(p);
        p += 5;

        if (0xD800 <= value && value <= 0xDBFF) {
          /* surrogate pair */
          if (*p == '\\' && *(p + 1) == 'u') {
            int32_t value2 = decode_unicode_escape(++p);
            p += 5;

            if (0xDC00 <= value2 && value2 <= 0xDFFF) {
              /* valid second surrogate */
              value =
                ((value - 0xD800) << 10) +
                (value2 - 0xDC00) +
                0x10000;
            } else {
              /* invalid second surrogate */
              error_set(error, lex,
                  "invalid Unicode '\\u%04X\\u%04X'",
                  value, value2);
              goto out;
            }
          } else {
            /* no second surrogate */
            error_set(error, lex, "invalid Unicode '\\u%04X'",
                value);
            goto out;
          }
        } else if (0xDC00 <= value && value <= 0xDFFF) {
          error_set(error, lex, "invalid Unicode '\\u%04X'", value);
          goto out;
        } else if (value == 0) {
          error_set(error, lex, "\\u0000 is not allowed");
          goto out;
        }

        if (utf8_encode(value, buffer, &length)) {
          ph_panic("failed to UTF-8 encode u%04X", value);
        }

        memcpy(t, buffer, length);
        t += length;
      } else {
        switch (*p) {
          case '"': case '\\': case '/':
            *t = *p; break;
          case 'b': *t = '\b'; break;
          case 'f': *t = '\f'; break;
          case 'n': *t = '\n'; break;
          case 'r': *t = '\r'; break;
          case 't': *t = '\t'; break;
          default:
            ph_panic("unpossible escape sequence \\%c (%d)", *p, *p);
        }
        t++;
        p++;
      }
    } else {
      *(t++) = *(p++);
    }
  }
  *t = '\0';
  lex->token = TOKEN_STRING;
  return;

out:
  ph_mem_free(mt_json, lex->value.string);
  lex->value.string = NULL;
}

static int ugh_strtod(ph_string_t *str, double *out)
{
  double value;
  char *end;

  // Force strtod to operate on the JSON defined decimal point,
  // regardless of the current locale settings */
  const char *ldp = localeconv()->decimal_point;
  if (*ldp != '.') {
    char *pos = memchr(str->buf, '.', str->len);
    if (pos) {
      *pos = *ldp;
    }
  }

  errno = 0;
  value = strtod(str->buf, &end);
  assert(end == str->buf + str->len);

  if (errno == ERANGE && value != 0) {
    /* Overflow */
    return -1;
  }

  *out = value;
  return 0;
}

static int lex_scan_number(lex_t *lex, int c, ph_var_err_t *error)
{
  const char *saved_text;
  char *end;
  double value;

  lex->token = TOKEN_INVALID;

  if (c == '-') {
    c = lex_get_save(lex, error);
  }

  if (c == '0') {
    c = lex_get_save(lex, error);
    if (l_isdigit(c)) {
      lex_unget_unsave(lex, c);
      goto out;
    }
  } else if (l_isdigit(c)) {
    c = lex_get_save(lex, error);
    while (l_isdigit(c)) {
      c = lex_get_save(lex, error);
    }
  } else {
    lex_unget_unsave(lex, c);
    goto out;
  }

  if (c != '.' && c != 'E' && c != 'e') {
    int64_t ivalue;

    lex_unget_unsave(lex, c);

    // Force it to be NUL terminated so we can pass it to strtoll
    ph_string_append_buf(&lex->saved_text, "\0", 1);
    saved_text = lex->saved_text.buf;

    errno = 0;
    ivalue = strtoll(saved_text, &end, 10);

    // "undo" the NUL termination
    lex->saved_text.len--;

    if (errno == ERANGE) {
      if (ivalue < 0) {
        error_set(error, lex, "too big negative integer");
      } else {
        error_set(error, lex, "too big integer");
      }
      goto out;
    }

    assert(end == saved_text + ph_string_len(&lex->saved_text));

    lex->token = TOKEN_INTEGER;
    lex->value.integer = ivalue;
    return 0;
  }

  if (c == '.') {
    c = lex_get(lex, error);
    if (!l_isdigit(c)) {
      lex_unget(lex, c);
      goto out;
    }
    lex_save(lex, c);

    c = lex_get_save(lex, error);
    while (l_isdigit(c))
      c = lex_get_save(lex, error);
  }

  if (c == 'E' || c == 'e') {
    c = lex_get_save(lex, error);
    if (c == '+' || c == '-')
      c = lex_get_save(lex, error);

    if (!l_isdigit(c)) {
      lex_unget_unsave(lex, c);
      goto out;
    }

    c = lex_get_save(lex, error);
    while (l_isdigit(c))
      c = lex_get_save(lex, error);
  }

  lex_unget_unsave(lex, c);

  if (ugh_strtod(&lex->saved_text, &value)) {
    error_set(error, lex, "real number overflow");
    goto out;
  }

  lex->token = TOKEN_REAL;
  lex->value.real = value;
  return 0;

out:
  return -1;
}

static int lex_scan(lex_t *lex, ph_var_err_t *error)
{
  int c;

  ph_string_reset(&lex->saved_text);

  if (lex->token == TOKEN_STRING) {
    ph_mem_free(mt_json, lex->value.string);
    lex->value.string = NULL;
  }

  c = lex_get(lex, error);
  while (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
    c = lex_get(lex, error);
  }

  if (c == STREAM_STATE_EOF) {
    lex->token = TOKEN_EOF;
    goto out;
  }

  if (c == STREAM_STATE_ERROR) {
    lex->token = TOKEN_INVALID;
    goto out;
  }

  lex_save(lex, c);

  if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',') {
    lex->token = c;
  } else if (c == '"') {
    lex_scan_string(lex, error);
  } else if (l_isdigit(c) || c == '-') {
    if (lex_scan_number(lex, c, error)) {
      goto out;
    }
  } else if (l_isalpha(c)) {
    /* eat up the whole identifier for clearer error messages */

    c = lex_get_save(lex, error);
    while (l_isalpha(c)) {
      c = lex_get_save(lex, error);
    }
    lex_unget_unsave(lex, c);

    if (ph_string_equal_cstr(&lex->saved_text, "true")) {
      lex->token = TOKEN_TRUE;
    } else if (ph_string_equal_cstr(&lex->saved_text, "false")) {
      lex->token = TOKEN_FALSE;
    } else if (ph_string_equal_cstr(&lex->saved_text, "null")) {
      lex->token = TOKEN_NULL;
    } else {
      lex->token = TOKEN_INVALID;
    }
  } else {
    /* save the rest of the input UTF-8 sequence to get an error
       message of valid UTF-8 */
    lex_save_cached(lex);
    lex->token = TOKEN_INVALID;
  }

out:
  return lex->token;
}

static ph_string_t *lex_steal_string(lex_t *lex)
{
  ph_string_t *str;
  uint32_t len;

  if (lex->token != TOKEN_STRING) {
    return 0;
  }

  len = strlen(lex->value.string);
  str = ph_string_make_claim(mt_json, lex->value.string, len, len);
  if (str) {
    lex->value.string = NULL;
  }
  return str;
}

static void init_json_mem(void)
{
  mt_json = ph_memtype_register(&def);
}
PH_LIBRARY_INIT(init_json_mem, 0)

static int lex_init(lex_t *lex, ph_stream_t *stm)
{
  memset(lex, 0, sizeof(*lex));
  stream_init(&lex->stream, stm);

  ph_string_init_claim(&lex->saved_text, PH_STRING_GROW_MT(mt_json),
      lex->saved_buf, 0, sizeof(lex->saved_buf));

  lex->token = TOKEN_INVALID;
  return 0;
}

static void lex_close(lex_t *lex)
{
  if (lex->token == TOKEN_STRING) {
    ph_mem_free(mt_json, lex->value.string);
  }

  ph_string_delref(&lex->saved_text);
}


/*** parser ***/

static ph_variant_t *parse_value(lex_t *lex, size_t flags,
    ph_var_err_t *error);

static ph_variant_t *parse_object(lex_t *lex, size_t flags,
    ph_var_err_t *error)
{
  ph_variant_t *object = ph_var_object(8);

  if (!object) {
    error_set_oom(error, lex);
    return NULL;
  }

  lex_scan(lex, error);
  if (lex->token == '}') {
    return object;
  }

  while (1) {
    ph_string_t *key;
    ph_variant_t *value;

    if (lex->token != TOKEN_STRING) {
      error_set(error, lex, "string or '}' expected");
      goto error;
    }

    key = lex_steal_string(lex);
    if (!key) {
      return NULL;
    }

    if (flags & PH_JSON_REJECT_DUPLICATES) {
      if (ph_var_object_get(object, key)) {
        ph_string_delref(key);
        error_set(error, lex, "duplicate object key");
        goto error;
      }
    }

    lex_scan(lex, error);
    if (lex->token != ':') {
      ph_string_delref(key);
      error_set(error, lex, "':' expected");
      goto error;
    }

    lex_scan(lex, error);
    value = parse_value(lex, flags, error);
    if (!value) {
      ph_string_delref(key);
      goto error;
    }

    if (ph_var_object_set_claim_kv(object, key, value) != PH_OK) {
      error_set_oom(error, lex);
      ph_string_delref(key);
      ph_var_delref(value);
      goto error;
    }

    lex_scan(lex, error);
    if (lex->token != ',') {
      break;
    }

    lex_scan(lex, error);
  }

  if (lex->token != '}') {
    error_set(error, lex, "'}' expected");
    goto error;
  }

  return object;

error:
  ph_var_delref(object);
  return NULL;
}

static ph_variant_t *parse_array(lex_t *lex, size_t flags, ph_var_err_t *error)
{
  ph_variant_t *array = ph_var_array(8);

  if (!array) {
    error_set_oom(error, lex);
    return NULL;
  }

  lex_scan(lex, error);
  if (lex->token == ']') {
    return array;
  }

  while (lex->token) {
    ph_variant_t *elem = parse_value(lex, flags, error);
    if (!elem) {
      goto error;
    }

    if (ph_var_array_append_claim(array, elem) != PH_OK) {
      error_set_oom(error, lex);
      ph_var_delref(elem);
      goto error;
    }

    lex_scan(lex, error);
    if (lex->token != ',') {
      break;
    }

    lex_scan(lex, error);
  }

  if (lex->token != ']') {
    error_set(error, lex, "']' expected");
    goto error;
  }

  return array;

error:
  ph_var_delref(array);
  return NULL;
}

static ph_variant_t *parse_value(lex_t *lex, size_t flags, ph_var_err_t *error)
{
  ph_variant_t *json = NULL;
  ph_string_t *str;

  switch (lex->token) {
    case TOKEN_STRING:
      str = ph_string_make_cstr(mt_json, lex->value.string);
      if (str) {
        json = ph_var_string_claim(str);
        if (!json) {
          ph_string_delref(str);
        }
      }
      break;

    case TOKEN_INTEGER:
      json = ph_var_int(lex->value.integer);
      break;

    case TOKEN_REAL:
      json = ph_var_double(lex->value.real);
      break;

    case TOKEN_TRUE:
      json = ph_var_bool(true);
      break;

    case TOKEN_FALSE:
      json = ph_var_bool(false);
      break;

    case TOKEN_NULL:
      json = ph_var_null();
      break;

    case '{':
      return parse_object(lex, flags, error);

    case '[':
      return parse_array(lex, flags, error);

    case TOKEN_INVALID:
      error_set(error, lex, "invalid token");
      return NULL;

    default:
      error_set(error, lex, "unexpected token");
      return NULL;
  }

  if (!json) {
    error_set_oom(error, lex);
  }

  return json;
}

static ph_variant_t *parse_json(lex_t *lex, size_t flags, ph_var_err_t *error)
{
  ph_variant_t *result;

  if (error) {
    error->text[0] = 0;
    error->transient = false;
  }

  lex_scan(lex, error);
  if (!(flags & PH_JSON_DECODE_ANY)) {
    if (lex->token != '[' && lex->token != '{') {
      error_set(error, lex, "'[' or '{' expected");
      return NULL;
    }
  }

  result = parse_value(lex, flags, error);
  if (!result)
    return NULL;

  if (!(flags & PH_JSON_DISABLE_EOF_CHECK)) {
    lex_scan(lex, error);
    if (lex->token != TOKEN_EOF) {
      error_set(error, lex, "end of file expected");
      ph_var_delref(result);
      return NULL;
    }
  }

  if (error) {
    /* Save the position even though there was no error */
    error->position = lex->stream.position;
  }

  return result;
}

ph_variant_t *ph_json_load_stream(ph_stream_t *stm, uint32_t flags,
    ph_var_err_t *err)
{
  lex_t lex;
  ph_variant_t *v;

  lex_init(&lex, stm);
  v = parse_json(&lex, flags, err);
  lex_close(&lex);

  return v;
}

ph_variant_t *ph_json_load_string(ph_string_t *str, uint32_t flags,
    ph_var_err_t *err)
{
  ph_stream_t *stm;
  ph_variant_t *v;

  stm = ph_stm_string_open(str);
  if (!stm) {
    if (err) {
      error_set_oom(err, NULL);
    }
    return 0;
  }

  v = ph_json_load_stream(stm, flags, err);

  ph_stm_close(stm);

  return v;
}

ph_variant_t *ph_json_load_cstr(const char *cstr, uint32_t flags,
    ph_var_err_t *err)
{
  ph_string_t str;
  uint32_t len = strlen(cstr);

  ph_string_init_claim(&str, PH_STRING_STATIC, (char*)cstr, len, len);
  return ph_json_load_string(&str, flags, err);
}

/* These live in here because they have access to the mt_json memtype
 * defined in this file and I'm too lazy to export it out of here
 */

ph_variant_t *ph_var_string_make_cstr(const char *cstr)
{
  ph_string_t *str;
  ph_variant_t *var;

  str = ph_string_make_cstr(mt_json, cstr);
  if (!str) {
    return 0;
  }

  var = ph_var_string_claim(str);
  if (!var) {
    ph_string_delref(str);
  }

  return var;
}

ph_result_t ph_var_object_set_claim_cstr(ph_variant_t *obj,
    const char *cstr, ph_variant_t *val)
{
  ph_string_t *str;
  ph_result_t res;

  str = ph_string_make_cstr(mt_json, cstr);
  if (!str) {
    return PH_NOMEM;
  }

  res = ph_var_object_set_claim_kv(obj, str, val);

  if (res != PH_OK) {
    ph_string_delref(str);
  }

  return res;
}

/* vim:ts=2:sw=2:et:
 */

