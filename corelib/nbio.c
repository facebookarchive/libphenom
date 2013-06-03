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

// We use 100ms resolution
#define WHEEL_INTERVAL_MS 100

static ph_timerwheel_t wheel;
static int num_schedulers;
int _ph_run_loop = 1;
static ph_thread_t **scheduler_threads;

#ifdef HAVE_EPOLL_CREATE
static int ep_fd = -1;
static int timer_fd;
static ph_job_t timer_job;
#endif

#ifdef HAVE_PORT_CREATE
static int port_fd;
static timer_t port_timer;
#endif

#ifdef HAVE_KQUEUE
static int kq_fd;
#endif

static ph_result_t trigger_now(ph_thread_t *thr,
    ph_job_t *job, ph_iomask_t why)
{
  if (!ck_pr_cas_ptr(&job->owner, NULL, thr)) {
    ph_log(PH_LOG_ERR, "trigger_now: can't claim job %p", (void*)job);
    return PH_BUSY;
  }
  if (why == PH_IOMASK_TIME && job->tvers != job->vers) {
    ph_log(PH_LOG_ERR, "trigger_now: wrong job vers %p", (void*)job);
    // Something changed since timer was last scheduled
    ck_pr_store_ptr(&job->owner, NULL);
    return PH_BUSY;
  }

  ck_pr_inc_32(&job->vers);
  ck_pr_store_ptr(&job->owner, NULL);

  job->callback(job, why, job->data);

  return PH_OK;
}

static void dispatch_timer(
    ph_timerwheel_t *w,
    struct ph_timerwheel_timer *timer,
    struct timeval now,
    void *arg)
{
  ph_job_t *job;
  ph_thread_t *thr = arg;

  unused_parameter(w);
  unused_parameter(now);

  // map the timer address back to that of its containing
  // work item
  job = (ph_job_t*)
    (((char*)timer) - ph_offsetof(ph_job_t, timer));

  trigger_now(thr, job, PH_IOMASK_TIME);
}

#ifdef HAVE_EPOLL_CREATE
static void tick_epoll(ph_job_t *job, ph_iomask_t why, void *data)
{
  uint64_t expirations = 0;

  unused_parameter(job);
  unused_parameter(why);
  unused_parameter(data);

  /* consume the number of ticks; ideally this is 1; anything bigger
   * means that we've fallen behind */
  if (read(timer_fd, &expirations, sizeof(expirations)) > 0) {
    if (expirations) {
      ph_timerwheel_tick(&wheel, ph_time_now(),
          dispatch_timer, ph_thread_self());
    }
  }

  ph_job_set_nbio(job, PH_IOMASK_READ, 0);
}

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

  if (sched_cores == 0) {
    sched_cores = ph_num_cores() / 4;
  }
  if (sched_cores < 1) {
    sched_cores = 1;
  }
  num_schedulers = sched_cores;
  scheduler_threads = calloc(num_schedulers, sizeof(void*));
  if (!scheduler_threads) {
    return PH_NOMEM;
  }

  ph_thread_init();
  me = ph_thread_self_slow();
  me->is_worker = true;

  gettimeofday(&me->now, NULL);
  me->refresh_time = false;
  ph_thread_set_name("phenom:sched");

  ph_timerwheel_init(&wheel, me->now, WHEEL_INTERVAL_MS);

  ph_job_pool_init();

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
static void epoll_emitter(ph_thread_t *thread)
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

      if (event.events & EPOLLIN) {
        mask |= PH_IOMASK_READ;
      }
      if (event.events & EPOLLOUT) {
        mask |= PH_IOMASK_WRITE;
      }
      if (event.events & (EPOLLERR|EPOLLHUP)) {
        mask |= PH_IOMASK_ERR;
      }
      trigger_now(thread, job, mask);
      if (ph_job_have_deferred_items(thread)) {
        ph_job_pool_apply_deferred_items(thread);
      }
    }
  }
}
#endif

#ifdef HAVE_PORT_CREATE
static void port_emitter(ph_thread_t *thread)
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

    switch (event.portev_source) {
      case PORT_SOURCE_TIMER:
        gettimeofday(&thread->now, NULL);
        thread->refresh_time = false;
        ph_timerwheel_tick(&wheel, thread->now, dispatch_timer, thread);
        break;

      case PORT_SOURCE_USER:
        break;

      case PORT_SOURCE_FD:
        thread->refresh_time = true;
        job = event.portev_user;
        mask = 0;

        if (event.portev_events & POLLIN) {
          mask |= PH_IOMASK_READ;
        }
        if (event.portev_events & POLLOUT) {
          mask |= PH_IOMASK_WRITE;
        }
        if (event.portev_events & (POLLERR|POLLHUP)) {
          mask |= PH_IOMASK_ERR;
        }
        trigger_now(thread, job, mask);
        break;
    }

    if (ph_job_have_deferred_items(thread)) {
      ph_job_pool_apply_deferred_items(thread);
    }
  }
}
#endif

