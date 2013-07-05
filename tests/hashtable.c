#include "phenom/sysutil.h"
#include "phenom/string.h"
#include "phenom/hashtable.h"
#include "tap.h"

static ph_memtype_def_t mt_def = { "test", "misc", 0, 0 };
static ph_memtype_t mt_misc = 0;
static union {
  void *ptr;
  ph_string_t **str;
} kptr, vptr;

static void load_data(ph_ht_t *ht, uint32_t howmany)
{
  ph_string_t **data;
  uint32_t i;
  ph_ht_ordered_iter_t oiter;

  ph_ht_free_entries(ht);

  if (howmany > 1000) {
    // need to fix the format to allow for this many
    abort();
  }

  data = calloc(howmany, sizeof(*data));
  for (i = 0; i < howmany; i++) {
    data[i] = ph_string_make_printf(mt_misc, 16, "key:%04" PRIu32, i);

    ph_ht_set(ht, &data[i], &data[i]);
  }

  is(ph_ht_size(ht), howmany);

  i = 0;
  if (ph_ht_ordered_iter_first(ht, &oiter, &kptr.ptr, &vptr.ptr)) do {
    ok(ph_string_equal(*kptr.str, *vptr.str), "key == val");
    ok(ph_string_equal(*kptr.str, data[i]), "key is correct");
    i++;
  } while (ph_ht_ordered_iter_next(ht, &oiter, &kptr.ptr, &vptr.ptr));
  ph_ht_ordered_iter_end(ht, &oiter);

  for (i = 0; i < howmany; i++) {
    ph_string_delref(data[i]);
  }
  free(data);
}

int main(int argc, char **argv)
{
  ph_ht_t ht;
  ph_string_t *a, *b;
  ph_ht_ordered_iter_t oiter;
  ph_ht_iter_t iter;

  unused_parameter(argc);
  unused_parameter(argv);

  plan_tests(217);

  mt_misc = ph_memtype_register(&mt_def);

  ok(ph_ht_init(&ht, 1, &ph_ht_string_key_def,
        &ph_ht_string_val_def) == PH_OK, "init");
  a = ph_string_make_cstr(mt_misc, "one");
  b = ph_string_make_cstr(mt_misc, "ONE");
  ok(ph_ht_insert(&ht, &a, &b, PH_HT_CLAIM) == PH_OK, "claim one");
  is(1, ph_ht_size(&ht));

  if (ph_ht_ordered_iter_first(&ht, &oiter, &kptr.ptr, &vptr.ptr)) do {
    ok(ph_string_equal_cstr(*kptr.str, "one"), "key is one");
    ok(ph_string_equal_cstr(*vptr.str, "ONE"), "val is ONE");
  } while (ph_ht_ordered_iter_next(&ht, &oiter, &kptr.ptr, &vptr.ptr));
  ph_ht_ordered_iter_end(&ht, &oiter);

  if (ph_ht_iter_first(&ht, &iter, &kptr.ptr, &vptr.ptr)) do {
    ok(ph_string_equal_cstr(*kptr.str, "one"), "key is one");
    ok(ph_string_equal_cstr(*vptr.str, "ONE"), "val is ONE");
  } while (ph_ht_iter_next(&ht, &iter, &kptr.ptr, &vptr.ptr));

  a = ph_string_make_cstr(mt_misc, "two");
  b = ph_string_make_cstr(mt_misc, "TWO");
  ok(ph_ht_set(&ht, &a, &b) == PH_OK, "set two");
  is(2, ph_ht_size(&ht));

  // Verify that we can't `set` the same key twice (need to use replace)
  ph_string_delref(b);
  b = ph_string_make_cstr(mt_misc, "not me");
  is(ph_ht_set(&ht, &a, &b), PH_EXISTS);
  // keep b around; we'll check replacing after this iteration

  if (ph_ht_ordered_iter_first(&ht, &oiter, &kptr.ptr, &vptr.ptr)) {
    ok(ph_string_equal_cstr(*kptr.str, "one"), "key is one");
    ok(ph_string_equal_cstr(*vptr.str, "ONE"), "val is ONE");
    diag("vptr = %p *vptr=%p", vptr.ptr, *vptr.str);
  }
  if (ph_ht_ordered_iter_next(&ht, &oiter, &kptr.ptr, &vptr.ptr)) {
    ok(ph_string_equal_cstr(*kptr.str, "two"), "key is two");
    ok(ph_string_equal_cstr(*vptr.str, "TWO"), "val is TWO");
  }
  ph_ht_ordered_iter_end(&ht, &oiter);

  ok(ph_ht_replace(&ht, &a, &b) == PH_OK, "replace two");
  ph_string_delref(b);

  // Now to verify that we handle freeing keys properly on collision.
  // valgrind should run clean
  b = ph_string_make_cstr(mt_misc, "not me either");
  is(ph_ht_insert(&ht, &a, &b, PH_HT_CLAIM), PH_EXISTS);
  ok(ph_ht_insert(&ht, &a, &b,
        PH_HT_CLAIM|PH_HT_REPLACE) == PH_OK, "replaced");
  // `a` and `b` have been claimed by the hash table; we mustn't free
  // them here.

  // life isn't about iterating....
  a = ph_string_make_cstr(mt_misc, "one");
  ok(ph_ht_lookup(&ht, &a, &b, true) == PH_OK, "lookup one");
  ok(ph_string_equal_cstr(b, "ONE"), "one -> ONE");
  ph_string_delref(b);
  b = NULL;

  // lookup without copy
  ok(ph_ht_lookup(&ht, &a, &b, false) == PH_OK, "lookup one");
  ok(ph_string_equal_cstr(b, "ONE"), "one -> ONE");

  // get: lookup without copy, but return the address of the val
  vptr.ptr = ph_ht_get(&ht, &a);
  ok(vptr.ptr, "got something");
  ok(ph_string_equal_cstr(*vptr.str, "ONE"), "one -> ONE");

  // Try some deleting
  is(ph_ht_del(&ht, &a), PH_OK);
  is(ph_ht_get(&ht, &a), 0);
  is(ph_ht_size(&ht), 1);
  is(ph_ht_del(&ht, &a), PH_NOENT);

  if (ph_ht_iter_first(&ht, &iter, &kptr.ptr, &vptr.ptr)) do {
    ok(ph_string_equal_cstr(*kptr.str, "two"), "key is two");
    ok(ph_string_equal_cstr(*vptr.str, "not me either"), "val not me either");
  } while (ph_ht_iter_next(&ht, &iter, &kptr.ptr, &vptr.ptr));

  ph_string_delref(a);

  load_data(&ht, 4);
  load_data(&ht, 8);
  load_data(&ht, 16);
  load_data(&ht, 64);

  ph_ht_destroy(&ht);

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

