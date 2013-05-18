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
#include "phenom/timerwheel.h"
#include "phenom/sysutil.h"
#include "phenom/memory.h"
#include "phenom/log.h"

// We use 100ms resolution
#define WHEEL_INTERVAL_MS 100

static struct {
  ph_memtype_t
      work_trigger,
      thread_trigger;
} mt;
// This feels ugly, but we need to export mt.thread_trigger to
// thread.c:init_thread
ph_memtype_t __ph_sched_mt_thread_trigger;

static ph_memtype_def_t defs[] = {
  { "sched", "work_trigger", sizeof(struct ph_work_trigger), 0 },
  { "sched", "thread_trigger", sizeof(struct ph_thread_trigger), 0 },
};

static ph_timerwheel_t wheel;
static int num_schedulers;
static int run_loop = 1;
static ph_thread_t **scheduler_threads;
#ifndef HAVE_PORT_CREATE
static ph_pingfd_t pingfd;
static ph_work_item_t trig_item;
static ck_fifo_mpmc_t trig_fifo;
#endif

ck_epoch_t __ph_trigger_epoch;

#ifdef HAVE_EPOLL_CREATE
static int ep_fd;
static int timer_fd;
static ph_work_item_t timer_item;
#endif
#ifdef HAVE_KQUEUE
static int kq_fd;
#endif
#ifdef HAVE_PORT_CREATE
static int port_fd;
static timer_t port_timer;
#endif

static inline ph_time_t ph_timeval_to_time_t(struct timeval *t)
{
  return (t->tv_sec * 1000) + (t->tv_usec / 1000);
}

static void do_dispatch_work(ph_thread_t *thread,
    ph_work_item_t *work,
    ph_time_t now,
    uint32_t trigger,
    intptr_t triggerdata)
{
  unused_parameter(thread);

  // last moment sanity checks
  if (trigger == PH_TRIGGER_TIMEOUT &&
      ph_timerwheel_timer_was_modified(&work->timer)) {
    // Timer was modified just as it triggered
    return;
  }

  work->callback(work, trigger, now, work->data, triggerdata);
}

/* Queue up the trigger */
static ph_result_t enqueue_trigger(
    ph_thread_t *thr,
    ph_work_item_t *work,
    uint32_t trigger,
    intptr_t triggerdata)
{
  struct ph_work_trigger *trig;
  struct ph_thread_trigger *tt = NULL;

  trig = ph_mem_alloc(mt.work_trigger);
  if (!trig) {
    return PH_NOMEM;
  }
  tt = ph_mem_alloc(mt.thread_trigger);
  if (!tt) {
    ph_mem_free(mt.work_trigger, trig);
    return PH_NOMEM;
  }

  trig->trigger = trigger;
  trig->triggerdata = triggerdata;

  ck_epoch_begin(&__ph_trigger_epoch, thr->trigger_record);

  ck_fifo_mpmc_enqueue(&work->triggers, &trig->entry, trig);

  if (work->affinity) {
    /* allow target thread to identify the subject */
    tt->work = work;
    ck_fifo_mpmc_enqueue(&work->affinity->triggers, &tt->entry, tt);

    /* trigger EINTR in epoll_wait() / kevent()
     * so that it wakes up and knows to look for us.
     * If we are the target, don't bother the kernel;
     * we'll check our queue again before we go to sleep */
    if (work->affinity != thr) {
      pthread_kill(work->affinity->thr, SIGIO);
    }

  } else {
    /* if there is no affinity, then we want to wake up any
     * of our scheduler threads and let them find this guy. */
#ifdef HAVE_PORT_CREATE
    // Arrives at our port with source = PORT_SOURCE_USER
    port_send(port_fd, 0, tt);
#else
    ck_fifo_mpmc_enqueue(&trig_fifo, &tt->entry, tt);
    ph_pingfd_ping(&pingfd);
#endif
  }

  ck_epoch_end(&__ph_trigger_epoch, thr->trigger_record);

  return PH_OK;
}

ph_result_t ph_work_trigger(
    ph_work_item_t *work,
    uint32_t trigger,
    intptr_t triggerdata)
{
  return enqueue_trigger(ph_thread_self(),
      work, trigger, triggerdata);
}

