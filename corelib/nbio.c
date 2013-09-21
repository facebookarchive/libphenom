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
#include "phenom/log.h"
#include "phenom/counter.h"
#include "phenom/configuration.h"
#include "ck_epoch.h"
#ifdef USE_GIMLI
#include "libgimli.h"
#endif

// We use 100ms resolution
#define WHEEL_INTERVAL_MS 100

static ph_counter_scope_t *counter_scope = NULL;
static const char *counter_names[] = {
  "dispatched",     // number of jobs dispatched
  "timer_ticks",    // how many times the timer has ticked
  "timer_busy",     // couldn't claim from timer
};
#define SLOT_DISP 0
#define SLOT_TIMER_TICK 1
#define SLOT_BUSY 2

static ph_timerwheel_t wheel;
static int num_schedulers;
int _ph_run_loop = 1;
static ph_thread_t **scheduler_threads;
static ph_job_t gc_job;
static int gc_interval;
#ifdef USE_GIMLI
static volatile struct gimli_heartbeat *hb = NULL;
#endif

#ifdef HAVE_EPOLL_CREATE
static int ep_fd = -1;
static int timer_fd;
static ph_job_t timer_job;
#define DEFAULT_POLL_MASK EPOLLHUP|EPOLLERR|EPOLLONESHOT
#endif

#ifdef HAVE_PORT_CREATE
static int port_fd;
static timer_t port_timer;
#define DEFAULT_POLL_MASK POLLHUP|POLLERR
#endif

#ifdef HAVE_KQUEUE
static int kq_fd;

struct kq_event_set {
  int size;
  int used;
  struct kevent *events;
  struct kevent base[16];
};

static inline void init_kq_set(struct kq_event_set *set)
{
  set->used = 0;
  set->events = set->base;
  set->size = sizeof(set->base) / sizeof(set->base[0]);
}

static void grow_kq_set(struct kq_event_set *set)
{
  struct kevent *k;

  if (set->events == set->base) {
    k = malloc(set->size * 2 * sizeof(*k));
    if (!k) {
      ph_panic("OOM");
    }
    memcpy(k, set->events, set->used * sizeof(*k));
    set->events = k;
  } else {
    k = realloc(set->events, set->size * 2 * sizeof(*k));
    if (!k) {
      ph_panic("OOM");
    }
    set->events = k;
  }
  set->size *= 2;
}

static inline void add_kq_set(struct kq_event_set *set,
    uintptr_t ident,
    int16_t filter,
    uint16_t flags,
    uint32_t fflags,
    intptr_t data,
    void *udata)
{
  int n;

  if (set->used + 1 >= set->size) {
    grow_kq_set(set);
  }

  n = set->used++;
  EV_SET(&set->events[n], ident, filter, flags, fflags, data, udata);
}

static inline void dispose_kq_set(struct kq_event_set *set)
{
  if (set->events == set->base) {
    return;
  }
  free(set->events);
}
#endif

static ph_result_t apply_io_mask(ph_job_t *job, ph_iomask_t mask, void *impl);
static void process_deferred(ph_thread_t *me, void *impl);

static inline void trigger_now(
    ph_counter_block_t *cblock,
    ph_job_t *job, ph_iomask_t why)
{
  if (why != PH_IOMASK_TIME &&
      ph_timerwheel_disable(&wheel, &job->timer) == PH_BUSY) {
    // timer is currently dispatching this: it wins
    ph_counter_block_add(cblock, SLOT_BUSY, 1);
    return;
  }
  ph_counter_block_add(cblock, SLOT_DISP, 1);
  job->callback(job, why, job->data);
}

// map the timer address back to that of its containing
// work item
static inline ph_job_t *job_from_timer(struct ph_timerwheel_timer *timer)
{
  return (ph_job_t*)(void*)(((char*)timer) - ph_offsetof(ph_job_t, timer));
}

