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
#include "phenom/job.h"
#include "phenom/log.h"
#include "phenom/timerwheel.h"
#include "phenom/sysutil.h"
#include "phenom/memory.h"
#include "phenom/counter.h"
#include "phenom/configuration.h"
#include "phenom/printf.h"
#include "corelib/job.h"
#include <ck_stack.h>
#include <ck_backoff.h>
#include <ck_queue.h>

#ifdef __linux__
#include <linux/futex.h>
#include <asm/unistd.h>
#include <sys/syscall.h>
#endif

/** Thread pool dispatcher mechanics.
 * This code has been finely tuned to have the lowest overhead and
 * contention while still providing a reasonably observable featureset.
 *
 * The work queue is actually distributed among the set of "producer"
 * threads (those that produce work items).  The set of producers is
 * further partitioned into two groups; the first group is the preferred
 * set (phenom threads with tid < MAX_RINGS) and the second group is the
 * contended set.
 *
 * The preferred set has a dedicated queue for each preferred thread.
 * The contended set share a single set and arbitrate for write access
 * using a spinlock.  It is therefore very desirable to be a member
 * of the preferred set of threads if a non-trivial volume of jobs are
 * to be scheduled.
 *
 * The queues themselves are actually ring buffers provided by the CK
 * library.  They have the property of being a single-producer-multi-consumer
 * bounded queue implemented very simply in terms of an array of pointers
 * (making it nice and fast).
 *
 * The worker threads (consumers) use a tight, hot loop and try to
 * consume an item first from their own preferred ring on the basis that
 * keeping the same thread hot will result in improved locality and cache
 * characteristics.
 *
 * The workers fall back to pulling from the other rings. A bitmask is
 * used to quickly identify the rings that have had jobs queued.
 *
 * If no work is found, a worker will enter a sleep state using futex()
 * or a condition variable depending on the OS in use.
 *
 * We want/need to avoid contended atomic instructions in all of the
 * functions in this file so that we have the fastest possible foundation
 * for dispatching work.
 */

// Holds max wait time for futex or condition waits
// This value is updated from configuration when we start the
// threads
static struct timespec wait_timespec = { 5, 0 };

static CK_LIST_HEAD(plist, ph_thread_pool)
  ph_all_thread_pools = CK_LIST_HEAD_INITIALIZER(ph_all_thread_pools);
static pthread_mutex_t pool_write_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  ph_memtype_t
    pool,
    ringbuf;
} mt;
static ph_memtype_def_t defs[] = {
  { "threadpool", "pool", sizeof(ph_thread_pool_t),
    PH_MEM_FLAGS_ZERO|PH_MEM_FLAGS_PANIC },
  { "threadpool", "ringbuf", 0, PH_MEM_FLAGS_ZERO|PH_MEM_FLAGS_PANIC },
};

static ph_counter_scope_t *pool_counter_scope;
static const char *counter_names[] = {
  "dispatched",     // number of jobs dispatched
  "consumer_sleep", // number of times consumer sleeps
  "producer_sleep", // number of times producer sleeps
  "num_pending",    // how many outstanding jobs
};
#define SLOT_DISP 0
#define SLOT_CONSUMER_SLEEP 1
#define SLOT_PRODUCER_SLEEP 2
#define SLOT_NUM_PENDING    3

extern int _ph_run_loop;

#define MAX_COLLECTORS 128
static uint8_t next_collector_id = 0;
static ph_job_collector_func collector_funcs[MAX_COLLECTORS];

ph_result_t ph_job_collector_register(ph_job_collector_func func)
{
  uint8_t slot;

  if (next_collector_id >= MAX_COLLECTORS) {
    return PH_ERR;
  }

  slot = ck_pr_faa_8(&next_collector_id, 1);
  if (next_collector_id >= MAX_COLLECTORS) {
    return PH_ERR;
  }

  collector_funcs[slot] = func;
  return PH_OK;
}

void ph_job_collector_call(ph_thread_t *me)
{
  uint8_t i;

  me->refresh_time = true;
  for (i = 0; i < next_collector_id; i++) {
    collector_funcs[i](me);
  }
}

