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
#include "phenom/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ph_thread;
struct ph_job;

typedef struct ph_thread ph_thread_t;

struct ph_thread {
  bool is_init;
  bool is_worker;

  ph_time_t now;

  // OS level representation
  pthread_t thr;

  PH_LIST_HEAD(pdisp, ph_job) pending_dispatch;

#ifdef __sun__
  id_t lwpid;
#endif

#ifdef HAVE_STRERROR_R
  char strerror_buf[128];
#endif
};

typedef void *(*ph_thread_func)(void *arg);

ph_thread_t *ph_spawn_thread(ph_thread_func func, void *arg);
bool ph_thread_init(void);

ph_thread_t *ph_thread_self_slow(void);

extern pthread_key_t __ph_thread_key;
#ifdef HAVE___THREAD
extern __thread ph_thread_t __ph_thread_self;
# define ph_thread_self_fast()   (&__ph_thread_self)
#else
# define ph_thread_self_fast()   \
  ((ph_thread_t*)pthread_getspecific(__ph_thread_key))
#endif

static inline ph_thread_t *ph_thread_self(void)
{
  ph_thread_t *me = ph_thread_self_fast();

  if (unlikely(me == NULL)) {
    return ph_thread_self_slow();
  }
#ifdef HAVE___THREAD
  if (unlikely(!me->is_init)) {
    return ph_thread_self_slow();
  }
#endif
  return me;
}

/* Set the name of the currently executing thread.
 * Used for debugging.  Phenom will set this up
 * when initializing thread pools, you probably don't
 * need to call it */
void ph_thread_set_name(const char *name);

/* Set the affinity of a thread */
bool ph_thread_set_affinity(ph_thread_t *thr, int affinity);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

