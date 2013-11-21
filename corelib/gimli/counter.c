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
#include "phenom/counter.h"
#include "corelib/gimli/gimli.h"
#include "corelib/counter.h"

struct agg_counter_scope {
  ph_counter_scope_t *scope;
  uint32_t namehash;
  int64_t values[1];
};

static struct agg_counter_scope **counter_scopes = NULL;
uint32_t num_scopes = 0;
uint32_t longest_name = 0;
static gimli_hash_t scope_hash = NULL;

static int compare_scope_name(const void *a, const void *b)
{
  const struct agg_counter_scope *A = *(const void**)a;
  const struct agg_counter_scope *B = *(const void**)b;
  int64_t diff = A->namehash - B->namehash;

  if (diff) {
    return (int)diff;
  }

  return strcmp(A->scope->full_scope_name, B->scope->full_scope_name);
}

static void collect_counter_scopes(gimli_proc_t proc)
{
  struct ph_gimli_ck_hs_iter hs_iter;
  uint32_t nscope = 0;
  ph_counter_scope_t scope_copy, *scope;
  struct agg_counter_scope *ascope;
  struct gimli_symbol *sym;
  gimli_addr_t valueptr;

  sym = gimli_sym_lookup(proc, NULL, "ph_counter_scope_map");
  if (!sym) {
    return;
  }

  if (!ph_gimli_ck_hs_iter_init(proc, sym->addr, &hs_iter)) {
    return;
  }

  num_scopes = hs_iter.size;
  counter_scopes = calloc(num_scopes, sizeof(void*));

  scope_hash = gimli_hash_new_size(NULL, GIMLI_HASH_U64_KEYS, num_scopes);

  while (ph_gimli_ck_hs_iter_next(&hs_iter, &valueptr)) {
    if (!gimli_read_mem(proc, valueptr, &scope_copy, sizeof(scope_copy))) {
      break;
    }
    // It's variable length, so figure out the correct length, then copy
    // the strings over
    uint32_t size = sizeof(scope_copy) +
                    ((scope_copy.num_slots - 1) * sizeof(char*));
    scope = malloc(size);
    if (!gimli_read_mem(proc, valueptr, scope, size)) {
      break;
    }

    ascope = calloc(1, sizeof(*ascope) + (scope->next_slot * sizeof(int64_t)));
    ascope->scope = scope;

    scope->scope_name = gimli_read_string(proc,
        (gimli_addr_t)scope->scope_name);

    scope->full_scope_name = gimli_read_string(proc,
        (gimli_addr_t)scope->full_scope_name);

    uint16_t slot;
    for (slot = 0; slot < scope->next_slot; slot++) {
      scope->slot_names[slot] = gimli_read_string(proc,
            (gimli_addr_t)scope->slot_names[slot]);

      // Compute a stupid weak 'hash' to group scopes by slot_names
      ascope->namehash += strlen(scope->slot_names[slot]);
      char *c = scope->slot_names[slot];
      while (*c) {
        ascope->namehash += *c;
        c++;
      }
    }

    counter_scopes[nscope++] = ascope;
    gimli_hash_insert_u64(scope_hash, scope->scope_id, ascope);
    longest_name = MAX(longest_name, strlen(ascope->scope->full_scope_name));
  }

  ph_gimli_ck_hs_iter_destroy(&hs_iter);

  qsort(counter_scopes, nscope, sizeof(void*), compare_scope_name);
  // It's possible that we read fewer scopes (due to memory corruption)
  num_scopes = nscope;
}

