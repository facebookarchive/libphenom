/*
 * Copyright 2012-present Facebook, Inc.
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

#include "phenom/job.h"
#include "phenom/sysutil.h"
#include "phenom/log.h"
#include <ck_backoff.h>
#include "corelib/job.h"

#ifdef __sun__
# include <sys/lwp.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
/* If the system supports the __thread extension, then __ph_thread_self
 * is magically allocated and made to be a thread local version of the
 * thread struct.  This allows us to avoid the function call and other
 * overheads of pthread_getspecific() in the hot path.
 * However, we still want to guarantee that we clean things up if/when
 * the thread exits, so we also register the thread struct with
 * pthread_setspecific so that we can trigger the TLS key destructor */

#ifdef HAVE___THREAD
__thread ph_thread_t *__ph_thread_self;
#endif
pthread_key_t __ph_thread_key;
static uint32_t next_tid = 0;
static ck_epoch_t misc_epoch;
ck_stack_t ph_thread_all_threads = CK_STACK_INITIALIZER;

static uint32_t num_cores = 0;

static void detect_cores(void)
{
  num_cores = sysconf(_SC_NPROCESSORS_ONLN);
#ifdef __APPLE__
  {
    unsigned ncores;
    size_t len = sizeof(ncores);

    if (sysctlbyname("machdep.cpu.core_count", &ncores, &len, NULL, 0) == 0) {
      num_cores = ncores;
    }
  }
#endif
}

uint32_t ph_num_cores(void)
{
  if (num_cores == 0) {
    detect_cores();
  }
  return num_cores;
}

static void destroy_thread(void *ptr)
{
  ph_thread_t *thr = ptr;

  ck_epoch_unregister(&thr->epoch_record);

#ifdef HAVE___THREAD
  __ph_thread_self = NULL;
#endif
}

/* This defines a function called ph_counter_head_from_stack_entry
 * that maps a ck_stack_entry_t to a ph_counter_head */
CK_STACK_CONTAINER(ph_thread_t,
    thread_linkage, ph_thread_from_stack_entry)

static void thread_fini(void)
{
#ifdef PH_PLACATE_VALGRIND
  ck_stack_entry_t *stack_entry;
  ph_thread_t *thr;

  while ((stack_entry = ck_stack_pop_npsc(&ph_thread_all_threads)) != NULL) {
    thr = ph_thread_from_stack_entry(stack_entry);
    ph_counter_tear_down_thread(thr);
    free(thr);
  }
#endif
}

static void thread_init(void)
{
  pthread_key_create(&__ph_thread_key, destroy_thread);
  ck_epoch_init(&misc_epoch);
}

PH_LIBRARY_INIT_PRI(thread_init, thread_fini, 0)

struct ph_thread_boot_data {
  ph_thread_t **thr;
  void *(*func)(void*);
  void *arg;
};

static ph_thread_t *ph_thread_init_myself(bool booting)
{
  ph_thread_t *me;
  ck_epoch_record_t *er;

  er = ck_epoch_recycle(&misc_epoch);
  if (er) {
    me = ph_container_of(er, ph_thread_t, epoch_record);
  } else {
    me = calloc(1, sizeof(*me));
    if (!me) {
      ph_panic("fatal OOM in ph_thread_init_myself()");
    }
    ck_epoch_register(&misc_epoch, &me->epoch_record);
    ck_stack_push_mpmc(&ph_thread_all_threads, &me->thread_linkage);
    ph_counter_init_thread(me);
  }
#ifdef HAVE___THREAD
  __ph_thread_self = me;
#endif
  pthread_setspecific(__ph_thread_key, me);

  PH_STAILQ_INIT(&me->pending_nbio);
  PH_STAILQ_INIT(&me->pending_pool);

  me->tid = ck_pr_faa_32(&next_tid, 1);
  me->thr = pthread_self();
  me->lwpid = get_own_tid();

#ifdef HAVE_PTHREAD_GETNAME_NP
  // see if we can discover our thread name from the system
  pthread_getname_np(me->thr, me->name, sizeof(me->name));
#endif

  // If we were recycled from a non-phenom thread, and are initializing
  // a non-phenom thread, it is possible that there are still deferred
  // items to reap in this record, so get them now.
  if (er && !booting) {
    ck_epoch_barrier(&me->epoch_record);
  }

  return me;
}

