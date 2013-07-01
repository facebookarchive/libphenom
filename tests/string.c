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
#include "phenom/string.h"
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

  is(ph_string_append_cstr(&stack2, "12345678901234567890"), PH_OK);
  is(20, ph_string_len(&stack2));
  ph_string_delref(&stack2);
}

int main(int argc, char **argv)
{
  ph_string_t *str, *str2;

  unused_parameter(argc);
  unused_parameter(argv);

  plan_tests(26);

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

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

