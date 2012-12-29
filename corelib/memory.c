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

#include "phenom/memory.h"
#include "phenom/counter.h"
#include <ck_pr.h>

struct mem_type {
  phenom_memtype_def_t def;
  phenom_counter_scope_t *scope;
  uint8_t first_slot;
};

#define HEADER_RESERVATION 16
struct sized_header {
  phenom_memtype_t mt;
  uint64_t size;
} CK_CC_ALIGN(HEADER_RESERVATION);

static uint32_t memtypes_size = 0;
static phenom_memtype_t next_memtype = 0;

static struct mem_type *memtypes = NULL;
static phenom_counter_scope_t *memory_scope = NULL;
static pthread_once_t memory_once_init = PTHREAD_ONCE_INIT;

static const char *sized_counter_names[] = {
  "bytes",   // current number of allocated bytes
  "oom",     // total number of times allocation failed
  "allocs",  // total number of successful allocation calls
  "frees",   // total number of free calls
};

static const char *vsize_counter_names[] = {
  "bytes",   // current number of allocated bytes
  "oom",     // total number of times allocation failed
  "allocs",  // total number of successful allocation calls
  "frees",   // total number of free calls
  "realloc", // total number of successful realloc calls
};

#define SLOT_BYTES 0
#define SLOT_OOM 1
#define SLOT_ALLOCS 2
#define SLOT_FREES 3
#define SLOT_REALLOC 4

#define MEM_COUNTER_SLOTS 5

static void memory_init(void)
{
  memtypes_size = 1024;
  memtypes = malloc(memtypes_size * sizeof(*memtypes));
  if (!memtypes) {
    abort();
  }

  memory_scope = phenom_counter_scope_define(NULL, "memory", 16);
  if (!memory_scope) {
    abort();
  }
}

static phenom_counter_scope_t *resolve_facility(const char *fac)
{
  phenom_counter_scope_t *scope;

  scope = phenom_counter_scope_resolve(memory_scope, fac);
  if (scope) {
    return scope;
  }

  return phenom_counter_scope_define(memory_scope, fac, 0);
}

phenom_memtype_t phenom_memtype_register(const phenom_memtype_def_t *def)
{
  phenom_memtype_t mt;
  phenom_counter_scope_t *scope, *fac_scope;
  struct mem_type *mem_type;
  const char **names;
  int num_slots;

  pthread_once(&memory_once_init, memory_init);

  fac_scope = resolve_facility(def->facility);
  if (!fac_scope) {
    return PHENOM_MEMTYPE_INVALID;
  }
  scope = phenom_counter_scope_define(fac_scope, def->name,
      MEM_COUNTER_SLOTS);
  phenom_counter_scope_delref(fac_scope);

  if (!scope) {
    return PHENOM_MEMTYPE_INVALID;
  }

  if ((uint32_t)next_memtype + 1 >= memtypes_size) {
    // TODO: grow the array
    return PHENOM_MEMTYPE_INVALID;
  }

  mt = ck_pr_faa_int(&next_memtype, 1);
  mem_type = &memtypes[mt];
  memset(mem_type, 0, sizeof(*mem_type));

  mem_type->def = *def;
  mem_type->def.facility = strdup(def->facility);
  mem_type->def.name = strdup(def->name);
  mem_type->scope = scope;

  if (mem_type->def.item_size == 0) {
    names = vsize_counter_names;
    num_slots = MEM_COUNTER_SLOTS;
  } else {
    names = sized_counter_names;
    num_slots = MEM_COUNTER_SLOTS - 1;
  }
  if (!phenom_counter_scope_register_counter_block(
      scope, num_slots, 0, names)) {
    abort();
  }

  return mt;
}

