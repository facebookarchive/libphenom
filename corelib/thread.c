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

#include "phenom/work.h"
#include "phenom/log.h"
#ifdef __sun__
# include <sys/lwp.h>
#endif

/* If the system supports the __thread extension, then __phenom_thread_self
 * is magically allocated and made to be a thread local version of the
 * thread struct.  This allows us to avoid the function call and other
 * overheads of pthread_getspecific() in the hot path.
 * However, we still want to guarantee that we clean things up if/when
 * the thread exits, so we also register the thread struct with
 * pthread_setspecific so that we can trigger the TLS key destructor */

#ifdef HAVE___THREAD
__thread phenom_thread_t __phenom_thread_self;
#else
static phenom_memtype_def_t mt_def = {
  "thread", "thread", sizeof(phenom_thread_t), PHENOM_MEM_FLAGS_ZERO
};
static phenom_memtype_t mt_thread;
#endif
pthread_key_t __phenom_thread_key;

static void destroy_thread(void *ptr)
{
  phenom_thread_t *thr = ptr;

  ck_epoch_unregister(&__phenom_trigger_epoch, thr->trigger_record);

#ifndef HAVE___THREAD
  phenom_mem_free(mt_thread, thr);
#endif
}

bool phenom_thread_init(void)
{
  pthread_key_create(&__phenom_thread_key, destroy_thread);

#ifndef HAVE___THREAD
  mt_thread = phenom_memtype_register(&mt_def);
  if (mt_thread == PHENOM_MEMTYPE_INVALID) {
    return false;
  }
#endif

  return true;
}

static void init_thread(phenom_thread_t *thr)
{
#ifdef HAVE___THREAD
  memset(thr, 0, sizeof(*thr));
  thr->is_init = true;
#endif

  thr->trigger_record = ck_epoch_recycle(&__phenom_trigger_epoch);
  if (!thr->trigger_record) {
    /* trigger records are never freed, but they can be recycled */
    thr->trigger_record = malloc(sizeof(*thr->trigger_record));
    if (!thr) {
      phenom_panic("fatal OOM in init_thread");
    }
    ck_epoch_register(&__phenom_trigger_epoch, thr->trigger_record);
  }
  ck_fifo_mpmc_init(&thr->triggers,
      phenom_mem_alloc(__phenom_sched_mt_thread_trigger));

  thr->thr = pthread_self();
#ifdef __sun__
  thr->lwpid = _lwp_self();
#endif
}

struct phenom_thread_boot_data {
  phenom_thread_t **thr;
  void *(*func)(void*);
  void *arg;
};

static phenom_thread_t *phenom_thread_init_myself(void)
{
  phenom_thread_t *me;

#ifndef HAVE___THREAD
  me = phenom_mem_alloc(mt_thread);
  if (!me) {
    phenom_panic("fatal OOM in phenom_thread_init_myself()");
  }
#else
  me = &__phenom_thread_self;
#endif

  pthread_setspecific(__phenom_thread_key, me);
  init_thread(me);

  return me;
}

static void *phenom_thread_boot(void *arg)
{
  struct phenom_thread_boot_data data;
  phenom_thread_t *me;

  /* copy in the boot data from the stack of our creator */
  memcpy(&data, arg, sizeof(data));

  me = phenom_thread_init_myself();

  /* this publishes that we're ready to run to
   * the thread that spawned us */
  ck_pr_store_ptr(data.thr, me);

  return data.func(data.arg);
}

phenom_thread_t *phenom_spawn_thread(phenom_thread_func func, void *arg)
{
  phenom_thread_t *thr = NULL;
  struct phenom_thread_boot_data data;
  pthread_t pt;

  data.thr = &thr;
  data.func = func;
  data.arg = arg;

  if (pthread_create(&pt, NULL, phenom_thread_boot, &data)) {
    return NULL;
  }

  // semi busy wait for the TLS to be set up
  while (ck_pr_load_ptr(&thr) == 0) {
    ck_pr_stall();
  }

  return thr;
}

phenom_thread_t *phenom_thread_self_slow(void)
{
  phenom_thread_t *thr = pthread_getspecific(__phenom_thread_key);

  if (likely(thr != NULL)) {
    return thr;
  }

  return phenom_thread_init_myself();
}

void phenom_thread_set_name(const char *name)
{
  phenom_thread_t *thr = phenom_thread_self();

  if (!thr) {
    return;
  }

#ifdef HAVE_PTHREAD_SET_NAME_NP
  /* OpenBSDish */
  pthread_set_name_np(thr->thr, name);
#elif defined(HAVE_PTHREAD_SETNAME_NP) && defined(__linux__)
  pthread_setname_np(thr->thr, name);
#elif defined(HAVE_PTHREAD_SETNAME_NP) && defined(__MACH__)
  pthread_setname_np(name);
#else
  unused_parameter(name);
#endif
}

/* vim:ts=2:sw=2:et:
 */