// Walk each thread; walk the counter_hs there and accumulate a count
// against the appropriate scope
static void collect_counter_values(gimli_proc_t proc)
{
  uint32_t nthreads, i;
  ph_thread_t *threads;
  struct ph_counter_block block;
  struct agg_counter_scope *ascope;
  int64_t max_slots[255];
  struct ph_gimli_ck_hs_iter hs_iter;
  gimli_addr_t addr;

  threads = ph_gimli_get_threads(proc, &nthreads);

  for (i = 0; i < nthreads; i++) {
    if (!ph_gimli_ck_hs_iter_init_from_map(proc,
        (gimli_addr_t)threads[i].counter_hs.map, &hs_iter)) {
      continue;
    }

    while (ph_gimli_ck_hs_iter_next(&hs_iter, &addr)) {
      if (!gimli_read_mem(proc, addr, &block, sizeof(block))) {
        break;
      }

      if (!gimli_hash_find_u64(scope_hash, block.scope_id, (void**)&ascope)) {
        continue;
      }

      // Block is variable size; copy the values portion locally
      if (!gimli_read_mem(proc,
            addr + ph_offsetof(struct ph_counter_block, slots),
            max_slots, ascope->scope->next_slot * sizeof(int64_t))) {
        continue;
      }

      uint16_t s;
      for (s = 0; s < ascope->scope->next_slot; s++) {
        ascope->values[s] += max_slots[s];
      }
    }

    ph_gimli_ck_hs_iter_destroy(&hs_iter);
  }
}

// Render a group of counters with the same slot_names
// Tabulates things reasonably across the values in this range
static void show_counters_in_range(int first_scope, int last_scope)
{
  struct agg_counter_scope *ascope;
  int nscope;
  int colwidths[255];
  char buf[16];
  uint16_t nslot;

  // populate widths with slot names
  ascope = counter_scopes[first_scope];
  for (nslot = 0; nslot < ascope->scope->next_slot; nslot++) {
    colwidths[nslot] = strlen(ascope->scope->slot_names[nslot]);
  }

  // make a pass through the data to find the widest numeric value(s) present
  for (nscope = first_scope; nscope <= last_scope; nscope++) {
    ascope = counter_scopes[nscope];

    for (nslot = 0; nslot < ascope->scope->next_slot; nslot++) {
      ph_snprintf(buf, sizeof(buf), "%" PRIi64, ascope->values[nslot]);
      colwidths[nslot] = MAX(colwidths[nslot], (int)strlen(buf));
    }
  }

  // Now render the labels
  char space[longest_name+1];
  memset(space, ' ', sizeof(space));

  ascope = counter_scopes[first_scope];
  printf("%.*s  ", longest_name, space);

  for (nslot = 0; nslot < ascope->scope->next_slot; nslot++) {
    printf("%*s  ",
      colwidths[nslot],
      ascope->scope->slot_names[nslot]);
  }
  printf("\n");

  // And now the data
  for (nscope = first_scope; nscope <= last_scope; nscope++) {
    ascope = counter_scopes[nscope];

    printf("%*s  ", longest_name,
        ascope->scope->full_scope_name);

    for (nslot = 0; nslot < ascope->scope->next_slot; nslot++) {
      ph_snprintf(buf, sizeof(buf), "%" PRIi64, ascope->values[nslot]);
      printf("%*s  ", colwidths[nslot], buf);
    }

    printf("\n");
  }
}

static void show_counters(gimli_proc_t proc, void *unused)
{
  ph_unused_parameter(unused);
  uint32_t first, last;

  collect_counter_scopes(proc);
  collect_counter_values(proc);

  printf("COUNTERS\n");

  for (first = 0; first < num_scopes; first = last + 1) {
    // Find the last in the sequence with the same namehash
    for (last = first + 1; last < num_scopes; last++) {
      if (counter_scopes[first]->namehash !=
          counter_scopes[last]->namehash) {
        last--;
        break;
      }
    }
    if (last == num_scopes) {
      last = num_scopes - 1;
    }

    if (counter_scopes[first]->scope->next_slot) {
      show_counters_in_range(first, last);
      printf("\n");
    }
  }
}

static void init(void) {
  gimli_module_register_tracer(show_counters, 0);
}
PH_GIMLI_INIT(init)

/* vim:ts=2:sw=2:et:
 */