static void *ph_thread_boot(void *arg)
{
  struct ph_thread_boot_data data;
  ph_thread_t *me;
  void *retval;

  /* copy in the boot data from the stack of our creator */
  memcpy(&data, arg, sizeof(data));

  me = ph_thread_init_myself(true);

  /* this publishes that we're ready to run to
   * the thread that spawned us */
  ck_pr_store_ptr(data.thr, ck_pr_load_ptr(&me));
  ck_pr_fence_store();

  retval = data.func(data.arg);
  ck_epoch_barrier(&me->epoch_record);

  return retval;
}

ph_thread_t *ph_thread_spawn(ph_thread_func func, void *arg)
{
  ph_thread_t *thr = NULL;
  struct ph_thread_boot_data data;
  pthread_t pt;
  ck_backoff_t backoff = CK_BACKOFF_INITIALIZER;

  data.thr = &thr;
  data.func = func;
  data.arg = arg;

  if (pthread_create(&pt, NULL, ph_thread_boot, &data)) {
    return NULL;
  }

  // semi busy wait for the TLS to be set up
  ck_pr_fence_load();
  while (ck_pr_load_ptr(&thr) == 0) {
    ck_backoff_eb(&backoff);
    ck_pr_fence_load();
  }

  return ck_pr_load_ptr(&thr);
}

int ph_thread_join(ph_thread_t *thr, void **res)
{
  return pthread_join(thr->thr, res);
}

ph_thread_t *ph_thread_self_slow(void)
{
  ph_thread_t *thr = pthread_getspecific(__ph_thread_key);

  if (ph_likely(thr != NULL)) {
    return thr;
  }

  return ph_thread_init_myself(false);
}

void ph_thread_set_name(const char *name)
{
  ph_thread_t *thr = ph_thread_self_slow();
  unsigned int namelen;

  if (!thr) {
    return;
  }

  namelen = strlen(name);
  namelen = MIN(namelen, sizeof(thr->name) - 1);
  memcpy(thr->name, name, namelen);
  thr->name[namelen] = 0;

#ifdef HAVE_PTHREAD_SET_NAME_NP
  /* OpenBSDish */
  pthread_set_name_np(thr->thr, thr->name);
#elif defined(HAVE_PTHREAD_SETNAME_NP) && defined(__linux__)
  pthread_setname_np(thr->thr, thr->name);
#elif defined(HAVE_PTHREAD_SETNAME_NP) && defined(__MACH__)
  pthread_setname_np(thr->name);
#endif
}

#if defined(HAVE_PTHREAD_SETAFFINITY_NP) || defined(HAVE_CPUSET_SETAFFINITY)
# ifdef __linux__
typedef cpu_set_t ph_cpu_set_t;
# else /* FreeBSD */
typedef cpuset_t ph_cpu_set_t;
# endif
static bool apply_affinity(ph_cpu_set_t *set, ph_thread_t *me) {
#  ifdef HAVE_CPUSET_SETAFFINITY
  ph_unused_parameter(me);
  return cpuset_setaffinity(CPU_LEVEL_WHICH,
      CPU_WHICH_TID, -1, sizeof(*set), set) == 0;
#  else
  return pthread_setaffinity_np(me->thr, sizeof(*set), set) == 0;
#endif
}
#elif defined(__APPLE__)
typedef thread_affinity_policy_data_t ph_cpu_set_t;
# define CPU_ZERO(setp)      (setp)->affinity_tag = 0
# define CPU_SET(aff, setp)  \
  if (!(setp)->affinity_tag) (setp)->affinity_tag = 1 + aff
