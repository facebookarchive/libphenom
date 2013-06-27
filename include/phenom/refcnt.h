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

#ifndef PHENOM_REFCNT_H
#define PHENOM_REFCNT_H

/**
 * # Reference Counting
 *
 * Helpers for working with reference counters in C.
 * These delegate to Concurrency Kit and use the
 * primitive fetch-and-add functions (ck_pr_faa_XXX).
 */

#include <ck_pr.h>

#ifdef __cplusplus
extern "C" {
#endif

/** holds a reference count */
typedef int ph_refcnt_t;

/** adds a reference */
static inline void ph_refcnt_add(ph_refcnt_t *ref)
{
  ck_pr_inc_int(ref);
}

/** releases a reference
 * Returns true if we just released the final reference */
static inline bool ph_refcnt_del(ph_refcnt_t *ref)
{
  return ck_pr_faa_int(ref, -1) == 1;
}

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

