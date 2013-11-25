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
#include "phenom/defs.h"
#include "phenom/socket.h"
#include "phenom/printf.h"
#include "corelib/gimli/gimli.h"

// missing from gimli header
extern gimli_proc_t the_proc;

static char *bare_proc_sym_name(const struct gimli_ana_api *api,
    void *pcaddr, char *buf, int bufsize)
{
  char *bt, *plus;
  if (!api->sym_name(pcaddr, buf, bufsize)) {
    return NULL;
  }

  bt = strchr(buf, '`');
  if (!bt) {
    return buf;
  }
  buf = bt + 1;
  plus = strchr(buf, '+');
  if (plus) {
    *plus = '\0';
  }
  return buf;
}

static struct {
  const char *names[8];
} suppress_threads[] = {
  { {"syscall", "wait_pool", "worker_thread", NULL} },
};

static ph_thread_t *tid_to_thread(int tid)
{
  ph_thread_t *threads;
  uint32_t nthreads, i;

  threads = ph_gimli_get_threads(the_proc, &nthreads);
  for (i = 0; i < nthreads; i++) {
    if (threads[i].lwpid == tid) {
      return &threads[i];
    }
  }
  return NULL;
}

// Suppress parked threads
static int begin_thread_trace(const struct gimli_ana_api *api,
    const char *object, int tid, int nframes,
    void **pcaddrs, void **contexts)
{
  uint32_t i;
  char fname[1024];
  const char *name;
  ph_thread_t *thr;

  ph_unused_parameter(object);
  ph_unused_parameter(tid);
  ph_unused_parameter(contexts);

  thr = tid_to_thread(tid);

  if (!thr || !ph_gimli_is_thread_interesting(the_proc, thr)) {
    for (i = 0; i < sizeof(suppress_threads)/sizeof(suppress_threads[0]); i++) {
      uint32_t j;
      bool matched = true;

      for (j = 0; suppress_threads[i].names[j]; j++) {
        if (j >= (uint32_t)nframes) {
          matched = false;
          break;
        }
        name = bare_proc_sym_name(api, pcaddrs[j], fname, sizeof(fname));
        if (strcmp(name, suppress_threads[i].names[j])) {
          matched = false;
          break;
        }
      }

      if (matched) {
        return GIMLI_ANA_SUPPRESS;
      }
    }
  }

  if (thr) {
    ph_gimli_print_summary_for_thread(the_proc, thr);
  }

  return GIMLI_ANA_CONTINUE;
}

// We don't need to drill so deep in some cases
static int before_print_frame(const struct gimli_ana_api *api,
    const char *object, int tid, int frameno, void *pcaddr, void *context)
{
  char fname[1024];
  const char *name;

  ph_unused_parameter(object);
  ph_unused_parameter(tid);
  ph_unused_parameter(frameno);
  ph_unused_parameter(context);

  name = bare_proc_sym_name(api, pcaddr, fname, sizeof(fname));
  if (!strcmp(name, "ph_thread_boot")) {
    return GIMLI_ANA_SUPPRESS;
  }

  return GIMLI_ANA_CONTINUE;
}

static struct gimli_ana_module mod = {
  GIMLI_ANA_API_VERSION,
  NULL, // perform_trace
  begin_thread_trace,
  before_print_frame, // before print frame
  NULL, // before print frame var
  NULL, // after print frame var
  NULL, // after print frame
  NULL, // end thread trace
};

struct gimli_ana_module *gimli_ana_init(const struct gimli_ana_api *api)
{
  ph_unused_parameter(api);
  return &mod;
}

struct printer {
  const char *tname;
  ph_gimli_type_printer func;
  int size;
  // space is reserved here for a copy of the type
};

static gimli_iter_status_t do_print_type(gimli_proc_t proc,
  gimli_stack_frame_t frame, const char *varname, gimli_type_t t,
  gimli_addr_t addr, int depth, void *arg)
{
  struct printer *p = arg;
  void *space = (p + 1);
  int i;

  for (i = 0; i <= depth; i++) {
    printf("    ");
  }

  if (gimli_read_mem(proc, addr, space, p->size) != p->size) {
    printf("%s %s = <invalid address %p>\n",
        p->tname, varname, (void*)(intptr_t)addr);
    return GIMLI_ITER_ERR;
  }

  return p->func(proc, frame, varname, t, addr, depth, space);
}

int ph_gimli_register_type_printer(const char *tname,
    size_t sizeoftname,
    ph_gimli_type_printer func)
{
  struct printer *p = calloc(1, sizeof(*p) + sizeoftname);
  p->tname = strdup(tname);
  p->size = sizeoftname;
  p->func = func;

  return gimli_module_register_var_printer_for_types(&p->tname, 1,
      do_print_type, p);
}

int gimli_module_init(int api_version)
{
  if (api_version != GIMLI_ANA_API_VERSION) {
    return 0;
  }

  // init phenom, but also any PH_GIMLI_INIT routines in this object
  ph_library_init();

  return 1;
}

/* vim:ts=2:sw=2:et:
 */

