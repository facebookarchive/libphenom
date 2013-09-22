/*
 * Copyright 2013-present Facebook, Inc.
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
#include "phenom/thread.h"
#include "phenom/job.h"
#include "phenom/stream.h"

static bool initialized = false;

ph_result_t ph_library_init(void)
{
  ph_result_t res;

  if (initialized) {
    return PH_OK;
  }

  if (!ph_thread_init()) {
    return PH_ERR;
  }

  res = ph_job_pool_init();
  if (res != PH_OK) {
    return res;
  }

  res = ph_stm_init();
  if (res != PH_OK) {
    return res;
  }

  initialized = true;
  return PH_OK;
}

/* vim:ts=2:sw=2:et:
 */

