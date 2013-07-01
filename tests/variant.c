#include "phenom/sysutil.h"
#include "phenom/string.h"
#include "phenom/variant.h"
#include "tap.h"

static ph_memtype_def_t mt_def = { "test", "misc", 0, 0 };
static ph_memtype_t mt_misc;

int main(int argc, char **argv)
{
  uint32_t i;
  ph_variant_t *v, *arr, *obj;
  ph_ht_ordered_iter_t oiter;
  ph_ht_iter_t iter;
  ph_string_t *str, *k;
  unused_parameter(argc);
  unused_parameter(argv);

  plan_tests(73);

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


  return exit_status();
}


/* vim:ts=2:sw=2:et:
 */