static bool before_dispatch_timer(
    ph_timerwheel_t *w,
    struct ph_timerwheel_timer *timer,
    struct timeval now,
    void *arg)
{
  ph_job_t *job;

  ph_unused_parameter(w);
  ph_unused_parameter(now);
  ph_unused_parameter(arg);

  job = job_from_timer(timer);

  if (job->fd != -1) {
    // Turn off any pending kernel notification
    apply_io_mask(job, 0, NULL);
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
  ph_counter_block_t *cblock = arg;

  ph_unused_parameter(w);
  ph_unused_parameter(now);

  // map the timer address back to that of its containing
  // work item
  job = job_from_timer(timer);

  trigger_now(cblock, job, PH_IOMASK_TIME);
}

#ifdef HAVE_EPOLL_CREATE
static void tick_epoll(ph_job_t *job, ph_iomask_t why, void *data)
{
  uint64_t expirations = 0;

  ph_unused_parameter(job);
  ph_unused_parameter(why);
  ph_unused_parameter(data);

  /* consume the number of ticks; ideally this is 1; anything bigger
   * means that we've fallen behind */
  if (read(timer_fd, &expirations, sizeof(expirations)) > 0) {
    if (expirations) {
      ph_counter_block_t *cblock = ph_counter_block_open(counter_scope);

      ph_counter_block_add(cblock, SLOT_TIMER_TICK, 1);
      ph_timerwheel_tick(&wheel, ph_time_now(),
          before_dispatch_timer, dispatch_timer, cblock);
      ph_counter_block_delref(cblock);
    }
  }

  ph_job_set_nbio(job, PH_IOMASK_READ, 0);
}

#ifndef TFD_CLOEXEC
# define TFD_CLOEXEC 02000000
#endif
#ifndef TFD_NONBLOCK
# define TFD_NONBLOCK 04000
#endif

#ifndef HAVE_TIMERFD_CREATE
# include <sys/syscall.h>

#ifndef SYS_timerfd_create
# define SYS_timerfd_create 283
#endif
#ifndef SYS_timerfd_settime
# define SYS_timerfd_settime 286
#endif

static int timerfd_create(int clockid, int flags)
{
  return syscall(SYS_timerfd_create, clockid, flags);
}

static int timerfd_settime(int fd, int flags,
  const struct itimerspec *new_value,
  struct itimerspec *old_value)
{
  return syscall(SYS_timerfd_settime, fd, flags, new_value, old_value);
}
#endif

static ph_result_t do_epoll_init(void)
{
  struct itimerspec ts;

#ifdef HAVE_EPOLL_CREATE1
  ep_fd = epoll_create1(EPOLL_CLOEXEC);
#else
  ep_fd = epoll_create(1024*1024);
#endif

  if (ep_fd == -1) {
    ph_panic("epoll_create: `Pe%d", errno);
  }

#ifndef HAVE_EPOLL_CREATE1
  fcntl(ep_fd, F_SETFD, FD_CLOEXEC);
#endif

  timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);

  memset(&ts, 0, sizeof(ts));
  ts.it_interval.tv_nsec = WHEEL_INTERVAL_MS * 1000000;
  ts.it_value.tv_nsec = ts.it_interval.tv_nsec;
  timerfd_settime(timer_fd, 0, &ts, NULL);

  ph_job_init(&timer_job);
  timer_job.callback = tick_epoll;
  timer_job.fd = timer_fd;
  ph_job_set_nbio(&timer_job, PH_IOMASK_READ, 0);

  return PH_OK;
}
#endif

#ifdef HAVE_PORT_CREATE
static ph_result_t do_port_init(void)
{
  struct sigevent sev;
  port_notify_t notify;
  struct itimerspec ts;

  port_fd = port_create();
  if (port_fd == -1) {
    ph_panic("port_create: `Pe%d", errno);
  }

  memset(&sev, 0, sizeof(sev));
  memset(&notify, 0, sizeof(notify));
  memset(&ts, 0, sizeof(ts));

  ts.it_interval.tv_nsec = WHEEL_INTERVAL_MS * 1000000;
  ts.it_value.tv_nsec = ts.it_interval.tv_nsec;

  notify.portnfy_port = port_fd;
  sev.sigev_notify = SIGEV_PORT;
  sev.sigev_value.sival_ptr = &notify;

  if (timer_create(CLOCK_REALTIME, &sev, &port_timer)) {
    ph_panic("failed to create timer: `Pe%d", errno);
  }
  if (timer_settime(port_timer, 0, &ts, NULL)) {
    ph_panic("failed to set timer: `Pe%d", errno);
  }

  return PH_OK;
}
#endif

#ifdef HAVE_KQUEUE
static ph_result_t do_kqueue_init(void)
{
  struct kevent tev;

  kq_fd = kqueue();
  if (kq_fd == -1) {
    ph_panic("kqueue(): `Pe%d", errno);
  }

  // Configure timer
  EV_SET(&tev, 0, EVFILT_TIMER, EV_ADD, 0, WHEEL_INTERVAL_MS, &kq_fd);
  if (kevent(kq_fd, &tev, 1, NULL, 0, NULL)) {
    ph_panic("setting up timer: kevent: `Pe%d", errno);
  }

  return PH_OK;
}

