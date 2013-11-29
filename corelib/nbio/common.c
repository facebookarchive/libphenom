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
#include "phenom/job.h"
#include "phenom/log.h"
#include "phenom/timerwheel.h"
#include "phenom/sysutil.h"
#include "phenom/memory.h"
#include "phenom/counter.h"
#include "phenom/configuration.h"
#include "corelib/job.h"
#include <ck_epoch.h>

#ifdef USE_GIMLI
#include <libgimli.h>
GIMLI_DECLARE_TRACER_MODULE("gimli_libphenom");
#endif

static ph_memtype_def_t ajob_def = {
  "nbio", "affine_job", sizeof(struct ph_nbio_affine_job), PH_MEM_FLAGS_ZERO
};
static ph_memtype_t mt_ajob;
static ph_counter_scope_t *counter_scope = NULL;
static const char *counter_names[] = {
  "dispatched",     // number of jobs dispatched
  "timer_ticks",    // how many times the timer has ticked
  "timer_busy",     // couldn't claim from timer
};

static uint32_t num_schedulers;
int _ph_run_loop = 1;
static struct ph_nbio_emitter *emitters;
static ph_job_t gc_job;
static int gc_interval;
#ifdef USE_GIMLI
static volatile struct gimli_heartbeat *hb = NULL;
#endif
static struct timeval max_sleep_tv = { 5, 0 };

static inline struct ph_nbio_emitter *emitter_for_affinity(uint32_t n)
{
  return &emitters[n % num_schedulers];
}

static inline struct ph_nbio_emitter *emitter_for_job(ph_job_t *job)
{
  return emitter_for_affinity(job->emitter_affinity);
}

struct ph_nbio_emitter *ph_nbio_emitter_for_job(ph_job_t *job) {
  if (!emitters) {
    // We can be called during TLS teardown
    return NULL;
  }
  return emitter_for_job(job);
}

static void process_deferred(ph_thread_t *me, void *impl);

void ph_nbio_emitter_dispatch_immediate(
    struct ph_nbio_emitter *emitter,
    ph_job_t *job, ph_iomask_t why)
{
  if (why != PH_IOMASK_TIME &&
      ph_timerwheel_disable(&emitter->wheel, &job->timer) == PH_BUSY) {
    // timer is currently dispatching this: it wins
    ph_counter_block_add(emitter->cblock, SLOT_BUSY, 1);
    return;
  }
  ph_counter_block_add(emitter->cblock, SLOT_DISP, 1);

  if (job != &emitter->timer_job && job != &gc_job) {
    emitter->last_dispatch = ph_time_now();
  }
  job->callback(job, why, job->data);
}

void ph_job_collector_emitter_call(struct ph_nbio_emitter *emitter)
{
  struct timeval now = ph_time_now();
  struct timeval target;

  timeradd(&emitter->last_dispatch, &max_sleep_tv, &target);

  if (!timercmp(&now, &target, <)) {
    emitter->last_dispatch = now;
    ph_job_collector_call(emitter->thread);
  }
}


// map the timer address back to that of its containing
// work item
static inline ph_job_t *job_from_timer(struct ph_timerwheel_timer *timer)
{
  return ph_container_of(timer, ph_job_t, timer);
}

static bool before_dispatch_timer(
    ph_timerwheel_t *w,
    struct ph_timerwheel_timer *timer,
    struct timeval now,
    void *arg)
{
  ph_job_t *job;
  struct ph_nbio_emitter *emitter = arg;

  ph_unused_parameter(w);
  ph_unused_parameter(now);
  ph_unused_parameter(arg);
  ph_unused_parameter(emitter);

  job = job_from_timer(timer);

  if (job->fd != -1) {
    // Turn off any pending kernel notification
    ph_nbio_emitter_apply_io_mask(emitter, job, 0);
  }

  return true;
}

static void dispatch_timer(
    ph_timerwheel_t *w,
    struct ph_timerwheel_timer *timer,
    struct timeval now,
    void *arg)
{
  ph_job_t *job;
  struct ph_nbio_emitter *emitter = arg;

  ph_unused_parameter(w);
  ph_unused_parameter(now);

  // map the timer address back to that of its containing
  // work item
  job = job_from_timer(timer);

  ph_nbio_emitter_dispatch_immediate(emitter, job, PH_IOMASK_TIME);
}

