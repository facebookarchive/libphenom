/*
 * Copyright 2012 Facebook, Inc.
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
#include "phenom/counter.h"
#include "tap.h"
#include <ck_pr.h>

struct counter_name_val {
  const char *scope_name;
  const char *name;
  int64_t val;
};

static int compare_counter_name_val(const void *a, const void *b)
{
  struct counter_name_val *A = (struct counter_name_val*)a;
  struct counter_name_val *B = (struct counter_name_val*)b;
  int diff;

  diff = strcmp(A->scope_name, B->scope_name);
  if (diff) return diff;

  return strcmp(A->name, B->name);
}

static void basicCounterFunctionality(void)
{
  ph_counter_scope_t *scope;

  scope = ph_counter_scope_define(NULL, "test1", 24);
  is_true(scope != NULL);
  is_string("test1", ph_counter_scope_get_name(scope));

  uint8_t slot = ph_counter_scope_register_counter(scope, "dummy");
  is(0, slot);

  ph_counter_scope_add(scope, slot, 1);
  is(1, ph_counter_scope_get(scope, slot));

  ph_counter_scope_add(scope, slot, 1);
  is(2, ph_counter_scope_get(scope, slot));

  ph_counter_scope_add(scope, slot, 3);
  is(5, ph_counter_scope_get(scope, slot));

  /* register some more slots */
  const char *names[2] = {"sent", "recd"};
  is_true(ph_counter_scope_register_counter_block(
        scope, 2, slot + 1, names));

  ph_counter_block_t *block = ph_counter_block_open(scope);
  is_true(block != NULL);

  ph_counter_block_add(block, slot + 1, 3);
  is(3, ph_counter_scope_get(scope, slot + 1));

  // C++, clogging up code with casts since the last century
  uint8_t bulk_slots[2] = { (uint8_t)(slot + 1), (uint8_t)(slot + 2) };
  int64_t values[2] = { 1, 5 };
  ph_counter_block_bulk_add(block, 2, bulk_slots, values);
  is(4, ph_counter_scope_get(scope, slot + 1));
  is(5, ph_counter_scope_get(scope, slot + 2));

  uint8_t num_slots;
  int64_t view_slots[10];
  const char *view_names[10];

  num_slots = ph_counter_scope_get_view(scope, 10, view_slots, view_names);
  is(3, num_slots);
  is(5, view_slots[0]);
  is(4, view_slots[1]);
  is(5, view_slots[2]);
  is_string("dummy", view_names[0]);
  is_string("sent", view_names[1]);
  is_string("recd", view_names[2]);

  ph_counter_scope_t *kid_scope;

  // Verify that attempting to define the same scope twice fails
  kid_scope = ph_counter_scope_define(NULL, "test1", 24);
  is_true(kid_scope == NULL);

  // Get ourselves a real child
  kid_scope = ph_counter_scope_define(scope, "child", 8);
  is_true(kid_scope != NULL);
  is_string("test1.child",
      ph_counter_scope_get_name(kid_scope));

  ph_counter_scope_t *resolved;

  resolved = ph_counter_scope_resolve(NULL, "test1");
  is(scope, resolved);

  resolved = ph_counter_scope_resolve(NULL, "test1.child");
  is(kid_scope, resolved);

  ph_counter_scope_register_counter(kid_scope, "w00t");

  // Test iteration
  struct counter_name_val counter_data[16];
  int n_counters = 0;
  ph_counter_scope_iterator_t iter;

  // Collect all counter data; it is returned in an undefined order.
  // For the sake of testing we want to order it, so we collect the data
  // and then sort it
  ph_counter_scope_iterator_init(&iter);
  ph_counter_scope_t *iter_scope;
  while ((iter_scope = ph_counter_scope_iterator_next(&iter)) != NULL) {
    int i;

    if (strncmp(ph_counter_scope_get_name(iter_scope), "test1", 5)) {
      continue;
    }

    num_slots = ph_counter_scope_get_view(iter_scope, 10,
        view_slots, view_names);

    for (i = 0; i < num_slots; i++) {
      counter_data[n_counters].scope_name =
        ph_counter_scope_get_name(iter_scope);
      counter_data[n_counters].name = view_names[i];
      counter_data[n_counters].val = view_slots[i];
      n_counters++;
    }

    ph_counter_scope_delref(iter_scope);
  }

  qsort(counter_data, n_counters, sizeof(struct counter_name_val),
      compare_counter_name_val);

  struct counter_name_val expected_data[] = {
    { "test1", "dummy", 5 },
    { "test1", "recd", 5 },
    { "test1", "sent", 4 },
    { "test1.child", "w00t", 0 },
  };
  int num_expected = sizeof(expected_data) / sizeof(expected_data[0]);

  is_int(num_expected, n_counters);

  for (int i = 0; i < n_counters; i++) {
    is_string(expected_data[i].scope_name,
        counter_data[i].scope_name);
    is_string(expected_data[i].name,
        counter_data[i].name);
    is(expected_data[i].val, counter_data[i].val);

    diag("%s.%s = %" PRIi64, counter_data[i].scope_name,
        counter_data[i].name, counter_data[i].val);
  }
}

struct counter_data {
  unsigned int barrier;
  unsigned int iters;
  uint8_t slot;
  ph_counter_scope_t *scope;
};

static void *spin_and_count(void *ptr)
{
  struct counter_data *data = (struct counter_data*)ptr;
  ph_counter_block_t *block;
  uint32_t i;

  // Since we're not already a phenom thread, we need to
  // make sure that we get initialized.  We only really
  // need to invoke ph_thread_self_slow() here, but that
  // feels a bit weird, so we go with the more honest
  // call to ph_library_init() to make it clear that we're
  // being initialized.
  ph_library_init();

  while (ck_pr_load_uint(&data->barrier) == 0);

  block = ph_counter_block_open(data->scope);

  for (i = 0; i < data->iters; i++) {
    ph_counter_block_add(block, data->slot, 1);
  }

  ph_counter_block_delref(block);

  return NULL;
}

// want to verify that we can observe the correct end counter value
// in the face of concurrent updates
static void concurrentCounters(void)
{
  int i, num_threads = 4;
  struct counter_data data = { 0, 100000, 0, NULL };
  pthread_t threads[num_threads];

  data.scope = ph_counter_scope_define(NULL, "testConcurrentCounters", 1);
  is_true(data.scope != NULL);

  data.slot = ph_counter_scope_register_counter(data.scope, "dummy");
  is(0, data.slot);

  for (i = 0; i < num_threads; i++) {
    pthread_create(&threads[i], NULL, spin_and_count, &data);
  }

  /* unleash the threads on the data */
  ck_pr_store_uint(&data.barrier, 1);

  for (i = 0; i < num_threads; i++) {
    void *unused;
    pthread_join(threads[i], &unused);
  }

  is(num_threads * data.iters,
      ph_counter_scope_get(data.scope, data.slot));
}


int main(int argc, char** argv)
{
  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  plan_tests(39);

  ph_assert(true, "always true");
  basicCounterFunctionality();
  concurrentCounters();

  return exit_status();
}


/* vim:ts=2:sw=2:et:
 */

