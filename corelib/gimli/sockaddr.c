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

#include "corelib/gimli/gimli.h"

static gimli_iter_status_t pretty_sockaddr(gimli_proc_t proc,
  gimli_stack_frame_t frame, const char *varname, gimli_type_t t,
  gimli_addr_t addr, int depth, void *arg)
{
  ph_unused_parameter(proc);
  ph_unused_parameter(frame);
  ph_unused_parameter(t);
  ph_unused_parameter(addr);
  ph_unused_parameter(depth);
  ph_unused_parameter(arg);

  fflush(stdout);
  ph_fdprintf(STDOUT_FILENO, "ph_sockaddr_t %p %s = `P{sockaddr:%p}\n",
      (void*)(intptr_t)addr, varname, arg);
  return GIMLI_ITER_STOP;
}

PH_GIMLI_TYPE(ph_sockaddr_t, pretty_sockaddr)


/* vim:ts=2:sw=2:et:
 */

