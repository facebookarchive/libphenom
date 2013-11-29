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
#include "phenom/configuration.h"
#include "corelib/job.h"

#ifdef HAVE_PORT_CREATE
#define DEFAULT_POLL_MASK POLLHUP|POLLERR

void ph_nbio_emitter_init(struct ph_nbio_emitter *emitter)
{
  struct sigevent sev;
  port_notify_t notify;
  struct itimerspec ts;

  emitter->io_fd = port_create();
  if (emitter->io_fd == -1) {
    ph_panic("port_create: `Pe%d", errno);
  }

  memset(&sev, 0, sizeof(sev));
  memset(&notify, 0, sizeof(notify));
  memset(&ts, 0, sizeof(ts));

  ts.it_interval.tv_nsec = WHEEL_INTERVAL_MS * 1000000;
  ts.it_value.tv_nsec = ts.it_interval.tv_nsec;

  notify.portnfy_port = emitter->io_fd;
  sev.sigev_notify = SIGEV_PORT;
  sev.sigev_value.sival_ptr = &notify;

  if (timer_create(CLOCK_REALTIME, &sev, &emitter->port_timer)) {
    ph_panic("failed to create timer: `Pe%d", errno);
  }
  if (timer_settime(emitter->port_timer, 0, &ts, NULL)) {
    ph_panic("failed to set timer: `Pe%d", errno);
  }
}

void ph_nbio_emitter_run(struct ph_nbio_emitter *emitter, ph_thread_t *thread)
{
  port_event_t *event;
  uint_t n, i, max_chunk, max_sleep;
  ph_job_t *job;
  ph_iomask_t mask;
  struct timespec ts;

  max_chunk = ph_config_query_int("$.nbio.max_per_wakeup", 1024);
  max_sleep = ph_config_query_int("$.nbio.max_sleep", 5000);
  ts.tv_sec = max_sleep / 1000;
  ts.tv_nsec = (max_sleep - (ts.tv_sec * 1000)) * 1000000;
  event = malloc(max_chunk * sizeof(port_event_t));

  while (ck_pr_load_int(&_ph_run_loop)) {
    n = 1;
    memset(event, 0, sizeof(*event));

    if (port_getn(emitter->io_fd, event, max_chunk, &n, &ts)) {
      if (errno != EINTR && errno != ETIME) {
        ph_panic("port_getn: `Pe%d", errno);
      }
      n = 0;
    }

    if (!n) {
      ph_job_collector_emitter_call(emitter);
      ph_thread_epoch_poll();
      continue;
    }

    for (i = 0; i < n; i++) {
      ph_thread_epoch_begin();

      switch (event[i].portev_source) {
        case PORT_SOURCE_TIMER:
          gettimeofday(&thread->now, NULL);
          thread->refresh_time = false;
          ph_nbio_emitter_timer_tick(emitter);
          break;

        case PORT_SOURCE_USER:
          break;

        case PORT_SOURCE_FD:
          thread->refresh_time = true;
          job = event[i].portev_user;

          switch (event[i].portev_events & (POLLIN|POLLOUT|POLLERR|POLLHUP)) {
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
          ph_nbio_emitter_dispatch_immediate(emitter, job, mask);
          break;
      }

      if (ph_job_have_deferred_items(thread)) {
        ph_job_pool_apply_deferred_items(thread);
      }
      ph_thread_epoch_end();
      ph_job_collector_emitter_call(emitter);
      ph_thread_epoch_poll();
    }
  }

  free(event);
}

ph_result_t ph_nbio_emitter_apply_io_mask(struct ph_nbio_emitter *emitter,
    ph_job_t *job, ph_iomask_t mask)
{
  int res;
  int want_mask = 0;

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
      res = port_dissociate(emitter->io_fd, PORT_SOURCE_FD, job->fd);
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
      res = port_associate(emitter->io_fd, PORT_SOURCE_FD, job->fd,
          want_mask, job);
      if (res != 0) {
        ph_panic("port_associate: setting mask to %02x on fd %d -> `Pe%d",
            mask, job->fd, errno);
        return PH_ERR;
      }
  }
  return PH_OK;
}

ph_iomask_t ph_job_get_kmask(ph_job_t *job)
{
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
}

#endif

/* vim:ts=2:sw=2:et:
 */

