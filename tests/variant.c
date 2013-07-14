#include "phenom/sysutil.h"
#include "phenom/string.h"
#include "phenom/variant.h"
#include "phenom/json.h"
#include "tap.h"

static ph_memtype_def_t mt_def = { "test", "misc", 0, 0 };
static ph_memtype_t mt_misc;

/* JSON test cases derived from work that is:
 * Copyright (c) 2009-2012 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */
static struct {
  const char *json;
  const char *err;
  uint32_t len;
} json_tests[] = {
  { "42", NULL, 0 },
  { "{hello:123}", "string or '}' expected near 'hello'", 0 },
  { "{\"hello\": 123}", NULL, 0 },
  { "{\"a\": 1, \"b\": 2}", NULL, 0 },
  { "{\"bar\": 42, \"foo\": true}", NULL, 0 },
  { "[null, true, false, 65536]", NULL, 0 },
  { "[1.5, 2.0]", NULL, 0 },
  { "[{\"lemon\": 2.5}, null, 16000, true, false]", NULL, 0 },
  { "[1, 16000, 65536, 90000, 2147483648, 4294967295]", NULL, 0 },
  { "[]", NULL, 0 },
  { "[1, 2, 3]", NULL, 0 },
  // empty
  { "",   "unexpected token near end of file", 0 },
  { "null", NULL, 0},
  // apostrophe
  { "['",   "invalid token near '''", 0 },
  // ascii-unicode-identifier
  { "a\303\245",   "invalid token near 'a'", 0 },
  // brace-comma
  { "{,",   "string or '}' expected near ','", 0 },
  // bracket-comma
  { "[,",   "unexpected token near ','", 0 },
  // bracket-one-comma
  { "[1,",   "']' expected near end of file", 0 },
  // escaped-null-byte-in-string
  { "[\"\\u0000 (null byte not allowed)\"]",   "\\u0000 is not allowed", 0 },
  // extra-comma-in-array
  { "[1,]",   "unexpected token near ']'", 0 },
  // extra-comma-in-multiline-array
  { "[1,\n2,\n3,\n4,\n5,\n]",   "unexpected token near ']'", 0 },
  // garbage-after-newline
  { "[1,2,3]\nfoo",   "end of file expected near 'foo'", 0 },
  // garbage-at-the-end
  { "[1,2,3]foo",   "end of file expected near 'foo'", 0 },
  // integer-starting-with-zero
  { "[012]",   "invalid token near '0'", 0 },
  // invalid-escape
  { "[\"\\a <-- invalid escape\"]",   "invalid escape near '\"\\a'", 0 },
  // invalid-identifier
  { "[troo",   "invalid token near 'troo'", 0 },
  // invalid-negative-integer
  { "[-123foo]",   "']' expected near 'foo'", 0 },
  // invalid-negative-real
  { "[-123.123foo]",   "']' expected near 'foo'", 0 },
  // invalid-second-surrogate
  { "[\"\\uD888\\u3210 (first surrogate and invalid second surrogate)\"]",
    "invalid Unicode '\\uD888\\u3210'", 0 },
  // lone-open-brace
  { "{",   "string or '}' expected near end of file", 0 },
  // lone-open-bracket
  { "[",   "']' expected near end of file", 0 },
  // lone-second-surrogate
  { "[\"\\uDFAA (second surrogate on it's own)\"]",
    "invalid Unicode '\\uDFAA'", 0 },
  // minus-sign-without-number
  { "[-foo]",   "invalid token near '-'", 0 },
  // negative-integer-starting-with-zero
  { "[-012]",   "invalid token near '-0'", 0 },
  // object-apostrophes
  { "{'a'",   "string or '}' expected near '''", 0 },
  // object-garbage-at-end
  { "{\"a\":\"a\" 123}",   "'}' expected near '123'", 0 },
  // object-in-unterminated-array
  { "[{}",   "']' expected near end of file", 0 },
  // object-no-colon
  { "{\"a\"",   "':' expected near end of file", 0 },
  // object-no-value
  { "{\"a\":",   "unexpected token near end of file", 0 },
  // object-unterminated-value
  { "{\"a\":\"a\n",   "unexpected newline near '\"a'", 0 },
  // real-garbage-after-e
  { "[1ea]",   "invalid token near '1e'", 0 },
  // real-negative-overflow
  { "[-123123e100000]",   "real number overflow near '-123123e100000'", 0 },
  // real-positive-overflow
  { "[123123e100000]",   "real number overflow near '123123e100000'", 0 },
  // real-truncated-at-e
  { "[1e]",   "invalid token near '1e'", 0 },
  // real-truncated-at-point
  { "[1.]",   "invalid token near '1.'", 0 },
  // tab-character-in-string
  { "[\"\t <-- tab character\"]",   "control character 0x9 near '\"'", 0 },
  // too-big-negative-integer
  { "[-123123123123123123123123123123]",   "too big negative integer", 0 },
  // too-big-positive-integer
  { "[123123123123123123123123123123]",   "too big integer", 0 },
  // truncated-unicode-surrogate
  { "[\"\\uDADA (first surrogate without the second)\"]",
    "invalid Unicode '\\uDADA'", 0 },
  // unicode-identifier
  { "\303\245",   "invalid token near '\303\245'", 0 },
  // unterminated-array
  { "[\"a\"",   "']' expected near end of file", 0 },
  // unterminated-array-and-object
  { "[{",   "string or '}' expected near end of file", 0 },
  // unterminated-empty-key
  { "{\"\n",   "unexpected newline near '\"'", 0 },
  // unterminated-key
  { "{\"a\n",   "unexpected newline near '\"a'", 0 },
  // unterminated-object-and-array
  { "{[",   "string or '}' expected near '['", 0 },
  // unterminated-string
  { "[\"a\n",   "unexpected newline near '\"a'", 0 },
  // null-byte-in-string
  { "[\"null byte \000 not allowed\"]",
    "control character 0x0 near '\"null byte '", 27 },
  // null-byte-outside-string
  { "[\000",   "invalid token near end of file", 2 },
};

