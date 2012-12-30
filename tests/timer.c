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
#include "tap.h"

static int ticks = 0;
static phenom_time_t last_tick;

static void record_tick(phenom_work_item_t *work, uint32_t trigger,
    phenom_time_t now, void *workdata, intptr_t triggerdata)
{
  phenom_time_t diff;

  unused_parameter(work);
  unused_parameter(trigger);
  unused_parameter(triggerdata);
  unused_parameter(workdata);

  // ~100ms resolution
  diff = now - last_tick;
  ok(diff >= 80 && diff <= 120, "100ms resolution: diff=%d", diff);
  last_tick = now;

  if (ticks++ < 3) {
    phenom_work_timeout_at(work, now + 1);
  } else {
    // Stop the scheduler now
    phenom_sched_stop();
  }
}

int main(int argc, char **argv)
{
  phenom_work_item_t timer;
  unused_parameter(argc);
  unused_parameter(argv);

  plan_tests(10);

  is(PHENOM_OK, phenom_sched_init(0, 0));
  is(PHENOM_OK, phenom_work_init(&timer));
  is_true(phenom_time_now() != 0);

  timer.callback = record_tick;
  last_tick = phenom_time_now();
  is(PHENOM_OK, phenom_work_timeout_at(&timer, last_tick));
  is(PHENOM_OK, phenom_work_trigger_enable(&timer));

  is(PHENOM_OK, phenom_sched_run());

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