static inline ph_result_t set_item_trigger_state(
    ph_work_item_t *work, uint32_t state)
{
  ph_thread_t *owner, *me;
  bool disown = false;

  me = ph_thread_self();
  owner = ck_pr_load_ptr(&work->owner);

  if (owner && owner != me) {
    return PH_BUSY;
  }

  if (!owner) {
    if (!ck_pr_cas_ptr(&work->owner, NULL, me)) {
      return PH_BUSY;
    }
    disown = true;
  }

  ck_pr_store_32(&work->trigger_state, state);

  if (disown) {
    ck_pr_store_ptr(&work->owner, NULL);
  }

  return PH_OK;
}

ph_result_t ph_work_trigger_enable(ph_work_item_t *work)
{
  return set_item_trigger_state(work, PH_TRIGGER_STATE_ENABLED);
}

ph_result_t ph_work_trigger_disable(
    ph_work_item_t *work,
    bool discard)
{
  return set_item_trigger_state(work,
        discard ? PH_TRIGGER_STATE_DISCARD :
                  PH_TRIGGER_STATE_PAUSED);
}

static ph_result_t trigger_now(ph_thread_t *thr,
    ph_time_t now, ph_work_item_t *work,
    uint32_t trigger, intptr_t triggerdata)
{
  if (ck_pr_load_32(&work->trigger_state) == PH_TRIGGER_STATE_DISCARD) {
    // Discard it!
    return PH_OK;
  }

  // If there is no affinity preference, or we are the target,
  // then we may just be able to dispatch here and now, and not
  // bother with allocating trigger structs
  if (work->affinity == NULL || work->affinity == thr) {

    if (ck_pr_cas_ptr(&work->owner, NULL, thr)) {
      /* We've claimed it */

      do_dispatch_work(thr, work, now, trigger, triggerdata);

      ck_pr_store_ptr(&work->owner, NULL);

      return PH_OK;
    }

    /* something else is busy with it right now, so let's
     * just fall through and queue it up */
  }
  return enqueue_trigger(thr, work, trigger, triggerdata);
}

CK_EPOCH_CONTAINER(struct ph_work_trigger, epoch,
    work_trigger_epoch_container)

static void work_trigger_dtor(ck_epoch_entry_t *ent)
{
  struct ph_work_trigger *t;

  t = work_trigger_epoch_container(ent);

  ph_mem_free(mt.work_trigger, t);
}

static void dispatch_work_triggers(ph_thread_t *thread,
    ph_time_t now,
    ph_work_item_t *work)
{
  struct ph_work_trigger *trig;
  ck_fifo_mpmc_entry_t *garbage;

  // claim this work item
  if (!ck_pr_cas_ptr(&work->owner, NULL, thread)) {
    return;
  }

  ck_epoch_begin(&__ph_trigger_epoch, thread->trigger_record);
  while (ck_fifo_mpmc_dequeue(&work->triggers, &trig, &garbage)) {

    do_dispatch_work(thread, work, now,
        trig->trigger, trig->triggerdata);

    ck_epoch_call(&__ph_trigger_epoch, thread->trigger_record,
        &trig->epoch, work_trigger_dtor);
  }
  ck_epoch_end(&__ph_trigger_epoch, thread->trigger_record);
  ck_epoch_poll(&__ph_trigger_epoch, thread->trigger_record);

  ck_pr_store_ptr(&work->owner, NULL);
}

static void dispatch_timer(
    ph_timerwheel_t *w,
    struct ph_timerwheel_timer *timer,
    ph_time_t now,
    void *arg)
{
  ph_work_item_t *work;
  ph_thread_t *thr = arg;

  unused_parameter(w);
  unused_parameter(now);

  // map the timer address back to that of its containing
  // work item
  work = (ph_work_item_t*)
    (((char*)timer) - ph_offsetof(ph_work_item_t, timer));

  trigger_now(thr, now, work, PH_TRIGGER_TIMEOUT, 0);
}

CK_EPOCH_CONTAINER(struct ph_thread_trigger, epoch,
    thread_trigger_epoch_container)

static void thread_trigger_dtor(ck_epoch_entry_t *ent)
{
  struct ph_thread_trigger *t;

  t = thread_trigger_epoch_container(ent);

  ph_mem_free(mt.thread_trigger, t);
}

