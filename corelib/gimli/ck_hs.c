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
#include "corelib/gimli/gimli.h"
#include <ck_md.h>
#include <ck_hs.h>

// Replicates an implementation detail from ck_hs.c
struct fake_ck_hs_map {
  unsigned int generation[2];
  unsigned int probe_maximum;
  unsigned long mask; // NOLINT(runtime/int)
  unsigned long step; // NOLINT(runtime/int)
  unsigned int probe_limit;
  unsigned int tombstones;
  unsigned long n_entries; // NOLINT(runtime/int)
  unsigned long capacity; // NOLINT(runtime/int)
  unsigned long size; // NOLINT(runtime/int)
  void **entries;
};
#define CK_HS_TOMBSTONE (~(uintptr_t)0)

bool ph_gimli_ck_hs_iter_init_from_map(gimli_proc_t proc, gimli_addr_t mapptr,
    struct ph_gimli_ck_hs_iter *iter)
{
  struct fake_ck_hs_map map;

  memset(iter, 0, sizeof(*iter));
  iter->pp = true;

  if (!gimli_read_mem(proc, mapptr, &map, sizeof(map))) {
    return false;
  }

  iter->capacity = map.capacity;
  iter->size = map.size;
  iter->entries = malloc(iter->capacity * sizeof(void*));

  if (!iter->entries) {
    return false;
  }

  if (!gimli_read_mem(proc, (gimli_addr_t)map.entries, iter->entries,
        iter->capacity * sizeof(void*))) {
    ph_gimli_ck_hs_iter_destroy(iter);
    return false;
  }

  return true;
}

bool ph_gimli_ck_hs_iter_init(gimli_proc_t proc, gimli_addr_t hsptr,
    struct ph_gimli_ck_hs_iter *iter)
{
  ck_hs_t hs;

  if (!gimli_read_mem(proc, hsptr, &hs, sizeof(hs))) {
    return false;
  }

  if (!ph_gimli_ck_hs_iter_init_from_map(proc, (gimli_addr_t)hs.map, iter)) {
    return false;
  }

  iter->pp = hs.mode & CK_HS_MODE_OBJECT;

  return true;
}

bool ph_gimli_ck_hs_iter_next(struct ph_gimli_ck_hs_iter *iter,
    gimli_addr_t *addr)
{
  gimli_addr_t value;

  if (iter->offset >= iter->capacity) {
    *addr = 0;
    return false;
  }

  do {
    value = (gimli_addr_t)iter->entries[iter->offset];
    if (value != 0 && value != CK_HS_TOMBSTONE) {
#ifdef CK_HS_PP
      if (iter->pp) {
        value = value & (((uintptr_t)1 << CK_MD_VMA_BITS) - 1);
      }
#endif
      iter->offset++;
      *addr = value;
      return true;
    }
  } while (++iter->offset < iter->capacity);

  *addr = 0;
  return false;
}

void ph_gimli_ck_hs_iter_destroy(struct ph_gimli_ck_hs_iter *iter)
{
  free(iter->entries);
  memset(iter, 0, sizeof(*iter));
}

/* vim:ts=2:sw=2:et:
 */

