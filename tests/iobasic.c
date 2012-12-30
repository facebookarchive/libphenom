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
#include "phenom/thread.h"
#include "phenom/sysutil.h"
#include "tap.h"

static phenom_work_item_t pipe_work;
static int pipe_fd[2];
static phenom_time_t start_time;
static int ticks = 0;

static void *ping_thread(void *arg)
{
  phenom_time_t delay = (intptr_t)arg;
  struct timespec ts = { 0, 0 };

  ts.tv_sec = delay / 1000;
  ts.tv_nsec = (delay - (ts.tv_sec * 1000)) * 1000000;

  ck_pr_store_64((uint64_t*)&start_time, phenom_time_now());
  nanosleep(&ts, NULL);

  ignore_result(write(pipe_fd[1], "a", 1));
  return NULL;
}

static bool ping_pipe_in(phenom_time_t delay)
{
  phenom_thread_t *thr;

  thr = phenom_spawn_thread(ping_thread, (void*)(intptr_t)delay);
  return thr != NULL;
}

static void pipe_dispatch(phenom_work_item_t *work, uint32_t trigger,
    phenom_time_t now, void *workdata, intptr_t triggerdata)
{
  char buf;
  phenom_time_t diff;

  unused_parameter(triggerdata);
  unused_parameter(workdata);
  unused_parameter(trigger);

  diff = now - start_time;
  ok(diff >= 80 && diff <= 120, "time diff is %" PRIi64, diff);

  is(1, read(pipe_fd[0], &buf, sizeof(buf)));

  if (ticks++ < 3) {
    ping_pipe_in(100);
    phenom_work_io_event_mask_set(work, pipe_fd[0], PHENOM_IO_MASK_READ);
  } else {
    phenom_sched_stop();
  }
}

int main(int argc, char **argv)
{
  unused_parameter(argc);
  unused_parameter(argv);

  plan_tests(13);

  is(PHENOM_OK, phenom_sched_init(0, 0));
  is(PHENOM_OK, phenom_work_init(&pipe_work));
  pipe_work.callback = pipe_dispatch;
  is(0, phenom_pipe(pipe_fd, PH_PIPE_NONBLOCK));
  phenom_work_io_event_mask_set(&pipe_work,
      pipe_fd[0], PHENOM_IO_MASK_READ);
  phenom_work_trigger_enable(&pipe_work);

  ok(ping_pipe_in(100), "set up ping");

  is(PHENOM_OK, phenom_sched_run());

  return exit_status();
}


/* vim:ts=2:sw=2:et:
 */