static void dispatch_trigger_queue(ph_thread_t *thread,
    ck_fifo_mpmc_t *fifo, ph_time_t now)
{
  ck_fifo_mpmc_entry_t *garbage;
  struct ph_thread_trigger *t;

  ck_epoch_begin(&__ph_trigger_epoch, thread->trigger_record);
  while (ck_fifo_mpmc_dequeue(fifo, &t, &garbage)) {
    // now we can dispatch any triggers that we have
    // accumulated against t->triggers
    dispatch_work_triggers(thread, now, t->work);

    ck_epoch_call(&__ph_trigger_epoch, thread->trigger_record,
        &t->epoch, thread_trigger_dtor);
  }
  ck_epoch_end(&__ph_trigger_epoch, thread->trigger_record);
}

#ifndef HAVE_PORT_CREATE
static void trig_dispatch(ph_work_item_t *work, uint32_t trigger,
    ph_time_t now, void *workdata, intptr_t triggerdata)
{
  unused_parameter(work);
  unused_parameter(trigger);
  unused_parameter(triggerdata);
  unused_parameter(workdata);

  if (ph_pingfd_consume_one(&pingfd)) {
    dispatch_trigger_queue(ph_thread_self(), &trig_fifo, now);
  }

  ph_work_io_event_mask_set(work, work->fd, PH_IO_MASK_READ);
}
#endif

ph_time_t ph_time_now(void)
{
  ph_thread_t *me = ph_thread_self();

  if (unlikely(me->now == 0)) {
    struct timeval now;

    gettimeofday(&now, NULL);
    return ph_timeval_to_time_t(&now);
  }

  return me->now;
}

void ph_sched_stop(void)
{
  ck_pr_store_int(&run_loop, 0);
}

#ifdef HAVE_PORT_CREATE
static void port_emitter(ph_thread_t *thread)
{
  port_event_t events[128];
  uint_t i, n;
  struct timeval now;
  ph_time_t nowt;

  while (ck_pr_load_int(&run_loop)) {
    n = 1;
    memset(&events[0], 0, sizeof(events[0]));
    if (port_getn(port_fd, events,
          sizeof(events) / sizeof(events[0]), &n, NULL)) {
      if (errno != EINTR) {
        ph_panic("port_getn: `Pe%d", errno);
      }
      n = 0;
    }

    gettimeofday(&now, NULL);
    nowt = ph_timeval_to_time_t(&now);
    thread->now = nowt;

    if (n == 0) {
      // Might have been a SIGIO, so we need to check our queue
      dispatch_trigger_queue(thread, &thread->triggers, nowt);
    }

    for (i = 0; i < n; i++) {
      switch (events[i].portev_source) {
        case PORT_SOURCE_TIMER:
          ph_timerwheel_tick(&wheel, nowt, dispatch_timer, thread);
          break;

        case PORT_SOURCE_USER:
        {
          struct ph_thread_trigger *t;

          t = events[i].portev_user;
          dispatch_work_triggers(thread, nowt, t->work);
          ph_mem_free(mt.thread_trigger, t);

          break;
        }
        case PORT_SOURCE_FD:
          {
            ph_work_item_t *work = events[i].portev_user;
            ph_io_mask_t mask = 0;

            if (events[i].portev_events & POLLIN) {
              mask |= PH_IO_MASK_READ;
            }
            if (events[i].portev_events & POLLOUT) {
              mask |= PH_IO_MASK_WRITE;
            }
            if (events[i].portev_events & (POLLERR|POLLHUP)) {
              mask |= PH_IO_MASK_ERR;
            }
            trigger_now(thread, nowt, work, PH_TRIGGER_IO, mask);
          }
          break;
      }
    }
    if (n > 0) {
      // We may have triggered affine work for ourselves,
      // check the queue again before we sleep
      dispatch_trigger_queue(thread, &thread->triggers, nowt);
    }

  }
}
#endif

#ifdef HAVE_EPOLL_CREATE
static void epoll_tick(ph_work_item_t *work, uint32_t trigger,
    ph_time_t now, void *workdata, intptr_t triggerdata)
{
  uint64_t expirations = 0;
  unused_parameter(work);
  unused_parameter(trigger);
  unused_parameter(triggerdata);
  unused_parameter(workdata);

  /* find out how many times it ticked since we last looked.
   * This should ideally be 1 */
  if (read(timer_fd, &expirations, sizeof(expirations)) > 0) {
    if (expirations) {
      ph_timerwheel_tick(&wheel, now, dispatch_timer, ph_thread_self());
    }
  }

  ph_work_io_event_mask_set(work, work->fd, PH_IO_MASK_READ);
}

