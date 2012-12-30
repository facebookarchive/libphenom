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

#ifndef PHENOM_THREAD_H
#define PHENOM_THREAD_H

#include "phenom/defs.h"
#include "phenom/queue.h"
#include "phenom/memory.h"
#include "ck_fifo.h"
#include "ck_epoch.h"

#ifdef __cplusplus
extern "C" {
#endif

struct phenom_thread;
struct phenom_work_item;

typedef struct phenom_thread phenom_thread_t;
typedef struct phenom_work_item phenom_work_item_t;

struct phenom_thread_trigger {
  ck_fifo_mpmc_entry_t entry;
  ck_epoch_entry_t epoch;
  phenom_work_item_t *work;
};

struct phenom_work_trigger {
  ck_fifo_mpmc_entry_t entry;
  ck_epoch_entry_t epoch;
  uint32_t trigger;
  uintptr_t triggerdata;
};

extern ck_epoch_t __phenom_trigger_epoch;
extern phenom_memtype_t __phenom_sched_mt_thread_trigger;

struct phenom_thread {
  bool is_init;

  // queue of phenom_thread_trigger
  ck_fifo_mpmc_t triggers;

  // for safe reclamation of trigger structs
  ck_epoch_record_t *trigger_record;

  phenom_time_t now;

  // OS level representation
  pthread_t thr;
#ifdef __sun__
  id_t lwpid;
#endif

#ifdef HAVE_STRERROR_R
  char strerror_buf[128];
#endif
};

typedef void *(*phenom_thread_func)(void *arg);

phenom_thread_t *phenom_spawn_thread(phenom_thread_func func, void *arg);
bool phenom_thread_init(void);

phenom_thread_t *phenom_thread_self_slow(void);

extern pthread_key_t __phenom_thread_key;
#ifdef HAVE___THREAD
extern __thread phenom_thread_t __phenom_thread_self;
# define phenom_thread_self_fast()   (&__phenom_thread_self)
#else
# define phenom_thread_self_fast()   \
  ((phenom_thread_t*)pthread_getspecific(__phenom_thread_key))
#endif

static inline phenom_thread_t *phenom_thread_self(void)
{
  phenom_thread_t *me = phenom_thread_self_fast();

  if (unlikely(me == NULL)) {
    return phenom_thread_self_slow();
  }
#ifdef HAVE___THREAD
  if (unlikely(!me->is_init)) {
    return phenom_thread_self_slow();
  }
#endif
  return me;
}

/* Set the name of the currently executing thread.
 * Used for debugging.  Phenom will set this up
 * when initializing thread pools, you probably don't
 * need to call it */
void phenom_thread_set_name(const char *name);

/* Set the affinity of a thread */
bool phenom_thread_set_affinity(phenom_thread_t *thr, int affinity);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