static void wait_destroy(struct ph_thread_pool_wait *waiter)
{
#if defined(USE_COND)
  pthread_mutex_destroy(&waiter->m);
  pthread_cond_destroy(&waiter->c);
#else
  ph_unused_parameter(waiter);
#endif
}

static void wait_init(struct ph_thread_pool_wait *waiter)
{
#if defined(USE_COND)
  pthread_mutex_init(&waiter->m, NULL);
  pthread_cond_init(&waiter->c, NULL);
#else
  ph_unused_parameter(waiter);
#endif
}

#define SECONDS_TO_NS 1000000000LL
static bool wait_pool(struct ph_thread_pool_wait *waiter)
{
  bool woke;

  ck_pr_inc_32(&waiter->num_waiting);
#ifdef USE_FUTEX
  woke = syscall(SYS_futex, &waiter->futcounter, FUTEX_WAIT,
      ck_pr_load_int(&waiter->futcounter), &wait_timespec, NULL, 0) == 0;
#elif defined(USE_COND)
  struct timeval now;
  struct timespec ts;
  pthread_mutex_lock(&waiter->m);
  gettimeofday(&now, NULL);
  ts.tv_sec = now.tv_sec + wait_timespec.tv_sec;
  ts.tv_nsec = (now.tv_usec * 1000) + wait_timespec.tv_nsec;
  if (ts.tv_nsec > SECONDS_TO_NS) {
    ts.tv_sec++;
    ts.tv_nsec -= SECONDS_TO_NS;
  }
  woke = pthread_cond_timedwait(&waiter->c, &waiter->m, &ts) == 0;
  pthread_mutex_unlock(&waiter->m);
#endif
  ck_pr_dec_32(&waiter->num_waiting);
  return woke;
}

#define should_wake_pool(waiter)  \
    ph_unlikely(ck_pr_load_32(&(waiter)->num_waiting))
static void wake_pool(struct ph_thread_pool_wait *waiter)
{
#ifdef USE_FUTEX
  ck_pr_inc_int(&waiter->futcounter);
  syscall(SYS_futex, &waiter->futcounter, FUTEX_WAKE, 1, NULL, NULL, 0);
#elif defined(USE_COND)
  pthread_cond_signal(&waiter->c);
#endif
}

void ph_thread_pool_stat(ph_thread_pool_t *pool,
    struct ph_thread_pool_stats *stats)
{
  ph_counter_scope_get_view(pool->counters,
      sizeof(counter_names)/sizeof(counter_names[0]),
      &stats->num_dispatched, NULL);
}

ph_thread_pool_t *ph_thread_pool_by_name(const char *name)
{
  ph_thread_pool_t *pool;

  CK_LIST_FOREACH(pool, &ph_all_thread_pools, plink) {
    if (!strcmp(name, pool->name)) {
      return pool;
    }
  }
  return NULL;
}

ph_thread_pool_t *ph_thread_pool_define(
    const char *name,
    uint32_t max_queue_len,
    uint32_t num_threads)
{
  ph_thread_pool_t *pool;

  pthread_mutex_lock(&pool_write_lock);

  if ((pool = ph_thread_pool_by_name(name))) {
    pthread_mutex_unlock(&pool_write_lock);
    return pool;
  }

  max_queue_len = ph_config_queryf_int(
      max_queue_len,
      "$.threadpool.%s.queue_len",
      name);

  num_threads = ph_config_queryf_int(
      num_threads,
      "$.threadpool.%s.num_threads",
      name);

  pool = ph_mem_alloc(mt.pool);
  pool->name = strdup(name);
  pool->max_workers = num_threads;
  pool->max_queue_len = max_queue_len;
  pool->threads = calloc(num_threads, sizeof(*pool->threads));
  pool->counters = ph_counter_scope_define(pool_counter_scope,
      pool->name, 16);
  ph_counter_scope_register_counter_block(
      pool->counters, sizeof(counter_names)/sizeof(counter_names[0]),
      0, counter_names);

  ck_spinlock_init(&pool->lock);
  wait_init(&pool->producer);
  wait_init(&pool->consumer);

  CK_LIST_INSERT_HEAD(&ph_all_thread_pools, pool, plink);

  pthread_mutex_unlock(&pool_write_lock);

  return pool;
}