static void epoll_emitter(ph_thread_t *thread)
{
  struct epoll_event events[128];
  int i, n;
  struct timeval now;
  ph_time_t nowt;

  while (ck_pr_load_int(&run_loop)) {
    n = epoll_wait(ep_fd, events, 1, -1);
    gettimeofday(&now, NULL);
    nowt = ph_timeval_to_time_t(&now);
    thread->now = nowt;

    if (n < 0) {
      if (errno == EINTR) {
        // Might have been a SIGIO, so we need to check our queue
        dispatch_trigger_queue(thread, &thread->triggers, nowt);
      } else {
        ph_log(PH_LOG_ERR, "epoll_wait: `Pe%d", errno);
      }
    }
    for (i = 0; i < n; i++) {
      ph_io_mask_t mask = 0;
      ph_work_item_t *work = events[i].data.ptr;

      if (events[i].events & EPOLLIN) {
        mask |= PH_IO_MASK_READ;
      }
      if (events[i].events & EPOLLOUT) {
        mask |= PH_IO_MASK_WRITE;
      }
      if (events[i].events & (EPOLLERR|EPOLLHUP)) {
        mask |= PH_IO_MASK_ERR;
      }
      trigger_now(thread, nowt, work, PH_TRIGGER_IO, mask);
    }
    if (n > 0) {
      // We may have triggered affine work for ourselves,
      // check the queue again before we sleep
      dispatch_trigger_queue(thread, &thread->triggers, nowt);
    }
  }
}
#endif

#ifdef HAVE_KQUEUE
static void kqueue_emitter(ph_thread_t *thread)
{
  struct kevent events[128];
  int i, n;
  struct timeval now;
  ph_time_t nowt;

  while (ck_pr_load_int(&run_loop)) {
    n = kevent(kq_fd, NULL, 0, events,
          sizeof(events)/sizeof(events[0]), NULL);
    gettimeofday(&now, NULL);
    nowt = ph_timeval_to_time_t(&now);
    thread->now = nowt;

    if (n < 0) {
      if (errno == EINTR) {
        // Might have been a SIGIO, so we need to check our queue
        dispatch_trigger_queue(thread, &thread->triggers, nowt);
      } else {
        ph_log(PH_LOG_ERR, "kevent: `Pe%d", errno);
      }
    }
    for (i = 0; i < n; i++) {
      if (events[i].filter == EVFILT_TIMER) {
        ph_timerwheel_tick(&wheel, nowt,
            dispatch_timer, thread);
      } else if (events[i].filter == EVFILT_READ) {
        ph_io_mask_t mask = PH_IO_MASK_READ;

        if (events[i].flags & EV_EOF) {
          mask |= PH_IO_MASK_ERR;
        }

        trigger_now(thread, nowt, events[i].udata,
            PH_TRIGGER_IO, mask);

      } else if (events[i].filter == EVFILT_WRITE) {
        trigger_now(thread, nowt, events[i].udata,
            PH_TRIGGER_IO, PH_IO_MASK_WRITE);
      }
    }
    if (n > 0) {
      // We may have triggered affine work for ourselves,
      // check the queue again before we sleep
      dispatch_trigger_queue(thread, &thread->triggers, nowt);
    }
  }
}
#endif

static void *sched_loop(void *arg)
{
  ph_thread_t *me = ph_thread_self();
  int i;

  for (i = 0; i < num_schedulers; i++) {
    if (scheduler_threads[i] == me) {
      char name[32];

      if (!ph_thread_set_affinity(me, i)) {
        ph_log(PH_LOG_ERR,
            "failed to set thread %p affinity to CPU %d\n",
            (void*)me, i);
      }

      ph_snprintf(name, sizeof(name), "sched-%d", i);
      ph_thread_set_name(name);
      break;
    }
  }

  unused_parameter(arg);

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

  scheduler_threads[0] = me;

  for (i = 1; i < num_schedulers; i++) {
    scheduler_threads[i] = ph_spawn_thread(sched_loop, NULL);
  }

  sched_loop(NULL);
  return PH_OK;
}

ph_result_t ph_work_destroy(
    ph_work_item_t *item)
{
  if (ph_work_trigger_disable(item, true) != PH_OK) {
    ph_panic(
        "ph_work_destroy: unable to disable triggers for item %p, "
        "is it still owned and active?",
        item);
  }

  // FIXME: tear down ck_fifo_mpmc
  if (!CK_FIFO_MPMC_ISEMPTY(&item->triggers)) {
    ph_panic(
        "ph_work_destroy: trigger fifo is not empty for item %p, "
        "is it still active?",
        item);
  }

  return PH_OK;
}