void ph_nbio_emitter_timer_tick(struct ph_nbio_emitter *emitter)
{
  struct timeval now = ph_time_now();
  while (timercmp(&emitter->wheel.next_run, &now, <)) {
    ph_counter_block_add(emitter->cblock, SLOT_TIMER_TICK, 1);
    ph_timerwheel_tick(&emitter->wheel, now,
        before_dispatch_timer, dispatch_timer, emitter);
  }
}

void ph_nbio_process_affine_jobs(struct ph_nbio_emitter *emitter)
{
  struct ph_nbio_affine_job *ajob, *tmp;

  if (ph_pingfd_consume_all(&emitter->affine_ping)) {
    ph_nbio_affine_job_stailq_t list;

    PH_STAILQ_INIT(&list);
    ck_rwlock_write_lock(&emitter->wheel.lock);
    PH_STAILQ_SWAP(&list, &emitter->affine_jobs, ph_nbio_affine_job);
    ck_rwlock_write_unlock(&emitter->wheel.lock);

    PH_STAILQ_FOREACH_SAFE(ajob, &list, ent, tmp) {
      PH_STAILQ_REMOVE(&list, ajob, ph_nbio_affine_job, ent);

      ajob->func(ajob->code, ajob->arg);
      ph_mem_free(mt_ajob, ajob);
    }
  }
}

ph_result_t ph_nbio_queue_affine_func(uint32_t emitter_affinity,
    ph_nbio_affine_func func, intptr_t code, void *arg)
{
  struct ph_nbio_affine_job *ajob;
  struct ph_nbio_emitter *emitter;
  bool need_ping;

  ajob = ph_mem_alloc(mt_ajob);
  if (!ajob) {
    return PH_NOMEM;
  }

  ajob->func = func;
  ajob->code = code;
  ajob->arg = arg;

  emitter = emitter_for_affinity(emitter_affinity);
  ck_rwlock_write_lock(&emitter->wheel.lock);
  need_ping = PH_STAILQ_EMPTY(&emitter->affine_jobs);
  PH_STAILQ_INSERT_TAIL(&emitter->affine_jobs, ajob, ent);
  ck_rwlock_write_unlock(&emitter->wheel.lock);

  if (need_ping) {
    ph_pingfd_ping(&emitter->affine_ping);
  }
  return PH_OK;
}

static void do_wakeup(intptr_t code, void *arg)
{
  ph_job_t *job = arg;
  ph_unused_parameter(code);

  job->callback(job, PH_IOMASK_WAKEUP, job->data);
}

ph_result_t ph_job_wakeup(ph_job_t *job)
{
  return ph_nbio_queue_affine_func(job->emitter_affinity, do_wakeup, 0, job);
}

static void affine_dispatch(ph_job_t *job, ph_iomask_t why, void *data)
{
  struct ph_nbio_emitter *emitter = data;

  ph_unused_parameter(why);

  ph_nbio_process_affine_jobs(emitter);
  ph_job_set_nbio(job, PH_IOMASK_READ, 0);
}

ph_result_t ph_nbio_init(uint32_t sched_cores)
{
  ph_thread_t *me;
  uint32_t i;
  int max_sleep;

  if (counter_scope) {
    return PH_OK;
  }

  max_sleep = ph_config_query_int("$.nbio.max_sleep", 5000);
  max_sleep_tv.tv_sec = max_sleep / 1000;
  max_sleep_tv.tv_usec = (max_sleep - (max_sleep_tv.tv_sec * 1000)) * 1000;

  sched_cores = ph_config_query_int("$.nbio.sched_cores", sched_cores);
  mt_ajob = ph_memtype_register(&ajob_def);

  if (sched_cores == 0) {
    /* Pick a reasonable default */
    sched_cores = ph_num_cores() / 2;
  }
  if (sched_cores < 1) {
    sched_cores = 1;
  }
  num_schedulers = sched_cores;
  emitters = calloc(num_schedulers, sizeof(struct ph_nbio_emitter));
  if (!emitters) {
    return PH_NOMEM;
  }

  me = ph_thread_self_slow();
  me->is_worker = 1;

  gettimeofday(&me->now, NULL);
  me->refresh_time = false;
  ph_thread_set_name("phenom:sched");

  for (i = 0; i < num_schedulers; i++) {
    ph_timerwheel_init(&emitters[i].wheel, me->now, WHEEL_INTERVAL_MS);
    emitters[i].emitter_id = i;
    emitters[i].last_dispatch = me->now;

    // prep for affine dispatch
    PH_STAILQ_INIT(&emitters[i].affine_jobs);
    ph_pingfd_init(&emitters[i].affine_ping);
    ph_job_init(&emitters[i].affine_job);
    emitters[i].affine_job.callback = affine_dispatch;
    emitters[i].affine_job.data = &emitters[i];
    emitters[i].affine_job.fd = ph_pingfd_get_fd(&emitters[i].affine_ping);
    emitters[i].affine_job.emitter_affinity = emitters[i].emitter_id;

    ph_nbio_emitter_init(&emitters[i]);

    // Enable affine ping
    ph_job_set_nbio(&emitters[i].affine_job, PH_IOMASK_READ, 0);
  }

  counter_scope = ph_counter_scope_define(NULL, "iosched", 16);
  ph_counter_scope_register_counter_block(
      counter_scope, sizeof(counter_names)/sizeof(counter_names[0]),
      0, counter_names);

  ph_log(PH_LOG_NOTICE, "ioscheduler initialized with %d threads",
      num_schedulers);

  return PH_OK;
}

