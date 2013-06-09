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

#include "phenom/job.h"
#include "phenom/log.h"
#include "ck_backoff.h"
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
__thread ph_thread_t __ph_thread_self;
#else
static ph_memtype_def_t mt_def = {
  "thread", "thread", sizeof(ph_thread_t), PH_MEM_FLAGS_ZERO
};
static ph_memtype_t mt_thread;
#endif
pthread_key_t __ph_thread_key;
static uint32_t next_tid = 0;

static uint32_t num_cores = 0;

static void detect_cores(void)
{
  num_cores = sysconf(_SC_NPROCESSORS_ONLN);
#ifdef __linux__
  {
    char buf[256];
    int fd;
    int divisor = 1;

    // Comma separated list of HT siblings.
    // We're assuming that if HT is present, it is symmetric and applies
    // evenly to all cores in the system.
    fd = open(
        "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list",
        O_RDONLY
    );
    if (fd >= 0) {
      int x = read(fd, buf, sizeof(buf));
      int i;

      for (i = 0; i < x; i++) {
        if (buf[i] == ',') {
          // Each comma represents a shared core
          divisor++;
        }
      }
      close(fd);
    }

    num_cores /= divisor;
  }
#endif
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

#ifndef HAVE___THREAD
  ph_mem_free(mt_thread, thr);
#else
  unused_parameter(thr);
#endif
}

bool ph_thread_init(void)
{
  pthread_key_create(&__ph_thread_key, destroy_thread);

#ifndef HAVE___THREAD
  mt_thread = ph_memtype_register(&mt_def);
  if (mt_thread == PH_MEMTYPE_INVALID) {
    return false;
  }
#endif

  return true;
}

static void init_thread(ph_thread_t *thr)
{
#ifdef HAVE___THREAD
  memset(thr, 0, sizeof(*thr));
  thr->is_init = true;
#endif

  PH_STAILQ_INIT(&thr->pending_nbio);
  PH_STAILQ_INIT(&thr->pending_pool);

  thr->tid = ck_pr_faa_32(&next_tid, 1);
  thr->thr = pthread_self();
#ifdef __sun__
  thr->lwpid = _lwp_self();
#endif
}

struct ph_thread_boot_data {
  ph_thread_t **thr;
  void *(*func)(void*);
  void *arg;
};

static ph_thread_t *ph_thread_init_myself(void)
{
  ph_thread_t *me;

#ifndef HAVE___THREAD
  me = ph_mem_alloc(mt_thread);
  if (!me) {
    ph_panic("fatal OOM in ph_thread_init_myself()");
  }
#else
  me = &__ph_thread_self;
#endif

  pthread_setspecific(__ph_thread_key, me);
  init_thread(me);

  return me;
}

static void *ph_thread_boot(void *arg)
{
  struct ph_thread_boot_data data;
  ph_thread_t *me;

  /* copy in the boot data from the stack of our creator */
  memcpy(&data, arg, sizeof(data));

  me = ph_thread_init_myself();

  /* this publishes that we're ready to run to
   * the thread that spawned us */
  ck_pr_store_ptr(data.thr, me);
  ck_pr_fence_store();

  return data.func(data.arg);
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

  return thr;
}

int ph_thread_join(ph_thread_t *thr, void **res)
{
  return pthread_join(thr->thr, res);
}

ph_thread_t *ph_thread_self_slow(void)
{
  ph_thread_t *thr = pthread_getspecific(__ph_thread_key);

  if (likely(thr != NULL)) {
    return thr;
  }

  return ph_thread_init_myself();
}

void ph_thread_set_name(const char *name)
{
  ph_thread_t *thr = ph_thread_self();
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

bool ph_thread_set_affinity(ph_thread_t *me, int affinity)
{
#ifdef HAVE_PTHREAD_SETAFFINITY_NP
# ifdef __linux__
  cpu_set_t set;
# else /* FreeBSD */
  cpuset_t set;
#endif

  CPU_ZERO(&set);
  CPU_SET(affinity, &set);

  return pthread_setaffinity_np(me->thr, sizeof(set), &set) == 0;
#elif defined(__APPLE__)
  thread_affinity_policy_data_t data;

  data.affinity_tag = affinity + 1;
  return thread_policy_set(pthread_mach_thread_np(me->thr),
      THREAD_AFFINITY_POLICY,
      (thread_policy_t)&data, THREAD_AFFINITY_POLICY_COUNT) == 0;
#elif defined(HAVE_CPUSET_SETAFFINITY)
  /* untested bsdish */
  cpuset_t set;

  CPU_ZERO(&set);
  CPU_SET(affinity, &set);
  cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(set), set);
#elif defined(HAVE_PROCESSOR_BIND)
  return processor_bind(P_LWPID, me->lwpid, affinity, NULL) == 0;
#endif
  return true;
}


/* vim:ts=2:sw=2:et:
 */