ph_result_t ph_work_init(
    ph_work_item_t *item)
{
  memset(item, 0, sizeof(*item));
  item->fd = -1;
  item->trigger_state = PH_TRIGGER_STATE_PAUSED;

  ck_fifo_mpmc_init(&item->triggers, ph_mem_alloc(mt.work_trigger));

  return PH_OK;
}

ph_result_t ph_work_timeout_at(
    ph_work_item_t *item,
    ph_time_t at)
{
  if (item->timer.due) {
    ph_timerwheel_remove(&wheel, &item->timer);
  }
  if (at == 0) {
    return PH_OK;
  }
  item->timer.due = at;
  return ph_timerwheel_insert(&wheel, &item->timer);
}

ph_result_t ph_work_io_event_mask_set(
    ph_work_item_t *item,
    ph_socket_t fd,
    ph_io_mask_t mask)
{
  /* TODO: should do magic to see if we're ok to poke this here */

  item->fd = fd;

#ifdef HAVE_EPOLL_CREATE
  {
    struct epoll_event evt;
    int res;

    evt.events = EPOLLHUP|EPOLLERR|EPOLLONESHOT;
    evt.data.ptr = item;

    if (mask & PH_IO_MASK_READ) {
      evt.events |= EPOLLIN;
    }
    if (mask & PH_IO_MASK_WRITE) {
      evt.events |= EPOLLOUT;
    }

    if (mask == PH_IO_MASK_NONE) {
      res = epoll_ctl(ep_fd, EPOLL_CTL_DEL, fd, &evt);
      // make it safe to turn off an item that is already off
      if (res < 0 && errno == ENOENT) {
        res = 0;
      }
    } else {
      res = epoll_ctl(ep_fd, EPOLL_CTL_ADD, fd, &evt);
      if (res < 0 && errno == EEXIST) {
        /* assume that we're going to add an existing
         * item more often than we modify one */
        res = epoll_ctl(ep_fd, EPOLL_CTL_MOD, fd, &evt);
      }
    }
    if (res != 0) {
      ph_log(PH_LOG_ERR, "epoll_ctl: `Pe%d", errno);
      return errno; // FIXME: shitty error handling
    }
  }
#endif
#ifdef HAVE_PORT_CREATE
  {
    int events = POLLHUP|POLLHUP;
    int res;

    if (mask & PH_IO_MASK_READ) {
      events |= POLLIN;
    }
    if (mask & PH_IO_MASK_WRITE) {
      events |= POLLOUT;
    }

    if (mask == PH_IO_MASK_NONE) {
      res = port_dissociate(port_fd, PORT_SOURCE_FD, fd);
      if (res != 0 && errno == ENOENT) {
        res = 0;
      }
    } else {
      res = port_associate(port_fd, PORT_SOURCE_FD, fd,
          events, item);
    }

    if (res != 0 && errno != ENOENT) {
      ph_log(PH_LOG_ERR, "port_dissociate: `Pe%d", errno);
      return errno; // FIXME: shitty
    }
  }
#endif
#ifdef HAVE_KQUEUE
  {
    struct kevent ev[2];
    int res;
    int n = 0;

    if (mask & PH_IO_MASK_READ) {
      EV_SET(&ev[n], fd, EVFILT_READ, EV_ADD|EV_ONESHOT, 0, 0, item);
      n++;
    }
    if (mask & PH_IO_MASK_WRITE) {
      EV_SET(&ev[n], fd, EVFILT_WRITE, EV_ADD|EV_ONESHOT, 0, 0, item);
      n++;
    }
    if (mask == PH_IO_MASK_NONE) {
      EV_SET(&ev[n], fd, EVFILT_WRITE, EV_DELETE, 0, 0, item);
      n++;
      EV_SET(&ev[n], fd, EVFILT_READ, EV_DELETE, 0, 0, item);
      n++;
    }
    res = kevent(kq_fd, ev, n, NULL, 0, NULL);
    if (res != 0) {
      ph_log(PH_LOG_ERR, "kevent: `Pe%d", errno);
      return errno;
    }
  }
#endif
  return PH_OK;
}