static void *sched_loop(void *arg)
{
  ph_thread_t *me = ph_thread_self();
  struct ph_nbio_emitter *emitter = arg;
  ph_variant_t *affinity;

  me->is_worker = 1 + (emitter - emitters);
  affinity = ph_config_query("$.nbio.affinity");
  if (!ph_thread_set_affinity_policy(me, affinity)) {
    ph_log(PH_LOG_ERR, "failed to set thread %p affinity", (void*)me);
  }
  if (affinity) {
    ph_var_delref(affinity);
  }

  // Preserve the longer name we picked for the main thread.
  // `top` on linux displays the thread name and 'sched' is
  // not very descriptive
  if (me->tid > 0) {
    ph_thread_set_name("sched");
  }

  emitter->cblock = ph_counter_block_open(counter_scope);
  ph_nbio_emitter_run(emitter, me);
  ph_counter_block_delref(emitter->cblock);
  return NULL;
}

static void epoch_gc(ph_job_t *job, ph_iomask_t why, void *data)
{
  ph_unused_parameter(why);
  ph_unused_parameter(data);

  // This looks weird: it's because the timer dispatch implicitly
  // brackets each event callback with a begin/end.  Since we can't
  // barrier inside a begin/end, we turn it inside out with an
  // end/begin.  The alternative is to spin up a thread just to trigger
  // the reclamation and heart beat, which seems OTT.
  ph_thread_epoch_end();
  ph_thread_epoch_barrier();
  ph_thread_epoch_begin();

#ifdef USE_GIMLI
  if (hb) {
    gimli_heartbeat_set(hb, GIMLI_HB_RUNNING);
  }
#endif
  ph_job_set_timer_in_ms(job, gc_interval);
}

ph_result_t ph_sched_run(void)
{
  ph_thread_t *me = ph_thread_self();
  uint32_t i;
  void *res;

  emitters[0].thread = me;
  me->is_emitter = &emitters[0];

  for (i = 1; i < num_schedulers; i++) {
    emitters[i].thread = ph_thread_spawn(sched_loop, &emitters[i]);
    emitters[i].thread->is_emitter = &emitters[i];
  }

  _ph_job_pool_start_threads();
  process_deferred(me, NULL);

  gc_interval = ph_config_query_int("$.nbio.epoch_interval", 5000);
  if (gc_interval > 0) {
#ifdef USE_GIMLI
    if (getenv("GIMLI_HB_FD")) {
      hb = gimli_heartbeat_attach();
      if (hb) {
        gimli_heartbeat_set(hb, GIMLI_HB_STARTING);
      }
    }
#endif

    ph_job_init(&gc_job);
    gc_job.callback = epoch_gc;
    ph_job_set_timer_in_ms(&gc_job, gc_interval);
  }

  sched_loop(me->is_emitter);

  for (i = 1; i < num_schedulers; i++) {
    ph_thread_join(emitters[i].thread, &res);
  }
  free(emitters);
  emitters = NULL;

  ph_job_pool_shutdown();
  ph_thread_epoch_barrier();
  return PH_OK;
}

void ph_sched_stop(void)
{
  ck_pr_store_int(&_ph_run_loop, 0);
}