static void job_pool_init(void)
{
  if (ph_memtype_register_block(sizeof(defs) / sizeof(defs[0]),
        defs, &mt.pool) == PH_MEMTYPE_INVALID) {
    ph_panic("ph_job_pool_init: unable to register memory types");
  }

  pool_counter_scope = ph_counter_scope_define(NULL, "threadpool", 16);
}

PH_LIBRARY_INIT_PRI(job_pool_init, 0, 4)

void ph_thread_pool_signal_stop(ph_thread_pool_t *pool)
{
  ck_pr_store_int(&pool->stop, 1);
  if (should_wake_pool(&pool->consumer)) {
    wake_pool(&pool->consumer);
  }
}

void ph_thread_pool_wait_stop(ph_thread_pool_t *pool)
{
  uint32_t i;
  void *res;

  ph_thread_pool_signal_stop(pool);

  while (ck_pr_load_32(&pool->num_workers)) {
    if (should_wake_pool(&pool->consumer)) {
      wake_pool(&pool->consumer);
    }
    sched_yield();
  }

  for (i = 0; i < pool->max_workers; i++) {
    ph_thread_join(pool->threads[i], &res);
  }
}

// Wait for all ph_all_thread_pools to be torn down
void ph_job_pool_shutdown(void)
{
  ph_thread_pool_t *pool, *tmp;
  uint32_t i;

  CK_LIST_FOREACH_SAFE(pool, &ph_all_thread_pools, plink, tmp) {
    pthread_mutex_lock(&pool_write_lock);
    CK_LIST_REMOVE(pool, plink);
    pthread_mutex_unlock(&pool_write_lock);

    ph_thread_pool_wait_stop(pool);

    for (i = 0; i < MAX_RINGS + 1; i++) {
      if (pool->rings[i]) {
        ph_mem_free(mt.ringbuf, pool->rings[i]);
      }
    }

    ph_counter_scope_delref(pool->counters);
    free(pool->name);
    free(pool->threads);
    wait_destroy(&pool->producer);
    wait_destroy(&pool->consumer);

    ph_mem_free(mt.pool, pool);
  }
  ph_counter_scope_delref(pool_counter_scope);
}

static inline ph_job_t *pop_job(ph_thread_pool_t *pool,
    ph_counter_block_t *cblock, int mybucket)
{
  ph_job_t *job;
  uint32_t i;
  intptr_t bits;
  bool woke;

  for (;;) {
    // Try the my own bucket first, as this is lowest contention
    if (ck_ring_dequeue_spmc(pool->rings[mybucket], &job)) {
      return job;
    }

    // Otherwise, we'll work through the set, not including myself
    bits = pool->used_rings & ~(1ULL << mybucket);
    for (;;) {
      i = __builtin_ffsll(bits);
      if (i == 0) {
        break;
      }
      i--;
      if (ck_ring_dequeue_spmc(pool->rings[i], &job)) {
        return job;
      }
      bits &= ~(1ULL << i);
    }

    // Can't find anything to do, so go to sleep
    ph_counter_block_add(cblock, SLOT_CONSUMER_SLEEP, 1);
    woke = wait_pool(&pool->consumer);

    if (!ck_pr_load_int(&_ph_run_loop)) {
      return NULL;
    }

    if (!woke) {
      ph_job_collector_call(ph_thread_self());
    }

    // poll to clean up anything we might have been sitting on while waiting for
    // more jobs or the epoch to advance
    ph_thread_epoch_poll();
  }
}