#ifdef HAVE_KQUEUE
static void kqueue_emitter(ph_thread_t *thread)
{
  struct kevent event;
  int n;
  ph_iomask_t mask;

  while (ck_pr_load_int(&_ph_run_loop)) {
    n = kevent(kq_fd, NULL, 0, &event, 1, NULL);

    if (n < 0 && errno != EINTR) {
      ph_panic("kevent: `Pe%d", errno);
    }

    if (n <= 0) {
      continue;
    }

    switch (event.filter) {
      case EVFILT_TIMER:
        gettimeofday(&thread->now, NULL);
        thread->refresh_time = false;
        ph_timerwheel_tick(&wheel, thread->now, dispatch_timer, thread);
        break;

      case EVFILT_READ:
        mask = PH_IOMASK_READ;

        if (event.flags & EV_EOF) {
          mask |= PH_IOMASK_ERR;
        }

        thread->refresh_time = true;
        trigger_now(thread, event.udata, mask);
        break;

      case EVFILT_WRITE:
        thread->refresh_time = true;
        trigger_now(thread, event.udata, PH_IOMASK_WRITE);
        break;
    }

    if (ph_job_have_deferred_items(thread)) {
      ph_job_pool_apply_deferred_items(thread);
    }
  }
}
#endif

static void *sched_loop(void *arg)
{
  ph_thread_t *me = ph_thread_self();
  unused_parameter(arg);

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

#ifdef HAVE_KQUEUE
  kqueue_emitter(me);
#endif
#ifdef HAVE_EPOLL_CREATE
  epoll_emitter(me);
#endif
#ifdef HAVE_PORT_CREATE
  port_emitter(me);
#endif
  return NULL;
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
  ph_job_pool_apply_deferred_items(me);
  sched_loop(NULL);

  for (i = 1; i < num_schedulers; i++) {
    ph_thread_join(scheduler_threads[i], &res);
  }
  free(scheduler_threads);
  scheduler_threads = NULL;

  ph_job_pool_shutdown();
  return PH_OK;
}

void ph_sched_stop(void)
{
  ck_pr_store_int(&_ph_run_loop, 0);
}

static ph_result_t apply_io_mask(ph_job_t *job, ph_iomask_t mask)
{
#ifdef HAVE_EPOLL_CREATE
  struct epoll_event evt;
  int res;

  if (job->fd == -1) {
    return PH_OK;
  }

  if ((mask & (PH_IOMASK_READ|PH_IOMASK_WRITE)) == 0) {
    res = epoll_ctl(ep_fd, EPOLL_CTL_DEL, job->fd, &evt);
    if (res < 0 && errno == ENOENT) {
      res = 0;
    }
  } else {
    evt.events = EPOLLHUP|EPOLLERR|EPOLLONESHOT;
    evt.data.ptr = job;

    if (mask & PH_IOMASK_READ) {
      evt.events |= EPOLLIN;
    }
    if (mask & PH_IOMASK_WRITE) {
      evt.events |= EPOLLOUT;
    }

    // Majority of transitions are for descriptors that are already tracked
    // by the epoll instance, so try to modify first, then add if it wasn't
    // tracked yet.  This makes the common case 1 epoll syscall per update.
    res = epoll_ctl(ep_fd, EPOLL_CTL_MOD, job->fd, &evt);
    if (res < 0 && errno == ENOENT) {
      res = epoll_ctl(ep_fd, EPOLL_CTL_ADD, job->fd, &evt);
    }
  }

  if (res) {
    ph_panic("epoll_ctl: setting mask to %02x on fd %d -> `Pe%d",
        mask, job->fd, errno);
    return PH_ERR;
  }
  job->mask = mask;
  return PH_OK;

#endif
#ifdef HAVE_PORT_CREATE
  int res;

  if (job->fd == -1) {
    return PH_OK;
  }

  if ((mask & (PH_IOMASK_READ|PH_IOMASK_WRITE)) == 0) {
    res = port_dissociate(port_fd, PORT_SOURCE_FD, job->fd);
    if (res != 0 && errno == ENOENT) {
      res = 0;
    }
    if (res != 0) {
      ph_panic("port_dissociate: setting mask to %02x on fd %d -> `Pe%d",
          mask, job->fd, errno);
    }
  } else {
    int events = POLLHUP|POLLERR;

    if (mask & PH_IOMASK_READ) {
      events |= POLLIN;
    }
    if (mask & PH_IOMASK_WRITE) {
      events |= POLLOUT;
    }
    res = port_associate(port_fd, PORT_SOURCE_FD, job->fd,
          events, job);
    if (res != 0) {
      ph_panic("port_associate: setting mask to %02x on fd %d -> `Pe%d",
          mask, job->fd, errno);
      return PH_ERR;
    }
  }
  job->mask = mask;
  return PH_OK;

#endif
#ifdef HAVE_KQUEUE
  struct kevent ev[2];
  int res, n = 0;

  if (job->fd == -1) {
    return PH_OK;
  }

  if (mask & PH_IOMASK_READ) {
    EV_SET(&ev[n], job->fd, EVFILT_READ, EV_ADD|EV_ONESHOT, 0, 0, job);
    n++;
  }
  if (mask & PH_IOMASK_WRITE) {
    EV_SET(&ev[n], job->fd, EVFILT_WRITE, EV_ADD|EV_ONESHOT, 0, 0, job);
    n++;
  }
  if (n == 0) {
    // Neither read nor write -> delete
    EV_SET(&ev[n], job->fd, EVFILT_READ, EV_DELETE, 0, 0, job);
    n++;
    EV_SET(&ev[n], job->fd, EVFILT_WRITE, EV_DELETE, 0, 0, job);
    n++;
  }

  res = kevent(kq_fd, ev, n, NULL, 0, NULL);
  if (res != 0) {
    ph_panic("kevent: setting mask to %02x on fd %d -> `Pe%d",
        mask, job->fd, errno);
    return PH_ERR;
  }
  job->mask = mask;
  return PH_OK;
#endif
}