#endif

ph_result_t ph_nbio_init(uint32_t sched_cores)
{
  ph_thread_t *me;

  if (counter_scope) {
    return PH_OK;
  }

  sched_cores = ph_config_query_int("$.nbio.sched_cores", sched_cores);

  if (sched_cores == 0) {
    /* Pick a reasonable default */
    sched_cores = ph_num_cores() / 2;
  }
  if (sched_cores < 1) {
    sched_cores = 1;
  }
  num_schedulers = sched_cores;
  scheduler_threads = calloc(num_schedulers, sizeof(void*));
  if (!scheduler_threads) {
    return PH_NOMEM;
  }

  me = ph_thread_self_slow();
  me->is_worker = true;

  gettimeofday(&me->now, NULL);
  me->refresh_time = false;
  ph_thread_set_name("phenom:sched");

  ph_timerwheel_init(&wheel, me->now, WHEEL_INTERVAL_MS);

  counter_scope = ph_counter_scope_define(NULL, "iosched", 16);
  ph_counter_scope_register_counter_block(
      counter_scope, sizeof(counter_names)/sizeof(counter_names[0]),
      0, counter_names);

  ph_log(PH_LOG_NOTICE, "ioscheduler initialized with %d threads",
      num_schedulers);

#ifdef HAVE_EPOLL_CREATE
  return do_epoll_init();
#elif defined(HAVE_PORT_CREATE)
  return do_port_init();
#elif defined(HAVE_KQUEUE)
  return do_kqueue_init();
#else
# error fail
#endif
}

#ifdef HAVE_EPOLL_CREATE
static void epoll_emitter(ph_counter_block_t *cblock, ph_thread_t *thread)
{
  struct epoll_event event;
  int n;

  while (ck_pr_load_int(&_ph_run_loop)) {
    n = epoll_wait(ep_fd, &event, 1, -1);
    thread->refresh_time = true;

    if (n < 0 && errno != EINTR) {
      ph_log(PH_LOG_ERR, "epoll_wait: `Pe%d", errno);
    }
    if (n > 0) {
      ph_iomask_t mask = 0;
      ph_job_t *job = event.data.ptr;

      ph_thread_epoch_begin();
      switch (event.events & (EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP)) {
        case EPOLLIN:
          mask = PH_IOMASK_READ;
          break;
        case EPOLLOUT:
          mask = PH_IOMASK_WRITE;
          break;
        case EPOLLIN|EPOLLOUT:
          mask = PH_IOMASK_READ|PH_IOMASK_WRITE;
          break;
        default:
          mask = PH_IOMASK_ERR;
      }
      // We can't just clear kmask completely because ONESHOT retains
      // the existence of the item; we need to know it is there so that
      // we can MOD it instead of ADD it later.
      job->kmask = DEFAULT_POLL_MASK;
      trigger_now(cblock, job, mask);
      if (ph_job_have_deferred_items(thread)) {
        process_deferred(thread, NULL);
      }
      ph_thread_epoch_end();
      ph_thread_epoch_poll();
    }
  }
}
#endif

#ifdef HAVE_PORT_CREATE
static void port_emitter(ph_counter_block_t *cblock, ph_thread_t *thread)
{
  port_event_t event;
  uint_t n;
  ph_job_t *job;
  ph_iomask_t mask;

  while (ck_pr_load_int(&_ph_run_loop)) {
    n = 1;
    memset(&event, 0, sizeof(event));

    if (port_getn(port_fd, &event, 1, &n, NULL)) {
      if (errno != EINTR) {
        ph_panic("port_getn: `Pe%d", errno);
      }
      n = 0;
    }

    if (!n) {
      continue;
    }

    ph_thread_epoch_begin();

    switch (event.portev_source) {
      case PORT_SOURCE_TIMER:
        gettimeofday(&thread->now, NULL);
        thread->refresh_time = false;

        ph_counter_block_add(cblock, SLOT_TIMER_TICK, 1);
        ph_timerwheel_tick(&wheel, thread->now, before_dispatch_timer,
            dispatch_timer, cblock);
        break;

      case PORT_SOURCE_USER:
        break;

      case PORT_SOURCE_FD:
        thread->refresh_time = true;
        job = event.portev_user;

        switch (event.portev_events & (POLLIN|POLLOUT|POLLERR|POLLHUP)) {
          case POLLIN:
            mask = PH_IOMASK_READ;
            break;
          case POLLOUT:
            mask = PH_IOMASK_WRITE;
            break;
          case POLLIN|POLLOUT:
            mask = PH_IOMASK_READ|PH_IOMASK_WRITE;
            break;
          default:
            mask = PH_IOMASK_ERR;
        }
        job->kmask = 0;
        trigger_now(cblock, job, mask);
        break;
    }

    if (ph_job_have_deferred_items(thread)) {
      process_deferred(thread, NULL);
    }
    ph_thread_epoch_end();
    ph_thread_epoch_poll();
  }
}
#endif

