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
static gimli_addr_t deferred_free_fn_ptr = 0;

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

// If the thread appears to be in an interesting state, we don't
// want to suppress printing out its stack trace.
// For example, if the thread is parked and has outstanding deferred
// items to free, this can be a cause of periodic watchdog failures
bool ph_gimli_is_thread_interesting(gimli_proc_t proc, ph_thread_t *thr)
{
  ph_unused_parameter(proc);

  if (thr->epoch_record.n_pending) {
    return true;
  }

  return false;
}

static void print_job_info(gimli_proc_t proc, gimli_addr_t ptr)
{
  ph_job_t job;
  struct ph_job_def def;
  char buf[512];
  const char *name;

  printf("ph_job_t %p {", (void*)ptr);

  if (!gimli_read_mem(proc, ptr, &job, sizeof(job))) {
    printf("INVALID}\n");
    return;
  }

  if (job.def) {
    printf(" def %p {", (void*)job.def);
    if (!gimli_read_mem(proc, (gimli_addr_t)job.def, &def, sizeof(def))) {
      printf("INVALID");
    } else {
      name = gimli_pc_sym_name(proc, (gimli_addr_t)def.callback,
          buf, sizeof(buf));
      printf("\n    cb = %s\n    memtype = %d\n    ", name, def.memtype);
      name = gimli_pc_sym_name(proc, (gimli_addr_t)def.dtor,
          buf, sizeof(buf));
      printf("dtor = %s", name);
    }
    printf("\n  }");
  }

  name = gimli_pc_sym_name(proc, (gimli_addr_t)job.callback,
      buf, sizeof(buf));
  printf("\n  cb = %s\n  data = %p\n  in_apply = %d\n"
      "  mask = %x\n  fd = %d\n}\n",
      name, job.data, job.in_apply, job.mask, job.fd);
}

static void show_deferred_items(gimli_proc_t proc, ph_thread_t *thr)
{
  ck_epoch_entry_t entry;
  int i;
  gimli_addr_t ptr;
  char buf[512];
  const char *name;

  printf("Deferred items for thread %s/%d:\n", thr->name, thr->tid);

  printf("epoch: {state=%u, epoch=%u, active=%u, "
      "pending=%u, peak=%u, disp=%lu}\n",
      thr->epoch_record.state,
      thr->epoch_record.epoch,
      thr->epoch_record.active,
      thr->epoch_record.n_pending,
      thr->epoch_record.n_peak,
      thr->epoch_record.n_dispatch);

  if (deferred_free_fn_ptr == 0) {
    struct gimli_symbol *sym;

    sym = gimli_sym_lookup(proc, NULL, "deferred_job_free");
    if (sym) {
      deferred_free_fn_ptr = sym->addr;
    }
  }

  for (i = 0; i < CK_EPOCH_LENGTH; i++) {
    ptr = (gimli_addr_t)ph_container_of(thr->epoch_record.pending[i].head,
            ck_epoch_entry_t, stack_entry);

    while (ptr) {
      if (!gimli_read_mem(proc, ptr, &entry, sizeof(entry))) {
        break;
      }

      name = gimli_pc_sym_name(proc, (gimli_addr_t)entry.function,
          buf, sizeof(buf));

      if ((gimli_addr_t)entry.function == deferred_free_fn_ptr) {
        ptr = (gimli_addr_t)ph_container_of(ptr, ph_job_t, epoch_entry);
        printf("deferred free: ");
        print_job_info(proc, ptr);
      } else {
        printf("deferred free: %p %s\n", (void*)ptr, name);
      }

      ptr = (gimli_addr_t)ph_container_of(entry.stack_entry.next,
            ck_epoch_entry_t, stack_entry);
    }
  }
}

void ph_gimli_print_summary_for_thread(gimli_proc_t proc, ph_thread_t *thr)
{
  ph_unused_parameter(proc);

  if (thr->epoch_record.n_pending) {
    show_deferred_items(proc, thr);
  }

  // omit a newline so this shows up on the same line as the
  // regular "Thread X (LWP Y)" stuff
  printf("[%s/%d] ", thr->name, thr->tid);
}

static void show_threads(gimli_proc_t proc, void *unused)
{
  ph_thread_t *thr;
  uint32_t n, i;
  uint32_t nworkers = 0;
  uint32_t risky = 0;
  ph_unused_parameter(unused);

  thr = ph_gimli_get_threads(proc, &n);

  for (i = 0; i < n; i++) {
    if (thr[i].is_worker) {
      nworkers++;
    } else if (thr[i].epoch_record.n_pending) {
      // Non workers with deferred items are risky because we (this module)
      // cannot assume that something will collect the deferred items in
      // a timely manner.  This is something that should be highlighted
      // to the developer as it may be a contributing factor to the trace
      // that they are viewing
      risky += thr[i].epoch_record.n_pending;
    }
  }

  printf("\nTHREADS\n");
  printf("There are %" PRIu32 " thread with phenom TLS; "
      "%" PRIu32 " workers, %" PRIu32 " risky deferred items\n",
      n, nworkers, risky);

  ph_unused_parameter(thr);
}

static void init(void) {
  gimli_module_register_tracer(show_threads, 0);
}
PH_GIMLI_INIT(init)

/* vim:ts=2:sw=2:et:
 */