phenom_memtype_t phenom_memtype_register_block(
    uint8_t num_types,
    const phenom_memtype_def_t *defs,
    phenom_memtype_t *types)
{
  int i;
  phenom_counter_scope_t *fac_scope, *scope = NULL;
  phenom_memtype_t mt;
  struct mem_type *mem_type;
  const char **names;
  uint32_t num_slots;

  pthread_once(&memory_once_init, memory_init);

  /* must all be same facility */
  for (i = 0; i < num_types; i++) {
    if (strcmp(defs[0].facility, defs[i].facility)) {
      return PHENOM_MEMTYPE_INVALID;
    }
  }

  if ((uint32_t)next_memtype + num_types >= memtypes_size) {
    // TODO: grow the array
    return PHENOM_MEMTYPE_INVALID;
  }

  fac_scope = resolve_facility(defs[0].facility);
  if (!fac_scope) {
    return PHENOM_MEMTYPE_INVALID;
  }

  mt = ck_pr_faa_int(&next_memtype, num_types);

  for (i = 0; i < num_types; i++) {
    mem_type = &memtypes[mt + i];
    memset(mem_type, 0, sizeof(*mem_type));

    mem_type->def = defs[i];
    if (i == 0) {
      mem_type->def.facility = strdup(defs[0].facility);
    } else {
      mem_type->def.facility = memtypes[mt].def.facility;
    }
    mem_type->def.name = strdup(defs[i].name);

    scope = phenom_counter_scope_define(fac_scope, mem_type->def.name,
        MEM_COUNTER_SLOTS);
    if (!scope) {
      // FIXME: cleaner error handling
      return PHENOM_MEMTYPE_INVALID;
    }
    mem_type->scope = scope;

    if (mem_type->def.item_size == 0) {
      names = vsize_counter_names;
      num_slots = MEM_COUNTER_SLOTS;
    } else {
      names = sized_counter_names;
      num_slots = MEM_COUNTER_SLOTS - 1;
    }
    mem_type->first_slot = phenom_counter_scope_get_num_slots(scope);
    if (!phenom_counter_scope_register_counter_block(
          scope, num_slots, 0, names)) {
      abort();
    }
  }

  if (types) {
    for (i = 0; i < num_types; i++) {
      types[i] = mt + i;
    }
  }

  return mt;
}

static inline struct mem_type *resolve_mt(phenom_memtype_t mt)
{
  if (mt < 0 || mt >= next_memtype) {
    abort();
  }
  return &memtypes[mt];
}

void *phenom_mem_alloc(phenom_memtype_t mt)
{
  struct mem_type *mem_type = resolve_mt(mt);
  void *ptr;
  phenom_counter_block_t *block;
  int64_t values[3];
  static const uint8_t slots[2] = {
    SLOT_BYTES, SLOT_ALLOCS
  };

  if (mem_type->def.item_size == 0) {
    abort();
    return NULL;
  }

  ptr = malloc(mem_type->def.item_size);
  if (!ptr) {
    phenom_counter_scope_add(mem_type->scope,
        mem_type->first_slot + SLOT_OOM, 1);
    return NULL;
  }

  block = phenom_counter_block_open(mem_type->scope);
  values[0] = mem_type->def.item_size;
  values[1] = 1;
  phenom_counter_block_bulk_add(block, 2, slots, values);
  phenom_counter_block_delref(block);

  if (mem_type->def.flags & PHENOM_MEM_FLAGS_ZERO) {
    memset(ptr, 0, mem_type->def.item_size);
  }

  return ptr;
}

void *phenom_mem_alloc_size(phenom_memtype_t mt, uint64_t size)
{
  struct mem_type *mem_type = resolve_mt(mt);
  struct sized_header *ptr;
  phenom_counter_block_t *block;
  static const uint8_t slots[2] = { SLOT_BYTES, SLOT_ALLOCS };
  int64_t values[2];

  if (mem_type->def.item_size) {
    abort();
    return NULL;
  }

  if (size > INT64_MAX) {
    // we can't account for numbers this big
    return NULL;
  }

  ptr = malloc(size + HEADER_RESERVATION);
  if (!ptr) {
    phenom_counter_scope_add(mem_type->scope,
        mem_type->first_slot + SLOT_OOM, 1);
    return NULL;
  }

  ptr->size = size;
  ptr->mt = mt;
  ptr++;

  block = phenom_counter_block_open(mem_type->scope);
  values[0] = size;
  values[1] = 1;
  phenom_counter_block_bulk_add(block, 2, slots, values);
  phenom_counter_block_delref(block);

  if (mem_type->def.flags & PHENOM_MEM_FLAGS_ZERO) {
    memset(ptr, 0, size);
  }

  return ptr;
}