/* we define this handler as a NOP.
 * We use SIGIO as a way to interrupt a sleeping
 * epoll_wait() or kevent() syscall. */
static void sigio_handler(int signo)
{
  unused_parameter(signo);
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

ph_result_t ph_sched_init(uint32_t sched_cores, uint32_t fd_hint)
{
  struct timeval now;
  struct sigaction sa;
  ph_thread_t *me;

  if (ph_memtype_register_block(sizeof(defs) / sizeof(defs[0]),
        defs, &mt.work_trigger) == PH_MEMTYPE_INVALID) {
    ph_panic("ph_sched_init: unable to register memory types");
  }
  __ph_sched_mt_thread_trigger = mt.thread_trigger;

  if (fd_hint == 0) {
    fd_hint = 1024 * 1024;
  }
  if (sched_cores == 0) {
    sched_cores = sysconf(_SC_NPROCESSORS_ONLN) / 4;
  }
  if (sched_cores < 1) {
    sched_cores = 1;
  }
  num_schedulers = sched_cores;
  scheduler_threads = calloc(num_schedulers, sizeof(void*));
  if (!scheduler_threads) {
    return PH_NOMEM;
  }

  ck_epoch_init(&__ph_trigger_epoch);
#ifndef HAVE_PORT_CREATE
  ck_fifo_mpmc_init(&trig_fifo, ph_mem_alloc(mt.thread_trigger));
#endif

  ph_thread_init();
  me = ph_thread_self();

  /* set up handler */
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigio_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGIO, &sa, NULL);

  gettimeofday(&now, NULL);
  me->now = ph_timeval_to_time_t(&now);
  ph_timerwheel_init(&wheel, me->now, WHEEL_INTERVAL_MS);

#ifdef HAVE_EPOLL_CREATE
  {
    struct itimerspec ts;

    ep_fd = epoll_create(fd_hint);
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);

    memset(&ts, 0, sizeof(ts));
    ts.it_interval.tv_nsec = WHEEL_INTERVAL_MS * 1000000;
    ts.it_value.tv_nsec = ts.it_interval.tv_nsec;
    timerfd_settime(timer_fd, 0, &ts, NULL);

    ph_work_init(&timer_item);
    timer_item.callback = epoll_tick;
    ph_work_io_event_mask_set(&timer_item, timer_fd,
        PH_IO_MASK_READ);
    ph_work_trigger_enable(&timer_item);
  }
#endif
#ifdef HAVE_PORT_CREATE
  {
    struct sigevent sev;
    port_notify_t notify;
    struct itimerspec ts;

    port_fd = port_create();
    if (port_fd == -1) {
      ph_panic("failed to create event port: `Pe%d", errno);
    }

    memset(&sev, 0, sizeof(sev));
    memset(&notify, 0, sizeof(notify));

    notify.portnfy_port = port_fd;
    sev.sigev_notify = SIGEV_PORT;
    sev.sigev_value.sival_ptr = &notify;

    if (timer_create(CLOCK_REALTIME, &sev, &port_timer)) {
      ph_panic("failed to create timer: `Pe%d", errno);
    }

    memset(&ts, 0, sizeof(ts));
    ts.it_interval.tv_nsec = WHEEL_INTERVAL_MS * 1000000;
    ts.it_value.tv_nsec = ts.it_interval.tv_nsec;
    if (timer_settime(port_timer, 0, &ts, NULL)) {
      ph_panic("failed to set timer: `Pe%d", errno);
    }
  }
#endif
#ifdef HAVE_KQUEUE
  {
    struct kevent ev;

    kq_fd = kqueue();

    // Set up the timer
    EV_SET(&ev, 0, EVFILT_TIMER, EV_ADD, 0, WHEEL_INTERVAL_MS, &kq_fd);

    kevent(kq_fd, &ev, 1, NULL, 0, NULL);
  }
#endif

#ifndef HAVE_PORT_CREATE
  /* channel for arbitrary triggers */
  if (ph_pingfd_init(&pingfd) != PH_OK) {
    ph_panic("ph_sched_init: unable to init pingfd");
  }
  ph_work_init(&trig_item);
  trig_item.callback = trig_dispatch;
  ph_work_io_event_mask_set(&trig_item,
      ph_pingfd_get_fd(&pingfd),
      PH_IO_MASK_READ);
  ph_work_trigger_enable(&trig_item);
#endif

  return PH_OK;
}


/* vim:ts=2:sw=2:et:
 */