static void cancel_timer(ph_job_t *job)
{
  if (timerisset(&job->timer.due)) {
    ph_timerwheel_remove(&wheel, &job->timer);
  }
}

void ph_job_pool_apply_deferred_items(ph_thread_t *me)
{
  ph_job_t *job, *tmp;

  PH_STAILQ_FOREACH_SAFE(job, &me->pending_dispatch, q_ent, tmp) {
    ph_iomask_t mask;

    PH_STAILQ_REMOVE(&me->pending_dispatch, job, ph_job, q_ent);

    if (job->pool) {
      _ph_job_set_pool_immediate(job, me);
      continue;
    }

    // Swap out the mask so that we can apply it safely
    mask = job->mask;
    job->mask = 0;
    // Release claim
    ck_pr_store_ptr(&job->owner, NULL);

    // Enable
    if (timerisset(&job->timer.due)) {
      ph_timerwheel_insert(&wheel, &job->timer);
    }
    apply_io_mask(job, mask);
  }
}

ph_result_t ph_job_set_nbio(ph_job_t *job, ph_iomask_t mask,
    struct timeval *timeout)
{
  ph_thread_t *me;

  if (unlikely((mask & (PH_IOMASK_READ|PH_IOMASK_WRITE)) && job->fd == -1)) {
    ph_panic("set_nbio: requested mask requires an fd");
  }

  if (unlikely(timeout && ((mask & PH_IOMASK_TIME) == 0))) {
    ph_panic("set_nbio: requested a timeout but didn't set TIME in the mask");
  }
  if (unlikely(timeout == 0 && ((mask & PH_IOMASK_TIME) != 0))) {
    ph_panic("set_nbio: set TIME in mask but didn't set a timeout value");
  }

  // Claim the job; if it is alread owned, we do nothing
  me = ph_thread_self();
  if (!ck_pr_cas_ptr(&job->owner, NULL, me)) {
    ph_log(PH_LOG_ERR, "fd=%d can't claim job", job->fd);
    return PH_BUSY;
  }

  // We've claimed the job. Increment the configuration version;
  // this will cause concurrent actors to recognize that something
  // changed
  ck_pr_inc_32(&job->vers);

  cancel_timer(job);
  job->pool = NULL;

  job->mask = mask;
  if (timeout) {
    job->timer.due = *timeout;
  } else {
    timerclear(&job->timer.due);
  }
  if (timerisset(&job->timer.due)) {
    ck_pr_store_32(&job->tvers, job->vers);
  }

  if (!me->is_worker) {
    // Release our claim; the vers increment protects our update,
    // and we don't want to risk having the item trigger before
    // we release the claim.
    ck_pr_store_ptr(&job->owner, NULL);

    if (timerisset(&job->timer.due)) {
      ph_timerwheel_insert(&wheel, &job->timer);
    }
    apply_io_mask(job, mask);

    return PH_OK;
  }

  // queue to our deferred list
  PH_STAILQ_INSERT_TAIL(&me->pending_dispatch, job, q_ent);

  // Leave while asserting ownership; the worker will
  // apply the changes
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

  if (!me->is_worker || me->refresh_time || unlikely(!timerisset(&me->now))) {
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

/* vim:ts=2:sw=2:et:
 */