#ifdef HAVE_KQUEUE
static inline void dispatch_kevent(ph_counter_block_t *cblock,
    ph_thread_t *thread, struct kevent *event)
{
  ph_iomask_t mask;
  ph_job_t *job;

  if (event->filter != EVFILT_TIMER && (event->flags & EV_ERROR) != 0) {
    // We're pretty strict about errors at this stage to try to
    // ensure that we're doing the right thing.  There may be
    // cases that we should ignore
    ph_panic("kqueue error on fd:%d `Pe%d",
        (int)event->ident, (int)event->data);
  }

  switch (event->filter) {
    case EVFILT_TIMER:
      gettimeofday(&thread->now, NULL);
      thread->refresh_time = false;
      ph_counter_block_add(cblock, SLOT_TIMER_TICK, 1);
      ph_timerwheel_tick(&wheel, thread->now, before_dispatch_timer,
          dispatch_timer, cblock);
      break;

    case EVFILT_READ:
      mask = PH_IOMASK_READ;

      // You'd think that we'd want to do this here, but EV_EOF can
      // be set when we notice that read has been shutdown, but while
      // we still have data in the buffer that we want to read.
      // On this platform we detect EOF as part of attempting to read
      /*
      if (event->flags & EV_EOF) {
        mask |= PH_IOMASK_ERR;
      }
      */

      thread->refresh_time = true;
      job = event->udata;
      job->kmask = 0;
      trigger_now(cblock, job, mask);
      break;

    case EVFILT_WRITE:
      thread->refresh_time = true;
      job = event->udata;
      job->kmask = 0;
      trigger_now(cblock, job, PH_IOMASK_WRITE);
      break;
  }
}

static void kqueue_emitter(ph_counter_block_t *cblock, ph_thread_t *thread)
{
  int n, i;
  struct kq_event_set set;
  int max_chunk;

  init_kq_set(&set);

  // Try to balance work across multiple threads; if we claim too many
  // events and take a while dispatching them, the other threads will
  // sit idle when they could be doing work.  Converseley, if we're
  // the only thread, we should try to consume as many as possible to
  // reduce the number of syscalls that we need.
  // Finally, we want to limit the number we'll process in a loop to
  // minimize the latency of rescheduling events.
  // TODO: This should be a property of the scheduler pool and be something
  // that we apply to the epoll and portfs schedulers
  max_chunk = num_schedulers == 1 ? 1024 : 128;
  ph_config_query_int("$.nbio.max_per_wakeup", max_chunk);

  while (ck_pr_load_int(&_ph_run_loop)) {
    n = kevent(kq_fd, set.events, set.used,
          set.events, MIN(set.size, max_chunk), NULL);

    if (n < 0 && errno != EINTR) {
      ph_panic("kevent: `Pe%d", errno);
    }

    if (n <= 0) {
      continue;
    }

    ph_thread_epoch_begin();
    for (i = 0; i < n; i++) {
      dispatch_kevent(cblock, thread, &set.events[i]);
    }

    if (n + 1 >= set.size) {
      grow_kq_set(&set);
    }
    set.used = 0;

    if (ph_job_have_deferred_items(thread)) {
      process_deferred(thread, &set);
    }
    ph_thread_epoch_end();
    ph_thread_epoch_poll();
  }
  ph_log(PH_LOG_INFO, "kqueue set size=%d", set.size);
  dispose_kq_set(&set);
}
#endif

