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
#include "phenom/stream.h"
#include "tap.h"

static ph_memtype_def_t mt_def = { "test", "misc", 0, 0 };
static ph_memtype_t mt_misc = 0;

static void stack_tests(void)
{
  PH_STRING_DECLARE_STACK(stack1, 10);
  PH_STRING_DECLARE_GROW(stack2, 10, mt_misc);

  // Expect this to succeed; we silently truncate
  is(ph_string_append_cstr(&stack1, "12345678901"), PH_OK);
  // Ensure length is good
  is(10, ph_string_len(&stack1));

  is(ph_string_append_cstr(&stack2, "hello"), PH_OK);
  is(5, ph_string_len(&stack2));
  ok(ph_string_equal_cstr(&stack2, "hello"), "grew");

  is(ph_string_append_cstr(&stack2, "12345678901234567890"), PH_OK);
  is(25, ph_string_len(&stack2));
  ok(ph_string_equal_cstr(&stack2, "hello12345678901234567890"), "grew");

  PH_STRING_DECLARE_AND_COPY_CSTR(cstr, &stack2);
  is_string(cstr, "hello12345678901234567890");

  PH_STRING_DECLARE_CSTR_AVOID_COPY(nocopy1, &stack2);
  is_string(nocopy1, "hello12345678901234567890");
  ok(nocopy1 != stack1.buf, "copy, because we can't terminate");

  ph_string_t *slice = ph_string_make_slice(&stack2, 0, 5);
  ok(ph_string_equal_cstr(slice, "hello"), "slice is right");
  PH_STRING_DECLARE_CSTR_AVOID_COPY(nocopy2, slice);
  is_string(nocopy2, "hello");
  ok(nocopy2 != stack2.buf, "copied");
  ok(ph_string_equal_cstr(&stack2, "hello12345678901234567890"), "didn't break stack2");
  ph_string_delref(slice);

  PH_STRING_DECLARE_STACK(stack3, 32);
  ph_string_append_cstr(&stack3, "woot");
  PH_STRING_DECLARE_CSTR_AVOID_COPY(nocopy3, &stack3);
  is_string(nocopy3, "woot");
  ok(nocopy3 == stack3.buf, "didn't copy");

  ph_string_delref(&stack2);

  PH_STRING_DECLARE_STATIC(static1, "lemon cake");
  ok(ph_string_equal_cstr(&static1, "lemon cake"), "static ok");

  const char *lemon_cake = "lemon cake";
  PH_STRING_DECLARE_STATIC_CSTR(static2, lemon_cake);
  ok(ph_string_equal_cstr(&static2, "lemon cake"), "static ok");
}


static struct {
  const char *input;
  bool valid;
} unicode_strings[] = {
  { "\xe2\x82\xac\xc3\xbe\xc4\xb1\xc5\x93\xc9\x99\xc3\x9f\xc3\xb0\x20"\
    "\x73\x6f\x6d\x65\x20\x75\x74\x66\x2d\x38\x20\xc4\xb8\xca\x92\xc3"\
    "\x97\xc5\x8b\xc2\xb5\xc3\xa5\xc3\xa4\xc3\xb6\xf0\x9d\x84\x9e",
    true
  },
  { "\xed\xa2\xab surrogate half", false },
  { "\xe5 invalid UTF-8", false },
  { "\x81 lone continuation", false },
  { "\xf4\xbf\xbf\xbf not in unicode range", false },
  { "\xe0\x80\xa2 over long 3 byte", false },
  { "\xf0\x80\x80\xa2 over long 4 byte", false },
  { "\xc1 over long 1 byte", false },
  { "\xfd restricted utf-8", false },
  { "\xe0\xff truncated utf-8", false },
  { "ascii is utf-8 too!", true },
};

static struct {
  int32_t cp;
  const char *output;
} utf16_strings[] = {
  { 0x2c,   "," },
  { 0x0123, "\xc4\xa3" },
  { 0x0821, "\xe0\xa0\xa1" },
  { 0x1d11e, "\xf0\x9d\x84\x9e" },
};

static struct {
  int32_t points[2];
  int32_t cp;
  const char *encoded;
} surrogates[] = {
  { { 0xD834, 0xDD1E }, 0x1d11e, "\xed\xa0\xb4\xed\xb4\x9e" },
};

static void utf16_tests(void)
{
  uint32_t i, len, n, off;
  int32_t cp;
  PH_STRING_DECLARE_STACK(str, 64);

  for (i = 0; i < sizeof(unicode_strings)/sizeof(unicode_strings[0]); i++) {
    ph_string_reset(&str);
    is(ph_string_append_cstr(&str, unicode_strings[i].input), PH_OK);
    is(ph_string_is_valid_utf8(&str), unicode_strings[i].valid);
  }

  for (i = 0; i < sizeof(utf16_strings)/sizeof(utf16_strings[0]); i++) {
    len = strlen(utf16_strings[i].output);

    ph_string_reset(&str);
    is(ph_string_append_utf16_as_utf8(&str,
          &utf16_strings[i].cp, 1, &n), PH_OK);
    is(n, len);
    ok(ph_string_equal_cstr(&str, utf16_strings[i].output), "matches");

    // Iterate the codepoints; we should get out what we put in
    off = 0;
    is(ph_string_iterate_utf8_as_utf16(&str, &off, &cp), PH_OK);
    diag("expect %" PRIx32 " got %" PRIx32, utf16_strings[i].cp, cp);
    is(cp, utf16_strings[i].cp);

    // Round-trip; put in the output and expect to iterate and get the input cp
    ph_string_reset(&str);
    is(ph_string_append_cstr(&str, utf16_strings[i].output), PH_OK);
    ok(ph_string_is_valid_utf8(&str), "valid utf-8");
    off = 0;
    is(ph_string_iterate_utf8_as_utf16(&str, &off, &cp), PH_OK);
    diag("round-trip: expect %" PRIx32 " got %" PRIx32,
        utf16_strings[i].cp, cp);
    is(cp, utf16_strings[i].cp);
  }

  for (i = 0; i < sizeof(surrogates)/sizeof(surrogates[0]); i++) {
    ph_string_reset(&str);
    is(ph_string_append_utf16_as_utf8(&str,
          surrogates[i].points, 2, &n), PH_OK);
    is(n, strlen(surrogates[i].encoded));
    off = 0;
    // We don't do any magical transformation of surrogates, and we don't
    // consider them valid utf8 strings
    is(ph_string_iterate_utf8_as_utf16(&str, &off, &cp), PH_ERR);
  }
}

