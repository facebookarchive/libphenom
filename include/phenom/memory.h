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

/**
 * # Memory management facility
 *
 * It is important for long-running infrastructure software to maintain
 * information about its memory usage.  This facility allows named memory
 * types to be registered and have stats maintained against them.
 */
#ifndef PHENOM_MEMORY_H
#define PHENOM_MEMORY_H

#include <phenom/defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* represents a registered memory type */
typedef int ph_memtype_t;
#define PH_MEMTYPE_INVALID  0
#define PH_MEMTYPE_FIRST    2

/* requests that allocations are zero'd out before being returned */
#define PH_MEM_FLAGS_ZERO 1

/* panic if memory could not be allocated */
#define PH_MEM_FLAGS_PANIC 2

/** defines a memory type.
 *
 * This data structure is used to define a named memory type.
 */
struct ph_memtype_def {
  /* General category of allocations.
   * Convention is to name it after the module or subsystem.
   * This will be used to construct a counter name of the form:
   * memory/<facility>/<name>
   */
  const char *facility;
  /* Name of this memtype.
   * See note in facility above */
  const char *name;
  /* Size of each distinct object of this type of memory.
   * This may be used as a hint to the underlying allocator,
   * and impacts the metrics that are collected about the
   * allocations.
   * If the item_size is zero, then allocations may be
   * of any size */
  uint64_t item_size;
  /* One of the PH_MEM_FLAGS:
   * PH_MEM_FLAGS_ZERO - implicitly zero out the memory before
   *   returning it from ph_mem_alloc() or ph_mem_alloc_size()
   * PH_MEM_FLAGS_PANIC - if the allocation fails, call `ph_panic`.
   *   Use this only for extremely critical allocations with no reasonable
   *   recovery path.
   */
  unsigned flags;
};
typedef struct ph_memtype_def ph_memtype_def_t;

/** Registers a memtype
 *
 * Returns the memtype identifier, or PH_MEMTYPE_INVALID if
 * registration failed.
 */
ph_memtype_t ph_memtype_register(const ph_memtype_def_t *def);

/** Registers a block of memtypes in one operation.
 *
 * The definitions MUST all have the same facility name.
 *
 * "defs" must point to the start of an array of "num_types" memtype definition
 * structures.
 *
 * The "types" parameter may be NULL.  If it is not NULL, it must
 * point to the start of an array of "num_types" elements to receive
 * the assigned memtype identifier for each of the registered
 * memtypes.
 *
 * This function always assigns a contiguous block of memtype identifiers.
 *
 * Returns the memtype identifier corresponding to the first definition, or
 * PH_MEMTYPE_INVALID if the registration failed.
```
ph_memtype_def_t defs[] = {
  { "example", "one", 0, 0 },
  { "example", "two", 0, 0 }
};
struct {
  ph_memtype_t one, two
} mt;
ph_memtype_register_block(
  sizeof(defs) / sizeof(defs[0]),
  defs,
  &mt.one);
// Now I can use mt.one and mt.two to allocate
```
 */
ph_memtype_t ph_memtype_register_block(
    uint8_t num_types,
    const ph_memtype_def_t *defs,
    ph_memtype_t *types);

/** Allocates a fixed-size memory chunk
 *
 * Given a memory type, allocates a block of memory of its defined
 * size and returns it.
 *
 * if PH_MEM_FLAGS_ZERO was specified in the flags of the memtype,
 * the memory region will be cleared to zero before it is returned.
 *
 * It is an error to call this for a memtype that was defined with
 * a 0 size.
 */
void *ph_mem_alloc(ph_memtype_t memtype)
#ifdef __GNUC__
  __attribute__((malloc))
#endif
  ;

/** Allocates a variable-size memory chunk
 *
 * Given a memory type that was registered with size 0, allocates
 * a chunk of the specified size and returns it.
 *
 * if PH_MEM_FLAGS_ZERO was specified in the flags of the memtype,
 * the memory region will be cleared to zero before it is returned.
 *
 * It is an error to call this for a memtype that was not defined with
 * a 0 size.
 */
void *ph_mem_alloc_size(ph_memtype_t memtype, uint64_t size)
#ifdef __GNUC__
  __attribute__((malloc))
#endif
  ;

/** Reallocates a variable-size memory chunk.
 *
 * Changes the size of the memory pointed to by "ptr" to "size" bytes.
 * The contents of the memory at "ptr" are unchanged to the minimum
 * of the old and new sizes.
 *
 * If the block is grown, the remaining space will hold undefined
 * values unless PH_MEM_FLAGS_ZERO was specified in the memtype.
 *
 * If ptr is NULL, this is equivalent to ph_mem_alloc_size().
 *
 * If size is 0, this is equivalent to ph_mem_free().
 *
 * It is an error if ptr was allocated against a different memtype.
 *
 * If the memory was moved, the original ptr value will be freed.
 */
void *ph_mem_realloc(ph_memtype_t memtype, void *ptr, uint64_t size);

/** Frees a memory chunk
 *
 * The memory MUST have been allocated via ph_mem_alloc()
 * or ph_mem_alloc_size().
 */
void ph_mem_free(ph_memtype_t memtype, void *ptr);

/** Duplicates a C-String using a memtype
 *
 * Behaves like strdup(3), except that the storage is allocated
 * against the specified memtype.
 *
 * It is an error to call this for a memtype that was not defined
 * with a 0 size.
 */
char *ph_mem_strdup(ph_memtype_t memtype, const char *str);

/** Data structure for querying memory usage information.
 *
 * This is implemented on top of the counter subsytem
 */
struct ph_mem_stats {
  /* the definition */
  const ph_memtype_def_t *def;
  /* current amount of allocated memory in bytes */
  uint64_t bytes;
  /* total number of out-of-memory events (allocation failures) */
  uint64_t oom;
  /* total number of successful allocation events */
  uint64_t allocs;
  /* total number of calls to free */
  uint64_t frees;
  /* total number of calls to realloc (that are not themselves
   * equivalent to an alloc or free) */
  uint64_t reallocs;
};
typedef struct ph_mem_stats ph_mem_stats_t;

/** Queries information about the specified memtype.
 *
 * * `memtype` - the memtype being interrogated
 * * `stats` - receives the usage information
 */
bool ph_mem_stat(ph_memtype_t memtype, ph_mem_stats_t *stats);

/** Queries information about memtypes in the specified facility.
 *
 * * `facility` - the facility name of interest
 * * `num_stats` - the number of elements in the stats array
 * * `stats` - array of num_stats elements, receives the stats
 *
 * Returns the number of stats that were populated.
 */
int ph_mem_stat_facility(const char *facility,
    int num_stats, ph_mem_stats_t *stats);

/** Queries information about a range of memtypes in the system.
 *
 * * `start` - starting memtype in the range
 * * `end` - ending memtype of the range (exclusive)
 * * `stats` - array of (end - start) elements to receive stats
 *
 * Returns the number of stats that were populated.
 * A short return value indicates that there are no more memtypes beyond
 * the "end" parameter.
 */
int ph_mem_stat_range(ph_memtype_t start,
    ph_memtype_t end, ph_mem_stats_t *stats);

/** Resolves a memory type by name
 *
 * Intended as a diagnostic/testing aid.
 */
ph_memtype_t ph_mem_type_by_name(const char *facility,
    const char *name);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