static void *sched_loop(void *arg)
{
  ph_thread_t *me = ph_thread_self();
  ph_counter_block_t *cblock;

  me->is_worker = true;
  ph_unused_parameter(arg);

  if (!ph_thread_set_affinity(me, me->tid % ph_num_cores())) {
    ph_log(PH_LOG_ERR,
      "failed to set thread %p affinity to CPU %d\n",
      (void*)me, me->tid);
  }

  // Preserve the longer name we picked for the main thread.
  // `top` on linux displays the thread name and 'sched' is
  // not very descriptive
  if (me->tid > 0) {
    ph_thread_set_name("sched");
  }

  cblock = ph_counter_block_open(counter_scope);

#ifdef HAVE_KQUEUE
  kqueue_emitter(cblock, me);
#endif
#ifdef HAVE_EPOLL_CREATE
  epoll_emitter(cblock, me);
#endif
#ifdef HAVE_PORT_CREATE
  port_emitter(cblock, me);
#endif

  ph_counter_block_delref(cblock);
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
  int i;
  void *res;

  scheduler_threads[0] = me;

  for (i = 1; i < num_schedulers; i++) {
    scheduler_threads[i] = ph_thread_spawn(sched_loop, NULL);
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

  sched_loop(NULL);

  for (i = 1; i < num_schedulers; i++) {
    ph_thread_join(scheduler_threads[i], &res);
  }
  free(scheduler_threads);
  scheduler_threads = NULL;

  ph_job_pool_shutdown();
  ph_thread_epoch_barrier();
  return PH_OK;
}

void ph_sched_stop(void)
{
  ck_pr_store_int(&_ph_run_loop, 0);
}

static ph_result_t apply_io_mask(ph_job_t *job, ph_iomask_t mask, void *impl)
{
#ifdef HAVE_EPOLL_CREATE
  struct epoll_event evt;
  int res;
  int want_mask;
  ph_unused_parameter(impl);

  if (job->fd == -1) {
    return PH_OK;
  }

  switch (mask & (PH_IOMASK_READ|PH_IOMASK_WRITE)) {
    case PH_IOMASK_READ|PH_IOMASK_WRITE:
      want_mask = EPOLLIN|EPOLLOUT|DEFAULT_POLL_MASK;
      break;
    case PH_IOMASK_READ:
      want_mask = EPOLLIN|DEFAULT_POLL_MASK;
      break;
    case PH_IOMASK_WRITE:
      want_mask = EPOLLOUT|DEFAULT_POLL_MASK;
      break;
    case 0:
    default:
      want_mask = 0;
      break;
  }

  if (want_mask == job->kmask) {
    return PH_OK;
  }

  if (want_mask == 0) {
    res = epoll_ctl(ep_fd, EPOLL_CTL_DEL, job->fd, &evt);
    if (res < 0 && errno == ENOENT) {
      res = 0;
    }
  } else {
    int op = job->kmask ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    evt.events = want_mask;
    evt.data.ptr = job;

    // Set the masks on the job before we epoll_ctl as it
    // may arrive at another thread *before* epoll_ctl returns
    job->kmask = want_mask;
    job->mask = mask;
    res = epoll_ctl(ep_fd, op, job->fd, &evt);
    if (res == -1 && errno == EEXIST && op == EPOLL_CTL_ADD) {
      // This can happen when we're transitioning between distinct job
      // pointers, for instance, when we're moving from an async connect
      // to setting up the sock job
      res = epoll_ctl(ep_fd, EPOLL_CTL_MOD, job->fd, &evt);
    }
  }

  if (res == -1) {
    ph_panic("epoll_ctl: setting mask to %02x on fd %d -> `Pe%d",
        mask, job->fd, errno);
    return PH_ERR;
  }
  return PH_OK;

#endif
#ifdef HAVE_PORT_CREATE
  int res;
  int want_mask = 0;
  ph_unused_parameter(impl);

  if (job->fd == -1) {
    return PH_OK;
  }

  switch (mask & (PH_IOMASK_READ|PH_IOMASK_WRITE)) {
    case PH_IOMASK_READ:
      want_mask = POLLIN|DEFAULT_POLL_MASK;
      break;
    case PH_IOMASK_WRITE:
      want_mask = POLLOUT|DEFAULT_POLL_MASK;
      break;
    case PH_IOMASK_READ|PH_IOMASK_WRITE:
      want_mask = POLLIN|POLLOUT|DEFAULT_POLL_MASK;
      break;
    case 0:
    default:
      want_mask = 0;
  }

  if (want_mask == job->kmask) {
    return PH_OK;
  }

  switch (want_mask) {
    case 0:
      res = port_dissociate(port_fd, PORT_SOURCE_FD, job->fd);
      if (res != 0 && errno == ENOENT) {
        res = 0;
      }
      if (res != 0) {
        ph_panic("port_dissociate: setting mask to %02x on fd %d -> `Pe%d",
            mask, job->fd, errno);
      }
      job->kmask = 0;
      job->mask = 0;
      break;

    default:
      job->mask = mask;
      job->kmask = want_mask;
      res = port_associate(port_fd, PORT_SOURCE_FD, job->fd,
          want_mask, job);
      if (res != 0) {
        ph_panic("port_associate: setting mask to %02x on fd %d -> `Pe%d",
            mask, job->fd, errno);
        return PH_ERR;
      }
  }
  return PH_OK;

#endif
#ifdef HAVE_KQUEUE
  struct kq_event_set *set = impl, local_set;
  int res;

  if (job->fd == -1) {
    return PH_OK;
  }

  if (mask == job->kmask) {
    return PH_OK;
  }

  if (!set) {
    init_kq_set(&local_set);
    set = &local_set;
  }

  if (mask & PH_IOMASK_READ) {
    add_kq_set(set, job->fd, EVFILT_READ, EV_ADD|EV_ONESHOT, 0, 0, job);
  }
  if (mask & PH_IOMASK_WRITE) {
    add_kq_set(set, job->fd, EVFILT_WRITE, EV_ADD|EV_ONESHOT, 0, 0, job);
  }
  if ((mask & (PH_IOMASK_READ|PH_IOMASK_WRITE)) == 0) {
    // Neither read nor write -> delete
    add_kq_set(set, job->fd, EVFILT_READ, EV_DELETE, 0, 0, job);
    add_kq_set(set, job->fd, EVFILT_WRITE, EV_DELETE, 0, 0, job);
  }

  job->kmask = mask;
  job->mask = mask;

  if (set == &local_set) {
    // Apply it immediately
    res = kevent(kq_fd, set->events, set->used, NULL, 0, NULL);
    if (res != 0 && mask == 0 && errno == ENOENT) {
      // It's "OK" if we decided to delete it and it wasn't there
      res = 0;
    }
    if (res != 0) {
      ph_panic("kevent: setting mask to %02x on fd %d with %d slots -> `Pe%d",
          mask, job->fd, set->used, errno);
      return PH_ERR;
    }
  }
  return PH_OK;
#endif
}

ph_iomask_t ph_job_get_kmask(ph_job_t *job)
{
#ifdef HAVE_KQUEUE
  return job->kmask;
#endif
#ifdef HAVE_EPOLL_CREATE
  switch (job->kmask & (EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP)) {
    case EPOLLIN:
      return PH_IOMASK_READ;
    case EPOLLOUT:
      return PH_IOMASK_WRITE;
    case EPOLLIN|EPOLLOUT:
      return PH_IOMASK_READ|PH_IOMASK_WRITE;
    default:
      return 0;
  }
#endif
#ifdef HAVE_PORT_CREATE
  switch (job->kmask & (POLLIN|POLLOUT|POLLERR|POLLHUP)) {
    case POLLIN:
      return PH_IOMASK_READ;
    case POLLOUT:
      return PH_IOMASK_WRITE;
    case POLLIN|POLLOUT:
      return PH_IOMASK_READ|PH_IOMASK_WRITE;
    default:
      return 0;
  }
#endif
}

static void process_deferred(ph_thread_t *me, void *impl)
{
  ph_job_t *job, *tmp;

  PH_STAILQ_FOREACH_SAFE(job, &me->pending_pool, q_ent, tmp) {
    PH_STAILQ_REMOVE(&me->pending_pool, job, ph_job, q_ent);
    _ph_job_set_pool_immediate(job, me);
  }

  PH_STAILQ_FOREACH_SAFE(job, &me->pending_nbio, q_ent, tmp) {
    ph_iomask_t mask;

    PH_STAILQ_REMOVE(&me->pending_nbio, job, ph_job, q_ent);
    job->in_apply = false;

    // Swap out the mask so that we can apply it safely
    mask = job->mask;
    job->mask = 0;

    // Enable
    if (timerisset(&job->timer.due)) {
      ph_timerwheel_enable(&wheel, &job->timer);
    }
    apply_io_mask(job, mask, impl);
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

  me = ph_thread_self();

  job->pool = NULL;

  job->mask = mask;
  if (timeout) {
    job->timer.due = *timeout;
  } else {
    ph_timerwheel_disable(&wheel, &job->timer);
    timerclear(&job->timer.due);
  }

  if (!me->is_worker) {
    if (timerisset(&job->timer.due)) {
      ph_timerwheel_enable(&wheel, &job->timer);
    }
    apply_io_mask(job, mask, NULL);

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