static void string_stream_tests(void)
{
  ph_string_t *str;
  ph_stream_t *stm;
  char buf[5];
  uint64_t r;

  str = ph_string_make_empty(mt_misc, 16);
  stm = ph_stm_string_open(str);
  ok(stm, "made a stream");

  ph_stm_printf(stm, "hello world");
  ok(ph_string_equal_cstr(str, "hello world"), "see printf");

  ok(ph_stm_seek(stm, 0, SEEK_SET, NULL), "rewound");
  ok(ph_stm_read(stm, buf, sizeof(buf), &r), "read data");
  is(r, sizeof(buf));
  is(memcmp(buf, "hello", 5), 0);

  ph_stm_printf(stm, " kitty and append!");
  ok(ph_string_equal_cstr(str, "hello kitty and append!"), "see printf");

  ok(ph_stm_seek(stm, 6, SEEK_SET, NULL), "rewound");
  ok(ph_stm_read(stm, buf, sizeof(buf), &r), "read data");
  is(r, sizeof(buf));
  is(memcmp(buf, "kitty", 5), 0);

  ok(ph_stm_seek(stm, -5, SEEK_END, NULL), "rewound");
  ok(ph_stm_read(stm, buf, sizeof(buf), &r), "read data");
  is(r, sizeof(buf));
  is(memcmp(buf, "pend!", 5), 0);

  ok(ph_stm_seek(stm, str->len, SEEK_SET, NULL), "seek SET to length");
  ok(ph_stm_seek(stm, 0, SEEK_END, &r), "seek to end");
  ok(!ph_stm_read(stm, buf, sizeof(buf), &r), "can't read off end");

  ph_stm_printf(stm, "add me");
  ok(ph_string_equal_cstr(str, "hello kitty and append!add me"), "appended ok");
  ok(!ph_stm_read(stm, buf, sizeof(buf), &r), "can't read off end");

  ph_stm_close(stm);
  ph_string_delref(str);
}

int main(int argc, char **argv)
{
  ph_string_t *str, *str2;

  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  plan_tests(122);

  mt_misc = ph_memtype_register(&mt_def);

  stack_tests();

  // Tests reallocation
  str = ph_string_make_empty(mt_misc, 16);
  is(ph_string_append_cstr(str, "1234567890"), PH_OK);
  is(10, ph_string_len(str));
  is(ph_string_append_cstr(str, "1234567890"), PH_OK);
  is(20, ph_string_len(str));
  is(memcmp(str->buf, "12345678901234567890", 20), 0);
  ph_string_delref(str);

  // Tests reallocation and string formatting
  str = ph_string_make_empty(mt_misc, 4);
  is(ph_string_printf(str, "Hello %s", "world"), 11);
  is(ph_string_len(str), 11);

  str2 = ph_string_make_empty(mt_misc, 10);
  is(ph_string_printf(str2, "copy `Ps%p", (void*)str), 16);
  diag(":%.*s:", str2->len, str2->buf);
  is(ph_string_len(str2), 16);
  is(memcmp(str2->buf, "copy Hello world", 16), 0);
  ph_string_delref(str2);

  str2 = ph_string_make_empty(mt_misc, 10);
  is(ph_string_printf(str2, "copy `Ps%d%p", 5, (void*)str), 10);
  is(memcmp(str2->buf, "copy Hello", 10), 0);
  ok(!ph_string_equal(str, str2), "not same");
  ph_string_delref(str2);

  str2 = ph_string_make_empty(mt_misc, 10);
  ph_string_append_buf(str2, str->buf, str->len);
  ok(ph_string_equal(str, str2), "same");
  is(ph_string_compare(str, str2), 0);

  ph_string_delref(str);
  ph_string_delref(str2);

  str = ph_string_make_cstr(mt_misc, "abc");
  str2 = ph_string_make_cstr(mt_misc, "bbc");
  ok(ph_string_compare(str, str2) < 0, "abc < bbc");
  ok(ph_string_compare(str2, str) > 0, "abc < bbc");

  ph_string_delref(str2);
  str2 = ph_string_make_cstr(mt_misc, "abca");
  ok(ph_string_compare(str, str2) < 0, "abc < abca");
  ok(ph_string_compare(str2, str) > 0, "abc < abca");

  ph_string_delref(str2);
  str2 = ph_string_make_cstr(mt_misc, "ab");
  ok(ph_string_compare(str, str2) > 0, "abc > ab");
  ok(ph_string_compare(str2, str) < 0, "abc > ab");

  ph_string_delref(str2);
  ph_string_delref(str);

  str = ph_string_make_printf(mt_misc, 16, "Hello %d", 42);
  ok(ph_string_equal_cstr(str, "Hello 42"), "same");
  ph_string_delref(str);

  utf16_tests();

  string_stream_tests();

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