#define should_init_ring(pool, bucket) ph_unlikely(pool->rings[bucket] == NULL)
static void init_ring(ph_thread_pool_t *pool, int bucket)
{
  uint32_t ring_size;
  char *ringbuf;
  intptr_t mask;

  ring_size = MAX(4, ph_power_2(pool->max_queue_len));

  pool->rings[bucket] = ph_mem_alloc_size(
      mt.ringbuf, sizeof(ck_ring_t) + (ring_size * sizeof(void*)));

  ringbuf = (char*)(pool->rings[bucket] + 1);
  ck_ring_init(pool->rings[bucket], ringbuf, ring_size);

  mask = 1ULL << bucket;
#if defined(__APPLE__) || defined(__clang__)
  /* optimization bug prevents use of ck_pr_or_64 */
  {
    intptr_t old, want;
    do {
      old = pool->used_rings;
      want = old | mask;
    } while (!ck_pr_cas_ptr(&pool->used_rings, (void*)old, (void*)want));
  }
#elif defined(CK_F_PR_CAS_64)
  ck_pr_or_64((uint64_t*)&pool->used_rings, mask);
#else
  ck_pr_or_32((uint32_t*)&pool->used_rings, mask);
#endif
  ck_pr_fence_store();
}

void ph_job_dispatch_now(ph_job_t *job)
{
  ph_thread_t *me = ph_thread_self();

  me->refresh_time = true;
  ph_thread_epoch_begin();
  job->callback(job, PH_IOMASK_NONE, job->data);
  ph_thread_epoch_end();
}

static void *worker_thread(void *arg)
{
  ph_thread_pool_t *pool = arg;
  ph_job_t *job;
  ph_thread_t *me;
  ph_counter_block_t *cblock;
  int my_bucket;

  me = ph_thread_self_slow();
  me->is_worker = 1 + ck_pr_faa_32(&pool->num_workers, 1);
  ph_thread_set_name(pool->name);
  my_bucket = me->tid < MAX_RINGS ? me->tid : MAX_RINGS;
  if (should_init_ring(pool, my_bucket)) {
    init_ring(pool, my_bucket);
  }

  if (!ph_thread_set_affinity_policy(me, pool->config)) {
    ph_log(PH_LOG_ERR, "failed to set thread %p affinity", (void*)me);
  }

  cblock = ph_counter_block_open(pool->counters);

  while (ph_likely(ck_pr_load_int(&_ph_run_loop))) {
    job = pop_job(pool, cblock, my_bucket);
    if (!job) {
      if (ph_unlikely(ck_pr_load_int(&pool->stop))) {
        break;
      }
      ph_thread_epoch_poll();
      continue;
    }

    ph_counter_block_add(cblock, SLOT_NUM_PENDING, -1);

    me->refresh_time = true;
    ph_thread_epoch_begin();
    job->callback(job, PH_IOMASK_NONE, job->data);
    ph_thread_epoch_end();
    ph_thread_epoch_poll();

    ph_counter_block_add(cblock, SLOT_DISP, 1);

    if (ph_job_have_deferred_items(me)) {
      ph_job_pool_apply_deferred_items(me);
    }
    if (should_wake_pool(&pool->producer)) {
      wake_pool(&pool->producer);
    }
  }

  ck_pr_dec_32(&pool->num_workers);
  ph_counter_block_delref(cblock);
  ph_thread_epoch_poll();

  if (should_wake_pool(&pool->consumer)) {
    wake_pool(&pool->consumer);
  }
  return NULL;
}

bool ph_thread_pool_start_workers(ph_thread_pool_t *pool)
{
  uint32_t i;

  pthread_mutex_lock(&pool_write_lock);
  if (pool->num_workers) {
    pthread_mutex_unlock(&pool_write_lock);
    return false;
  }

  // If we're being used to restart the pool, make sure we're
  // not set to stop...
  ck_pr_store_int(&pool->stop, 0);

  if (!pool->config) {
    char query[512];
    ph_snprintf(query, sizeof(query), "$.threadpool.%s.affinity", pool->name);
    pool->config = ph_config_query(query);
  }

  for (i = 0; i < pool->max_workers; i++) {
    pool->threads[i] = ph_thread_spawn(worker_thread, pool);
  }

  pthread_mutex_unlock(&pool_write_lock);
  return true;
}