static void process_deferred(ph_thread_t *me, void *impl)
{
  ph_job_t *job, *tmp;
  ph_unused_parameter(impl);

  PH_STAILQ_FOREACH_SAFE(job, &me->pending_pool, q_ent, tmp) {
    PH_STAILQ_REMOVE(&me->pending_pool, job, ph_job, q_ent);
    _ph_job_set_pool_immediate(job, me);
  }

  PH_STAILQ_FOREACH_SAFE(job, &me->pending_nbio, q_ent, tmp) {
    ph_iomask_t mask;
    struct ph_nbio_emitter *target_emitter;

    PH_STAILQ_REMOVE(&me->pending_nbio, job, ph_job, q_ent);
    job->in_apply = false;
    target_emitter = emitter_for_job(job);

    // Swap out the mask so that we can apply it safely
    mask = job->mask;
    job->mask = 0;

    // Enable
    if (timerisset(&job->timer.due)) {
      ph_timerwheel_enable(&target_emitter->wheel, &job->timer);
    }
    ph_nbio_emitter_apply_io_mask(target_emitter, job, mask);
  }
}

void ph_job_pool_apply_deferred_items(ph_thread_t *me)
{
  process_deferred(me, NULL);
}

ph_result_t ph_job_set_nbio_timeout_in(
    ph_job_t *job,
    ph_iomask_t mask,
    struct timeval interval)
{
  struct timeval abst = ph_time_now();
  timeradd(&abst, &interval, &abst);
  return ph_job_set_nbio(job, mask, &abst);
}

ph_result_t ph_job_set_nbio(ph_job_t *job, ph_iomask_t mask,
    struct timeval *timeout)
{
  ph_thread_t *me;
  struct ph_nbio_emitter *target_emitter = emitter_for_job(job);

  me = ph_thread_self();

  job->pool = NULL;

  job->mask = mask;
  if (timeout) {
    job->timer.due = *timeout;
  } else {
    ph_timerwheel_disable(&target_emitter->wheel, &job->timer);
    timerclear(&job->timer.due);
  }

  if (!me->is_worker) {
    if (timerisset(&job->timer.due)) {
      ph_timerwheel_enable(&target_emitter->wheel, &job->timer);
    }
    ph_nbio_emitter_apply_io_mask(target_emitter, job, mask);

    return PH_OK;
  }

  // FIXME: immediate apply_io_mask when mask == 0 or add
  // an explicit teardown function

  // queue to our deferred list
  if (ph_likely(!job->in_apply)) {
    PH_STAILQ_INSERT_TAIL(&me->pending_nbio, job, q_ent);
    job->in_apply = true;
  }

  return PH_OK;
}

ph_result_t ph_job_set_timer_at(
    ph_job_t *job,
    struct timeval abstime)
{
  return ph_job_set_nbio(job, PH_IOMASK_TIME, &abstime);
}

ph_result_t ph_job_set_timer_in(
    ph_job_t *job,
    struct timeval interval)
{
  struct timeval abst = ph_time_now();
  timeradd(&abst, &interval, &abst);
  return ph_job_set_nbio(job, PH_IOMASK_TIME, &abst);
}

ph_result_t ph_job_clear_timer(ph_job_t *job)
{
  return ph_job_set_nbio(job, 0, NULL);
}

ph_result_t ph_job_set_timer_in_ms(
    ph_job_t *job,
    uint32_t interval)
{
  struct timeval d;

  d.tv_sec = interval / 1000;
  d.tv_usec = (interval - (d.tv_sec * 1000)) * 1000;

  return ph_job_set_timer_in(job, d);
}

ph_result_t ph_job_init(ph_job_t *job)
{
  memset(job, 0, sizeof(*job));

  job->fd = -1;

  return PH_OK;
}

struct timeval ph_time_now(void)
{
  ph_thread_t *me = ph_thread_self();

  if (!me->is_worker || me->refresh_time ||
      ph_unlikely(!timerisset(&me->now))) {
    gettimeofday(&me->now, NULL);
    me->refresh_time = false;
  }

  return me->now;
}

void ph_socket_set_nonblock(ph_socket_t fd, bool enable)
{
  int flag = fcntl(fd, F_GETFL);

  if (enable) {
    fcntl(fd, F_SETFL, flag | O_NONBLOCK);
  } else {
    fcntl(fd, F_SETFL, flag & ~O_NONBLOCK);
  }
}

void ph_nbio_stat(struct ph_nbio_stats *stats)
{
  stats->num_threads = num_schedulers;
  ph_counter_scope_get_view(counter_scope,
      sizeof(counter_names)/sizeof(counter_names[0]),
      &stats->num_dispatched, NULL);
}

/* vim:ts=2:sw=2:et:
 */

