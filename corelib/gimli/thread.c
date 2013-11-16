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
#include "phenom/thread.h"
#include "corelib/gimli/gimli.h"

/* Copy in the ph_thread info from the target */
static ph_thread_t *threads = NULL;
static uint32_t n_threads = 0;

ph_thread_t *ph_gimli_get_threads(gimli_proc_t proc, uint32_t *nthreads)
{
  ck_stack_t all_threads;
  uint32_t alloc;
  gimli_addr_t ptr;

  if (threads) {
    *nthreads = n_threads;
    return threads;
  }

  if (!gimli_copy_from_symbol(NULL, "ph_thread_all_threads", 0, &all_threads,
        sizeof(all_threads))) {
    return NULL;
  }

  alloc = 128;
  threads = calloc(alloc, sizeof(ph_thread_t));

  // all_threads is a stack of the threads.  The stack entry points to
  // the thread_linkage member, so that's the address we're going to read in
  ptr = (gimli_addr_t)ph_container_of(all_threads.head,
                      ph_thread_t, thread_linkage);

  if (!gimli_read_mem(proc, ptr, threads, sizeof(ph_thread_t))) {
    return NULL;
  }

  while (threads[n_threads].thread_linkage.next) {
    ptr = (gimli_addr_t)ph_container_of(threads[n_threads].thread_linkage.next,
            ph_thread_t, thread_linkage);

    if (n_threads + 1 > alloc) {
      ph_thread_t *bigger = realloc(threads, alloc * 2 * sizeof(ph_thread_t));
      if (!bigger) {
        break;
      }
      threads = bigger;
      alloc *= 2;
    }

    if (gimli_read_mem(proc, ptr, threads + n_threads + 1,
          sizeof(ph_thread_t)) != sizeof(ph_thread_t)) {
      break;
    }

    n_threads++;
  }

  n_threads++;

  *nthreads = n_threads;
  return threads;
}

/* vim:ts=2:sw=2:et:
 */

