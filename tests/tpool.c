/*
 * Copyright 2013 Facebook, Inc.
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
#include "phenom/sysutil.h"
#include "phenom/job.h"
#include "phenom/log.h"
#include "tap.h"

#define NUM_BUSY_JOBS 1024 /* must be power-of-2 >= 4 */

static ph_thread_pool_t *mypool, *otherpool;

static ph_job_t myjob, timer;
static struct {
  ph_job_t j CK_CC_CACHELINE;
} busyjobs[NUM_BUSY_JOBS];
static int should_stop = 0;
static uint64_t sampled_runs = 0;
static struct timeval tick_start, tick_stop;

static char *commaprint(uint64_t n, char *retbuf, uint32_t size)
{
  char *p = retbuf + size - 1;
  int i = 0;

  *p = '\0';
  do {
    if (i % 3 == 0 && i != 0) {
      *--p = ',';
    }
    *--p = '0' + n % 10;
    n /= 10;
    i++;
  } while (n != 0);

  return p;
}

static void busyjob(ph_job_t *job, ph_iomask_t why, void *data)
{
  ph_unused_parameter(why);
  ph_unused_parameter(data);

  // stack it up again
  //ph_job_set_pool(job, otherpool);
  ph_job_set_pool_immediate(job, otherpool);
}

static void jobfunc(ph_job_t *job, ph_iomask_t why, void *data)
{
  ph_unused_parameter(job);
  ph_unused_parameter(why);
  ph_unused_parameter(data);

  ok(1, "executed jobfunc");

  if (should_stop++) {
    struct ph_thread_pool_stats stats;
    ph_thread_pool_stat(otherpool, &stats);
    sampled_runs = stats.num_dispatched;
    gettimeofday(&tick_stop, 0);
    diag("prod sleeps %" PRIi64 " cons sleeps %" PRIi64,
        stats.producer_sleeps, stats.consumer_sleeps);
    ph_sched_stop();
    return;
  }

  is(PH_OK, ph_job_set_timer_in_ms(&timer, 300));
}

static void sched_job(ph_job_t *job, ph_iomask_t why, void *data)
{
  ph_unused_parameter(job);
  ph_unused_parameter(why);
  ph_unused_parameter(data);

  ok(1, "executed timer");
  ph_job_init(&myjob);
  myjob.callback = jobfunc;
  ph_job_set_pool(&myjob, mypool);
  diag("scheduled jobfunc to mypoll");
}

static void overall_failure(ph_job_t *job, ph_iomask_t why, void *data)
{
  ph_unused_parameter(job);
  ph_unused_parameter(why);
  ph_unused_parameter(data);

  ok(0, "took too long to complete");
  ph_sched_stop();
}

int main(int argc, char **argv)
{
  int i;
  ph_job_t fail_timer;
  int num_cores;
  struct timeval tdiff;
  double duration, jps;
  char niceb[32];

  ph_unused_parameter(argc);
  ph_unused_parameter(argv);

  ph_library_init();
  plan_tests(11);

  is(PH_OK, ph_nbio_init(0));

  mypool = ph_thread_pool_define("mine", 64, 1);
  ok(mypool != NULL, "made pool");

  num_cores = ph_num_cores();
  otherpool = ph_thread_pool_define("other", NUM_BUSY_JOBS, num_cores);
  ok(otherpool != NULL, "made otherpool");
  for (i = 0; i < NUM_BUSY_JOBS; i++) {
    ph_job_init(&busyjobs[i].j);
    busyjobs[i].j.callback = busyjob;
    ph_job_set_pool(&busyjobs[i].j, otherpool);
  }

  is(PH_OK, ph_job_init(&timer));
  timer.callback = sched_job;
  is(PH_OK, ph_job_set_timer_in_ms(&timer, 100));

  ph_job_init(&fail_timer);
  fail_timer.callback = overall_failure;
  ph_job_set_timer_in_ms(&fail_timer, 60000);

  diag("starting scheduler now");
  gettimeofday(&tick_start, NULL);
  is(PH_OK, ph_sched_run());

  timersub(&tick_stop, &tick_start, &tdiff);
  duration = tdiff.tv_sec + (tdiff.tv_usec/1000000.0f);
  jps = sampled_runs / duration;

  diag("total %s jobs on %d threads in %.3fs",
      commaprint(sampled_runs, niceb, sizeof(niceb)),
      num_cores, duration);
  diag("jps      %s",
      commaprint(jps, niceb, sizeof(niceb)));
  diag("jps/core %s",
      commaprint((uint64_t)(jps/num_cores), niceb, sizeof(niceb)));

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */

