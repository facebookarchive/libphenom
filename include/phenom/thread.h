/*
 * Copyright 2012-2013 Facebook, Inc.
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

#ifndef PHENOM_THREAD_H
#define PHENOM_THREAD_H

#include "phenom/defs.h"
#include "phenom/queue.h"
#include "phenom/memory.h"
#include <ck_stack.h>
#include <ck_queue.h>
#include <ck_epoch.h>
#include <ck_hs.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ph_thread;
struct ph_job;
struct ph_thread_pool;
struct ph_nbio_emitter;

typedef struct ph_thread ph_thread_t;

struct ph_thread {
  bool refresh_time;
  // internal monotonic thread id
  uint32_t tid;

  PH_STAILQ_HEAD(pdisp, ph_job) pending_nbio, pending_pool;
  struct ph_nbio_emitter *is_emitter;

  int is_worker;
  struct timeval now;

  ck_epoch_record_t epoch_record;
  ck_hs_t counter_hs;
  // linkage so that a stat reader can find all counters
  ck_stack_entry_t thread_linkage;

  // OS level representation
  pthread_t thr;

  // If part of a pool, linkage in that pool
  CK_LIST_ENTRY(ph_thread) pool_ent;

  pid_t lwpid;

#ifdef HAVE_STRERROR_R
  char strerror_buf[128];
#endif

  // Name for debugging purposes
  char name[16];
};

struct ph_thread_pool;
typedef struct ph_thread_pool ph_thread_pool_t;

typedef void *(*ph_thread_func)(void *arg);

/** Spawn a thread */
ph_thread_t *ph_thread_spawn(ph_thread_func func, void *arg);

/** Wait for a thread to complete */
int ph_thread_join(ph_thread_t *thr, void **res);

ph_thread_t *ph_thread_self_slow(void);

extern pthread_key_t __ph_thread_key;
#ifdef HAVE___THREAD
extern __thread ph_thread_t *__ph_thread_self;
# define ph_thread_self_fast()   (__ph_thread_self)
#else
# define ph_thread_self_fast()   \
  ((ph_thread_t*)pthread_getspecific(__ph_thread_key))
#endif

/** Return my own thread handle.
 *
 * If you create a thread for yourself, not using ph_thread_spawn(),
 * you must call ph_library_init() at least once prior to calling
 * any phenom function in that thread.
 *
 * This restriction avoids a conditional branch on every ph_thread_self() call,
 * which can amount to over a million thread pool callback dispatches per
 * second difference in throughput.
 */
static inline ph_thread_t *ph_thread_self(void)
{
  ph_thread_t *me = ph_thread_self_fast();

#if 0 /* enabling this costs 1mm dispatches per second */
  if (ph_unlikely(me == NULL)) {
    return ph_thread_self_slow();
  }
#endif
  return me;
}

/** Set the name of the currently executing thread.
 * Used for debugging.  libPhenom will set this up
 * when initializing thread pools, you probably don't
 * need to call it */
void ph_thread_set_name(const char *name);

/** Set the affinity of a thread */
bool ph_thread_set_affinity(ph_thread_t *thr, int affinity);

/** Begin a new epoch
 *
 * Mark the start of an epoch-protected code section.
 * The epoch is terminated by a call to ph_thread_epoch_end().
 * It is possible to nest an epoch within an existing epoch,
 * but the epoch will not end until the ph_thread_epoch_end() is
 * called enough times to counter each ph_thread_epoch_begin()
 * call.
 *
 * You do not typically need to call this function unless you
 * are implementing a long-lived thread that does not participate
 * in the NBIO or thread pool subsystems.
 */
void ph_thread_epoch_begin(void);

/** Mark the end of an epoch
 *
 * Mark the end of an epoch-protected code section.
 *
 * You do not typically need to call this function unless you
 * are implementing a long-lived thread that does not participate
 * in the NBIO or thread pool subsystems.
 */
void ph_thread_epoch_end(void);

/** Defer a function call until a grace period has been detected in epoch
 *
 * All threads that may have held references to the object need to participate
 * in the epoch for this to work as intended; once they do, the callback
 * function will be invoked at a safe point where all observers have moved
 * beyond observing the object.
 *
 * The epoch entry should to be embedded in the object in question.
 * See http://concurrencykit.org/doc/ck_epoch_call.html for more information
 * on the underlying implementation of this function.
 */
void ph_thread_epoch_defer(ck_epoch_entry_t *entry, ck_epoch_cb_t *func);

/** Attempt to dispatch any deferred epoch calls, if safe
 *
 * Returns `true` if at least one function was dispatched.
 * Returns `false` if it has determined that not all threads have
 * observed the latest generation of epoch-protected objects.
 *
 * Neither value indicates an error.
 *
 * You do not typically need to call this function unless you
 * are implementing a long-lived thread that does not participate
 * in the NBIO or thread pool subsystems.
 */
bool ph_thread_epoch_poll(void);

/** Synchronize and reclaim memory
 *
 * You must not call this from within an epoch-protected code section.
 * This means that you must not call this from within a job callback
 * function or other epoch protected region.
 *
 * You do not typically need to call this function unless you
 * are implementing a long-lived thread that does not participate
 * in the NBIO or thread pool subsystems.
 */
void ph_thread_epoch_barrier(void);

/** Return the emitter affinity value for the currently executing thread
 *
 * If the thread is an emitter (one of the nbio threads), return an affinity
 * value that can be set in job->affinity to cause a job to execute on the
 * same thread later.
 *
 * If the thread is not an emitter, returns 0
 */
uint32_t ph_thread_emitter_affinity(void);

void ph_counter_tear_down_thread(ph_thread_t *thr);
void ph_counter_init_thread(ph_thread_t *thr);
extern ck_stack_t ph_thread_all_threads;

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

