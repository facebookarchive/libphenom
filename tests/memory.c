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

#include "phenom/memory.h"
#include "phenom/counter.h"
#include "phenom/sysutil.h"
#include "tap.h"

static void dump_mem_stats(void)
{
  phenom_mem_stats_t stats[1];
  phenom_memtype_t base = 0;

  while (1) {
    int n, i;

    n = phenom_mem_stat_range(base,
          base + (sizeof(stats) / sizeof(stats[0])), stats);

    for (i = 0; i < n; i++) {
      diag("%s %s bytes=%lu oom=%lu allocs=%lu frees=%lu reallocs=%lu",
          stats[i].def->facility, stats[i].def->name,
          stats[i].bytes, stats[i].oom, stats[i].allocs,
          stats[i].frees, stats[i].reallocs);
    }

    if ((uint32_t)n < sizeof(stats) / sizeof(stats[0])) {
      break;
    }

    base += n;
  }
}

struct widget {
  int aval;
};

int main(int argc, char** argv)
{
  uint32_t i;

  unused_parameter(argc);
  unused_parameter(argv);

  plan_tests(46);

  phenom_memtype_def_t defs[] = {
    { "memtest1", "widget", sizeof(struct widget), PHENOM_MEM_FLAGS_ZERO },
    { "memtest1", "string", 0, PHENOM_MEM_FLAGS_ZERO },
  };
  phenom_memtype_t types[sizeof(defs) / sizeof(defs[0])];
  phenom_memtype_t mt = phenom_memtype_register_block(
      sizeof(defs) / sizeof(defs[0]),
      defs, types);
  is_true(mt != PHENOM_MEMTYPE_INVALID);

  for (i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
    is(mt + i, (uint32_t)types[i]);
  }

  struct widget *w;
  struct widget null_widget = {0};

  w = (struct widget*)phenom_mem_alloc(types[0]);
  is(0, memcmp(w, &null_widget, sizeof(*w)));

  phenom_mem_stats_t st;
  phenom_mem_stat(types[0], &st);
  is(1, st.allocs);
  is(sizeof(*w), st.bytes);
  is(0, st.frees);
  is(0, st.reallocs);

  phenom_mem_free(types[0], w);
  phenom_mem_stat(types[0], &st);
  is(1, st.allocs);
  is(0, st.bytes);
  is(1, st.frees);
  is(0, st.reallocs);

  char *buf1 = (char*)phenom_mem_alloc_size(types[1], 24);
  is_true(buf1 != NULL);
  is(0, buf1[0]);

  phenom_mem_stat(types[1], &st);
  is(1, st.allocs);
  is(24, st.bytes);
  is(0, st.frees);
  is(0, st.reallocs);

  char *buf2 = (char*)phenom_mem_alloc_size(types[1], 53);
  is_true(buf2 != NULL);
  is(0, buf2[0]);

  phenom_mem_stat(types[1], &st);
  is(2, st.allocs);
  is(77, st.bytes);
  is(0, st.frees);
  is(0, st.reallocs);

  strcpy(buf2, "Hello");
  buf2 = (char*)phenom_mem_realloc(types[1], buf2, 4);
  // Verify that we see the same first few bytes
  is(0, memcmp(buf2, "Hello", 4));

  phenom_mem_stat(types[1], &st);
  is(2, st.allocs);
  is(28, st.bytes);
  is(0, st.frees);
  is(1, st.reallocs);

  phenom_mem_free(types[1], buf1);

  phenom_mem_stat(types[1], &st);
  is(2, st.allocs);
  is(4, st.bytes);
  is(1, st.frees);
  is(1, st.reallocs);

  phenom_mem_free(types[1], buf2);

  phenom_mem_stat(types[1], &st);
  is(2, st.allocs);
  is(0, st.bytes);
  is(2, st.frees);
  is(1, st.reallocs);

  char *strp;
  phenom_mtsprintf(types[1], &strp, "testing %s", "format");
  is_string(strp, "testing format");

  phenom_mem_stat(types[1], &st);
  is(3, st.allocs);
  is(128, st.bytes);
  is(2, st.frees);
  is(1, st.reallocs);

  phenom_mem_free(types[1], strp);

  phenom_mem_stat(types[1], &st);
  is(3, st.allocs);
  is(0, st.bytes);
  is(3, st.frees);
  is(1, st.reallocs);

  dump_mem_stats();

  return exit_status();
}


/* vim:ts=2:sw=2:et:
 */