static bool apply_affinity(ph_cpu_set_t *set, ph_thread_t *me) {
  return thread_policy_set(pthread_mach_thread_np(me->thr),
      THREAD_AFFINITY_POLICY,
      (thread_policy_t)set, THREAD_AFFINITY_POLICY_COUNT) == 0;
}
#elif defined(HAVE_PROCESSOR_BIND)
typedef processorid_t ph_cpu_set_t;
# define CPU_ZERO(setp)     (*setp) = PBIND_NONE
# define CPU_SET(aff, setp) \
  if (*setp == PBIND_NONE) (*setp) = aff
static bool apply_affinity(ph_cpu_set_t *set, ph_thread_t *me) {
  return processor_bind(P_LWPID, me->lwpid, *set, NULL) == 0;
}
#endif

static inline void ph_cpu_set(int aff, ph_cpu_set_t *set) {
  // ph_log(PH_LOG_DEBUG, "CPU affinity: Adding %d", aff);
  CPU_SET(aff, set);
}

/* {
 *   "base": 0, // base core number; is added to "selector"
 *   "selector": "tid", // use tid
 *   "selector": "wid", // use thr->is_worker id
 *   "selector": [1,2,3],  // use 1+base, 2+base, 3+base
 *   "selector": 1, // use 1+base
 *   "selector": "none" // don't specify affinity
 * }
 */
bool ph_thread_set_affinity_policy(ph_thread_t *me, ph_variant_t *policy)
{
  ph_cpu_set_t set;
  uint32_t cores = ph_num_cores();

  CPU_ZERO(&set);
  if (!policy) {
    ph_cpu_set(me->tid % cores, &set);
  } else {
    int base = 0;
    ph_var_err_t err;
    ph_variant_t *sel = NULL;

    ph_var_unpack(policy, &err, 0, "{si, so}", "base", &base, "selector", &sel);

    if (sel && ph_var_is_array(sel)) {
      uint32_t i;

      for (i = 0; i < ph_var_array_size(sel); i++) {
        int cpu = ph_var_int_val(ph_var_array_get(sel, i));
        ph_cpu_set((base + cpu) % cores, &set);
      }
    } else if (sel && ph_var_is_string(sel)) {
      ph_string_t *s = ph_var_string_val(sel);
      if (ph_string_equal_cstr(s, "tid")) {
        ph_cpu_set((base + me->tid) % cores, &set);
      } else if (ph_string_equal_cstr(s, "wid")) {
        ph_cpu_set((base + me->is_worker - 1) % cores, &set);
      } else if (ph_string_equal_cstr(s, "none")) {
        return true;
      } else {
        ph_log(PH_LOG_ERR, "Unknown thread affinity selector `Ps%p",
            (void*)s);
      }
    } else if (sel && ph_var_is_int(sel)) {
      ph_cpu_set((base + ph_var_int_val(sel)) % cores, &set);
    } else {
      ph_cpu_set((base + me->tid) % cores, &set);
    }
  }

  return apply_affinity(&set, me);
}

bool ph_thread_set_affinity(ph_thread_t *me, int affinity)
{
  ph_cpu_set_t set;

  CPU_ZERO(&set);
  CPU_SET(affinity, &set);

  return apply_affinity(&set, me);
}

void ph_thread_epoch_begin(void)
{
  ph_thread_t *me = ph_thread_self();
  ck_epoch_begin(&me->epoch_record, NULL);
}

void ph_thread_epoch_end(void)
{
  ph_thread_t *me = ph_thread_self();
  ck_epoch_end(&me->epoch_record, NULL);
}

void ph_thread_epoch_defer(ck_epoch_entry_t *entry, ck_epoch_cb_t *func)
{
  ph_thread_t *me = ph_thread_self();
  ck_epoch_call(&me->epoch_record, entry, func);
}

bool ph_thread_epoch_poll(void)
{
  ph_thread_t *me = ph_thread_self();
  return ck_epoch_poll(&me->epoch_record);
}

void ph_thread_epoch_barrier(void)
{
  ph_thread_t *me = ph_thread_self();
  ck_epoch_barrier(&me->epoch_record);
}


/* vim:ts=2:sw=2:et:
 */
