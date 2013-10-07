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
#include <ck_stack.h>
#include <ck_spinlock.h>

static int initialized = 0;
static ck_stack_t init_func_stack = CK_STACK_INITIALIZER;
static struct ph_library_init_entry **init_funcs = NULL;
static int num_init_ents = 0;
static ck_spinlock_t init_lock = CK_SPINLOCK_INITIALIZER;

void ph_library_init_register(struct ph_library_init_entry *ent)
{
  ph_static_assert(sizeof(ent->st) == sizeof(ck_stack_entry_t),
      must_match_ck_stack_entry_t);

  if (ph_unlikely(initialized)) {
    ph_panic("attempted to register %s after ph_library_init() was called",
        ent->file);
  }

  ck_stack_push_mpmc(&init_func_stack, (ck_stack_entry_t*)(void*)&ent->st);
  ck_pr_inc_int(&num_init_ents);
}

static void ph_library_teardown(void)
{
  struct ph_library_init_entry *ent;
  int i;

  // Destroy in reverse priority order
  for (i = num_init_ents - 1 ; i >= 0; i--) {
    ent = init_funcs[i];
    if (ent->fini) {
      ent->fini();
    }
  }

  free(init_funcs);
  init_funcs = NULL;
}

static int compare_ent(const void *A, const void *B)
{
  struct ph_library_init_entry *a = *(void**)A;
  struct ph_library_init_entry *b = *(void**)B;
  int diff;

  // priority first
  diff = a->pri - b->pri;
  if (diff) {
    return diff;
  }

  // then by file
  diff = strcmp(a->file, b->file);
  if (diff) {
    return diff;
  }

  // then by the order of declaration in the file
  diff = a->line - b->line;
  if (diff) {
    return diff;
  }

  // they managed to put two on the same line, so order
  // by address in the data segment.  This is probably
  // the same as order of declaration in the file but
  // at least yields a stable ordering between invocations
  // of this same binary
  return (ptrdiff_t)a - (ptrdiff_t)b;
}

CK_STACK_CONTAINER(struct ph_library_init_entry,
    st, ent_from_stack)

ph_result_t ph_library_init(void)
{
  ck_stack_entry_t *st;
  int i = 0;

  if (ck_pr_load_int(&initialized)) {
    ph_thread_self_slow();
    return PH_OK;
  }

  ck_spinlock_lock(&init_lock);

  if (ck_pr_load_int(&initialized)) {
    ck_spinlock_unlock(&init_lock);
    ph_thread_self_slow();
    return PH_OK;
  }

  // Sort according to priority
  init_funcs = malloc(num_init_ents * sizeof(*init_funcs));
  CK_STACK_FOREACH(&init_func_stack, st) {
    init_funcs[i++] = ent_from_stack(st);
  }
  qsort(init_funcs, num_init_ents, sizeof(void*), compare_ent);

  // arrange to destroy stuff at the end
  atexit(ph_library_teardown);

  // Now initialize in priority order
  for (i = 0; i < num_init_ents; i++) {
    struct ph_library_init_entry *ent = init_funcs[i];
    if (ent->init) {
      ent->init();
    }
  }

  ck_pr_store_int(&initialized, 1);
  ck_spinlock_unlock(&init_lock);
  ph_thread_self_slow();
  return PH_OK;
}

/* vim:ts=2:sw=2:et:
 */

