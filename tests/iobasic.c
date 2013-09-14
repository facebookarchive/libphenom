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
#include "phenom/thread.h"
#include "phenom/sysutil.h"
#include "tap.h"

static ph_job_t pipe_job;
static int pipe_fd[2];
static struct timeval start_time;
static int ticks = 0;

static void *ping_thread(void *arg)
{
  uint32_t delay = (intptr_t)arg;
  struct timespec ts = { 0, 0 };

  ts.tv_sec = delay / 1000;
  ts.tv_nsec = (delay - (ts.tv_sec * 1000)) * 1000000;

  start_time = ph_time_now();
  nanosleep(&ts, NULL);

  ph_ignore_result(write(pipe_fd[1], "a", 1));
  return NULL;
}

static bool ping_pipe_in(uint32_t delay)
{
  ph_thread_t *thr;

  thr = ph_thread_spawn(ping_thread, (void*)(intptr_t)delay);
  return thr != NULL;
}

static void pipe_dispatch(ph_job_t *job, ph_iomask_t why, void *data)
{
  char buf;
  struct timeval diff;
  int64_t diffn;
  struct timeval now = ph_time_now();

  ph_unused_parameter(why);
  ph_unused_parameter(data);

  timersub(&now, &start_time, &diff);
  diffn = (diff.tv_sec * 1000) + (diff.tv_usec / 1000);
  ok(diffn >= 80 && diffn <= 120, "100ms resolution: diff=%d", (int)diffn);

  is(1, read(pipe_fd[0], &buf, sizeof(buf)));

  if (ticks++ < 3) {
    ping_pipe_in(100);
    ph_job_set_nbio(job, PH_IOMASK_READ, 0);
  } else {
    ph_sched_stop();
  }
}

int main(int argc, char **argv)
{
  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  plan_tests(13);

  is(PH_OK, ph_nbio_init(0));
  is(PH_OK, ph_job_init(&pipe_job));
  pipe_job.callback = pipe_dispatch;
  is(0, ph_pipe(pipe_fd, PH_PIPE_NONBLOCK));
  pipe_job.fd = pipe_fd[0];
  ph_job_set_nbio(&pipe_job, PH_IOMASK_READ, 0);

  ok(ping_pipe_in(100), "set up ping");

  is(PH_OK, ph_sched_run());

  return exit_status();
}


/* vim:ts=2:sw=2:et:
 */