void _ph_job_pool_start_threads(void)
{
  ph_thread_pool_t *pool;

  int max_sleep = ph_config_query_int("$.nbio.max_sleep", 5000);

  wait_timespec.tv_sec = max_sleep / 1000;
  wait_timespec.tv_nsec =
    (max_sleep - (wait_timespec.tv_sec * 1000)) * 1000000;

  CK_LIST_FOREACH(pool, &ph_all_thread_pools, plink) {
    if (ck_pr_load_32(&pool->num_workers) < pool->max_workers) {
      ph_thread_pool_start_workers(pool);
    }
  }
}

static inline void do_set_pool(ph_job_t *job, ph_thread_t *me)
{
  ph_thread_pool_t *pool;
  ph_counter_block_t *cblock;

  pool = job->pool;

  cblock = ph_counter_block_open(pool->counters);
  ph_counter_block_add(cblock, SLOT_NUM_PENDING, 1);

  if (ph_unlikely(me->tid >= MAX_RINGS)) {
    ck_spinlock_lock(&pool->lock);
    if (should_init_ring(pool, MAX_RINGS)) {
      init_ring(pool, MAX_RINGS);
    }
    while (!ck_ring_enqueue_spmc(pool->rings[MAX_RINGS], job)) {
      ck_spinlock_unlock(&pool->lock);
      ph_counter_block_add(cblock, SLOT_PRODUCER_SLEEP, 1);
      wait_pool(&pool->producer);
      ck_spinlock_lock(&pool->lock);
    }
    ck_spinlock_unlock(&pool->lock);
  } else {
    if (should_init_ring(pool, me->tid)) {
      init_ring(pool, me->tid);
    }
    while (ph_unlikely(!ck_ring_enqueue_spmc(pool->rings[me->tid], job))) {
      ph_counter_block_add(cblock, SLOT_PRODUCER_SLEEP, 1);
      wait_pool(&pool->producer);
    }
  }

  if (should_wake_pool(&pool->consumer)) {
    wake_pool(&pool->consumer);
  }

  ph_counter_block_delref(cblock);
}

void _ph_job_set_pool_immediate(ph_job_t *job, ph_thread_t *me)
{
  do_set_pool(job, me);
}

ph_result_t ph_job_set_pool_immediate(
    ph_job_t *job,
    ph_thread_pool_t *pool)
{
  ph_thread_t *me = ph_thread_self();

  job->pool = pool;
  do_set_pool(job, me);
  return PH_OK;
}

ph_result_t ph_job_set_pool(
    ph_job_t *job,
    ph_thread_pool_t *pool)
{
  ph_thread_t *me = ph_thread_self();

  job->pool = pool;

  if (!me->is_worker) {
    do_set_pool(job, me);
  } else {
    PH_STAILQ_INSERT_TAIL(&me->pending_pool, job, q_ent);
  }

  return PH_OK;
}

ph_job_t *ph_job_alloc(struct ph_job_def *def)
{
  ph_job_t *job;

  job = ph_mem_alloc(def->memtype);
  if (!job) {
    return NULL;
  }

  ph_job_init(job);
  job->callback = def->callback;
  job->def = def;

  return job;
}

CK_EPOCH_CONTAINER(ph_job_t, epoch_entry, epoch_entry_to_job)

static void deferred_job_free(ck_epoch_entry_t *e)
{
  ph_job_t *job = epoch_entry_to_job(e);
  struct ph_nbio_emitter *target_emitter = ph_nbio_emitter_for_job(job);

  // emitter can be NULL during TLS teardown
  if (target_emitter) {
    ph_timerwheel_remove(&target_emitter->wheel, &job->timer);
  }

  if (job->def->dtor) {
    job->def->dtor(job);
  }

  ph_mem_free(job->def->memtype, job);
}

void ph_job_free(ph_job_t *job)
{
  struct ph_nbio_emitter *target_emitter = ph_nbio_emitter_for_job(job);

  if (timerisset(&job->timer.due)) {
    ph_timerwheel_disable(&target_emitter->wheel, &job->timer);
  }
  ph_thread_epoch_defer(&job->epoch_entry, deferred_job_free);
}

/* vim:ts=2:sw=2:et:
 */