static struct {
  const char *json;
  const char *expect;
} json_tests_2[] = {

  // complex-array
  { "[1,2,3,4,\n\"a\", \"b\", \"c\",\n{\"foo\": \"bar\","
    "\"core\": \"dump\"},\ntrue, false, true, true, null, false\n]",
    "[1, 2, 3, 4, \"a\", \"b\", \"c\", {\"core\": \"dump\","
    " \"foo\": \"bar\"}, true, false, true, true, null, false]"
  },
  // empty-array
  { "[]", NULL },
  // empty-object
  { "{}", NULL },
  // empty-object-in-array
  { "[{}]", NULL },
  // empty-string
  { "[\"\"]", NULL },
  // escaped-utf-control-char
  { "[\"\\u0012 escaped control character\"]", NULL },
  // false
  { "[false]", NULL },
  // negative-int
  { "[-123]", NULL },
  // negative-one
  { "[-1]", NULL },
  // negative-zero
  { "[-0]", "[0]" },
  // null
  { "[null]", NULL },
  // one-byte-utf-8
  { "[\"\\u002c one-byte UTF-8\"]", "[\", one-byte UTF-8\"]" },
  // real-capital-e
  { "[1E22]", "[1e22]" },
  // real-capital-e-negative-exponent
  { "[1E-2]", "[0.01]" },
  // real-capital-e-positive-exponent
  { "[1E+2]", "[100.0]" },
  // real-exponent
  { "[123e45]", "[1.2299999999999999e47]" },
  // real-fraction-exponent
  { "[123.456e78]", "[1.23456e80]" },
  // real-negative-exponent
  { "[1e-2]", "[0.01]" },
  // real-positive-exponent
  { "[1e+2]", "[100.0]" },
  // real-underflow
  { "[123e-10000000]", "[0.0]" },
  // short-string
  { "[\"a\"]", NULL },
  // simple-ascii-string
  { "[\"abcdefghijklmnopqrstuvwxyz1234567890 \"]", NULL },
  // simple-int-0
  { "[0]", NULL },
  // simple-int-1
  { "[1]", NULL },
  // simple-int-123
  { "[123]", NULL },
  // simple-object
  { "{\"a\":[]}", "{\"a\": []}" },
  // simple-real
  { "[123.456789]", NULL },
  // string-escapes
  { "[\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"]", NULL },
  // three-byte-utf-8
  { "[\"\\u0821 three-byte UTF-8\"]", "[\"\xe0\xa0\xa1 three-byte UTF-8\"]" },
  // true
  { "[true]", NULL },
  // two-byte-utf-8
  { "[\"\\u0123 two-byte UTF-8\"]", "[\"\xc4\xa3 two-byte UTF-8\"]" },
  // utf-surrogate-four-byte-encoding
  { "[\"\\uD834\\uDD1E surrogate, four-byte UTF-8\"]",
    "[\"\xf0\x9d\x84\x9e surrogate, four-byte UTF-8\"]" },
};

static void test_json(void)
{
  ph_variant_t *v;
  ph_json_err_t err;
  uint32_t i;
  PH_STRING_DECLARE_GROW(dumpstr, 128, mt_misc);

  ph_stm_init();

  // Couple of one-offs to test actual object values
  v = ph_json_load_cstr("42", 0, &err);
  is(v, 0); // Default is to parse objects or arrays

  v = ph_json_load_cstr("42", PH_JSON_DECODE_ANY, &err);
  is(ph_var_is_int(v), true);
  is(ph_var_int_val(v), 42);
  ph_var_delref(v);

  // Rest of these load data from json_tests, create
  // variants, then try to dump them, and compare with
  // the original text.  If they compare the same, all
  // is good.

  for (i = 0; i < sizeof(json_tests)/sizeof(json_tests[0]); i++) {
    ph_string_t str;
    uint32_t len = json_tests[i].len;

    if (!len) {
      len = strlen(json_tests[i].json);
    }
    ph_string_init_claim(&str, PH_STRING_STATIC,
        (char*)json_tests[i].json, len, len);

    diag("load %.*s", len, json_tests[i].json);
    v = ph_json_load_string(&str, PH_JSON_DECODE_ANY, &err);
    if (json_tests[i].err) {
      is(v, 0);
      is_string(json_tests[i].err, err.text);
      continue;
    }

    ok(v, "parsed");
    if (!v) {
      diag("failed: %s", err.text);
      continue;
    }

    ph_string_reset(&dumpstr);
    is(ph_json_dump_string(v, &dumpstr, PH_JSON_SORT_KEYS), PH_OK);
    diag("dumped %.*s", dumpstr.len, dumpstr.buf);
    ok(ph_string_equal_cstr(&dumpstr, json_tests[i].json), "roundtrip");

    ph_var_delref(v);
  }

  // Test parsing across newlines etc.  These need to specify input and
  // desired output
  for (i = 0; i < sizeof(json_tests_2)/sizeof(json_tests_2[0]); i++) {
    const char *expect;

    diag("load %s", json_tests_2[i].json);
    v = ph_json_load_cstr(json_tests_2[i].json, PH_JSON_DECODE_ANY, &err);
    ok(v, "parsed");
    if (!v) {
      diag("failed: %s", err.text);
      continue;
    }

    ph_string_reset(&dumpstr);
    is(ph_json_dump_string(v, &dumpstr,
          PH_JSON_SORT_KEYS|PH_JSON_ESCAPE_SLASH), PH_OK);
    diag("dumped %.*s", dumpstr.len, dumpstr.buf);
    expect = json_tests_2[i].expect;
    if (!expect) {
      expect = json_tests_2[i].json;
    }
    ok(ph_string_equal_cstr(&dumpstr, expect), "matched");

    ph_var_delref(v);
  }
  ph_string_delref(&dumpstr);

}

