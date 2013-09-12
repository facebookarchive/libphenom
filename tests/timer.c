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
#include "phenom/sysutil.h"
#include "tap.h"

static int ticks = 0;
static struct timeval last_tick;

static void record_tick(ph_job_t *job, ph_iomask_t why,
    void *data)
{
  struct timeval now, diff;
  int64_t diffn;

  ph_unused_parameter(why);
  ph_unused_parameter(data);

  now = ph_time_now();
  // ~100ms resolution
  timersub(&now, &last_tick, &diff);
  diffn = (diff.tv_sec * 1000) + (diff.tv_usec / 1000);
  ok(diffn >= 80 && diffn <= 120, "100ms resolution: diff=%d", (int)diffn);
  last_tick = now;

  if (ticks++ < 3) {
    ph_job_set_timer_in_ms(job, 100);
  } else {
    // Stop the scheduler now
    ph_sched_stop();
  }
}

int main(int argc, char **argv)
{
  ph_job_t timer;
  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  plan_tests(8);

  is(PH_OK, ph_nbio_init(0));
  is(PH_OK, ph_job_init(&timer));

  timer.callback = record_tick;
  last_tick = ph_time_now();
  is(PH_OK, ph_job_set_timer_at(&timer, last_tick));

  is(PH_OK, ph_sched_run());

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