void phenom_mem_free(phenom_memtype_t mt, void *ptr)
{
  struct mem_type *mem_type = resolve_mt(mt);
  phenom_counter_block_t *block;
  static const uint8_t slots[2] = { SLOT_BYTES, SLOT_FREES };
  int64_t values[2];
  uint64_t size;

  if (mem_type->def.item_size) {
    size = mem_type->def.item_size;
  } else {
    struct sized_header *hdr = ptr;

    hdr--;
    ptr = hdr;

    size = hdr->size;
    if (hdr->mt != mt) {
      abort();
    }
  }

  free(ptr);

  block = phenom_counter_block_open(mem_type->scope);
  values[0] = -size;
  values[1] = 1;
  phenom_counter_block_bulk_add(block, 2, slots, values);
  phenom_counter_block_delref(block);
}

void *phenom_mem_realloc(phenom_memtype_t mt, void *ptr, uint64_t size)
{
  struct mem_type *mem_type;
  phenom_counter_block_t *block;
  static const uint8_t slots[2] = { SLOT_BYTES, SLOT_REALLOC };
  int64_t values[3];
  struct sized_header *hdr;
  uint64_t orig_size;
  void *new_ptr;

  if (size == 0) {
    phenom_mem_free(mt, ptr);
    return NULL;
  }
  if (ptr == NULL) {
    return phenom_mem_alloc_size(mt, size);
  }
  mem_type = resolve_mt(mt);
  if (mem_type->def.item_size) {
    abort();
    return NULL;
  }

  hdr = ptr;
  hdr--;
  ptr = hdr;

  if (hdr->mt != mt) {
    abort();
  }

  orig_size = hdr->size;
  if (orig_size == size) {
    return ptr;
  }

  hdr = realloc(ptr, size + HEADER_RESERVATION);
  if (!hdr) {
    phenom_counter_scope_add(mem_type->scope,
        mem_type->first_slot + SLOT_OOM, 1);
    return NULL;
  }
  new_ptr = hdr + 1;
  hdr->size = size;

  block = phenom_counter_block_open(mem_type->scope);
  values[0] = size - orig_size;
  values[1] = 1;
  phenom_counter_block_bulk_add(block, 2, slots, values);
  phenom_counter_block_delref(block);

  if (size > orig_size && mem_type->def.flags & PHENOM_MEM_FLAGS_ZERO) {
    memset((char*)new_ptr + orig_size, 0, size - orig_size);
  }

  return new_ptr;
}

bool phenom_mem_stat(phenom_memtype_t mt, phenom_mem_stats_t *stats)
{
  struct mem_type *mem_type;
  int64_t values[MEM_COUNTER_SLOTS];
  int n;

  if (mt < 0 || mt >= next_memtype) {
    return false;
  }
  mem_type = &memtypes[mt];

  memset(stats, 0, sizeof(*stats));
  stats->def = &mem_type->def;
  n = phenom_counter_scope_get_view(mem_type->scope,
      MEM_COUNTER_SLOTS, values, NULL);
  if (n == MEM_COUNTER_SLOTS) {
    stats->reallocs = values[SLOT_REALLOC];
  }
  stats->frees = values[SLOT_FREES];
  stats->allocs = values[SLOT_ALLOCS];
  stats->oom = values[SLOT_OOM];
  stats->bytes = values[SLOT_BYTES];

  return true;
}

int phenom_mem_stat_facility(const char *facility,
    int num_stats, phenom_mem_stats_t *stats)
{
  int n_stats = 0;
  int i;

  for (i = 0; i < next_memtype && n_stats < num_stats; i++) {
    if (strcmp(facility, memtypes[i].def.facility)) {
      continue;
    }
    phenom_mem_stat(i, &stats[n_stats++]);
  }

  return n_stats;
}

int phenom_mem_stat_range(phenom_memtype_t start,
    phenom_memtype_t end, phenom_mem_stats_t *stats)
{
  int n_stats = 0;
  int i;

  for (i = start; i < next_memtype && i < end; i++) {
    phenom_mem_stat(i, &stats[n_stats++]);
  }

  return n_stats;
}

phenom_memtype_t phenom_mem_type_by_name(const char *facility,
    const char *name)
{
  int i;

  for (i = 0; i < next_memtype; i++) {
    if (!strcmp(facility, memtypes[i].def.facility) &&
        !strcmp(name, memtypes[i].def.name)) {
      return i;
    }
  }
  return PHENOM_MEMTYPE_INVALID;
}

/* vim:ts=2:sw=2:et:
 */