int main(int argc, char **argv)
{
  uint32_t i;
  ph_variant_t *v, *arr, *obj;
  ph_ht_ordered_iter_t oiter;
  ph_ht_iter_t iter;
  ph_string_t *str, *k;
  unused_parameter(argc);
  unused_parameter(argv);

  plan_tests(299);

  mt_misc = ph_memtype_register(&mt_def);

  v = ph_var_bool(true);
  ok(v, "got bool");
  is(ph_var_is_boolean(v), true);
  is(ph_var_bool_val(v), true);
  ph_var_delref(v);

  v = ph_var_bool(false);
  ok(v, "got bool");
  is(ph_var_is_boolean(v), true);
  is(ph_var_bool_val(v), false);
  ph_var_delref(v);

  v = ph_var_null();
  ok(v, "got null");
  is(ph_var_is_null(v), true);
  ph_var_delref(v);

  v = ph_var_int(42);
  ok(v, "got int");
  is(ph_var_is_null(v), false);
  is(ph_var_is_int(v), true);
  is(ph_var_int_val(v), 42);
  ph_var_delref(v);

  v = ph_var_double(2.5);
  ok(v, "got double");
  is(ph_var_is_double(v), true);
  is(ph_var_double_val(v), 2.5);
  ph_var_delref(v);

  v = ph_var_string_claim(ph_string_make_cstr(mt_misc, "hello"));
  ok(v, "got string");
  is(ph_var_is_string(v), true);
  ok(ph_string_equal_cstr(ph_var_string_val(v), "hello"), "compare ok");
  ph_var_delref(v);

  str = ph_string_make_cstr(mt_misc, "w00t");
  v = ph_var_string_make(str);
  is(ph_var_is_string(v), true);
  ok(ph_string_equal_cstr(ph_var_string_val(v), "w00t"), "compare ok");
  ph_var_delref(v);
  ph_string_delref(str);

  // Test creation and growing of arrays
  arr = ph_var_array(0);
  is(ph_var_array_size(arr), 0);
  is(ph_var_is_array(arr), true);

  for (i = 0; i < 16; i++) {
    is(ph_var_array_append_claim(arr, ph_var_int(i)), PH_OK);
  }
  is(ph_var_array_size(arr), 16);

  for (i = 0; i < ph_var_array_size(arr); i++) {
    v = ph_var_array_get(arr, i);
    is(ph_var_int_val(v), i);
  }

  v = ph_var_int(42);
  // Can't create a hole
  is(ph_var_array_set(arr, 17, v), PH_NOENT);
  // But can set an existing value
  is(ph_var_array_set(arr, 4, v), PH_OK);
  is(ph_var_int_val(ph_var_array_get(arr, 4)), 42);
  // And can grow it by one
  is(ph_var_array_set(arr, 16, v), PH_OK);
  is(ph_var_int_val(ph_var_array_get(arr, 16)), 42);
  ph_var_delref(v);
  ph_var_delref(arr);

  obj = ph_var_object(0);
  ok(obj, "made object");
  is(ph_var_object_size(obj), 0);

  ph_var_object_set_claim_kv(obj,
      ph_string_make_cstr(mt_misc, "k1"),
      ph_var_int(2)
  );
  is(ph_var_object_size(obj), 1);

  str = ph_string_make_cstr(mt_misc, "not here");
  v = ph_var_object_get(obj, str);
  is(v, NULL);
  ph_string_delref(str);

  str = ph_string_make_cstr(mt_misc, "k1");
  v = ph_var_object_get(obj, str);
  is(ph_var_int_val(v), 2);

  ph_var_object_del(obj, str);
  is(ph_var_object_size(obj), 0);

  ph_string_delref(str);

  ph_var_object_set_claim_kv(obj,
      ph_string_make_cstr(mt_misc, "alpha"),
      ph_var_int(0)
  );
  ph_var_object_set_claim_kv(obj,
      ph_string_make_cstr(mt_misc, "beta"),
      ph_var_int(1)
  );

  if (ph_var_object_ordered_iter_first(obj, &oiter, &k, &v)) {
    ok(ph_string_equal_cstr(k, "alpha"), "alpha first");
    is(ph_var_int_val(v), 0);
  }
  if (ph_var_object_ordered_iter_next(obj, &oiter, &k, &v)) {
    ok(ph_string_equal_cstr(k, "beta"), "beta next");
    is(ph_var_int_val(v), 1);

    // Delete this one, so that we can try the unordered iterator
    // and ensure it has a single iteration
    is(ph_var_object_del(obj, k), PH_OK);
  }
  ph_var_object_ordered_iter_end(obj, &oiter);

  if (ph_var_object_iter_first(obj, &iter, &k, &v)) do {
    ok(ph_string_equal_cstr(k, "alpha"), "alpha first");
    is(ph_var_int_val(v), 0);
  } while (ph_var_object_iter_next(obj, &iter, &k, &v));

  ph_var_delref(obj);

  test_json();

  return exit_status();
}


/* vim:ts=2:sw=2:et:
 */

