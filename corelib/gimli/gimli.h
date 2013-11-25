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

#ifndef CORELIB_GIMLI_GIMLI_H
#define CORELIB_GIMLI_GIMLI_H

#include "phenom/sysutil.h"
#include "phenom/defs.h"
#include "phenom/printf.h"
#include "phenom/counter.h"
#include <libgimli_ana.h>

// Register stuff with gimli
#define PH_GIMLI_INIT(initfn) PH_LIBRARY_INIT_PRI(initfn, 0, 9000)

typedef gimli_iter_status_t (*ph_gimli_type_printer)(
    // The target process
    gimli_proc_t proc,
    // The stack frame where this var is situated
    gimli_stack_frame_t frame,
    // The variable name of this reference
    const char *varname,
    // Type information
    gimli_type_t t,
    // The address of the data in the target
    gimli_addr_t addr,
    // Structure nesting depth
    int depth,
    // points to a copy of the type in the current process address space
    void *ptr);

int ph_gimli_register_type_printer(const char *tname,
    size_t sizeoftname,
    ph_gimli_type_printer func);

#define PH_GIMLI_TYPE_INNER(tname, fname, initfn) \
static void initfn(void) { \
  ph_gimli_register_type_printer(#tname, sizeof(tname), fname); \
} \
PH_GIMLI_INIT(initfn)
#define PH_GIMLI_TYPE(tname, fname) \
  PH_GIMLI_TYPE_INNER(tname, fname, ph_defs_gen_symbol(regtype))

ph_thread_t *ph_gimli_get_threads(gimli_proc_t proc, uint32_t *nthreads);
bool ph_gimli_is_thread_interesting(gimli_proc_t proc, ph_thread_t *thr);
void ph_gimli_print_summary_for_thread(gimli_proc_t proc, ph_thread_t *thr);

// Helper for visiting each value in a ck_hs_t
struct ph_gimli_ck_hs_iter {
  void **entries;
  uint32_t offset, capacity, size;
  bool pp;
};

bool ph_gimli_ck_hs_iter_init(gimli_proc_t proc, gimli_addr_t hsptr,
    struct ph_gimli_ck_hs_iter *iter);
bool ph_gimli_ck_hs_iter_init_from_map(gimli_proc_t proc, gimli_addr_t mapptr,
    struct ph_gimli_ck_hs_iter *iter);
bool ph_gimli_ck_hs_iter_next(struct ph_gimli_ck_hs_iter *iter,
    gimli_addr_t *addr);
void ph_gimli_ck_hs_iter_destroy(struct ph_gimli_ck_hs_iter *iter);

#endif

/* vim:ts=2:sw=2:et:
 */

