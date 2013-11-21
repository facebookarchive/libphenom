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
#include "phenom/log.h"
#include "corelib/log.h"
#include <ctype.h>

static int compare_log_entry(const void *a, const void *b)
{
  const struct log_entry *A = a;
  const struct log_entry *B = b;

  if (timercmp(&A->when, &B->when, <)) {
    return -1;
  }
  if (timercmp(&B->when, &A->when, <)) {
    return 1;
  }
  return 0;
}

static void show_logs(gimli_proc_t proc, void *unused)
{
  ph_unused_parameter(unused);
  ph_unused_parameter(proc);
  struct log_entry log_buf[PH_LOG_CIRC_ENTRIES];
  uint32_t i;

  if (!gimli_copy_from_symbol(NULL, "ph_log_buf", 0,
        log_buf, sizeof(log_buf))) {
    return;
  }

  qsort(log_buf, PH_LOG_CIRC_ENTRIES, sizeof(struct log_entry),
      compare_log_entry);

  printf("RECENT LOGS\n");
  for (i = 0; i < PH_LOG_CIRC_ENTRIES; i++) {
    int len;
    char *end;

    if (log_buf[i].msg[0] == 0) {
      continue;
    }

    end = memchr(log_buf[i].msg, 0, sizeof(log_buf[i].msg));
    if (end) {
      len = end - log_buf[i].msg;
    } else {
      len = sizeof(log_buf[i].msg);
    }
    while (len > 0 && isspace(log_buf[i].msg[len - 1])) {
      len--;
    }

    printf("%.*s\n", len, log_buf[i].msg);
  }
}

static void init(void) {
  gimli_module_register_tracer(show_logs, 0);
}
PH_GIMLI_INIT(init)

/* vim:ts=2:sw=2:et:
 */

