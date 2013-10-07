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
#include "phenom/string.h"
#include "phenom/listener.h"
#include "phenom/dns.h"
#include "phenom/log.h"
#include "phenom/printf.h"
#include "phenom/counter.h"

/* Implements a debug console server that is useful while developing
 * and debugging an implementation.  There is no authentication beyond
 * UNIX permissions and ownership rules.  You should only enable this
 * during development.
 *
 * Usage:  `echo "memory" | nc -UC /unix/sock/path`
 *
 * The protocol is super simple: once the session is established,
 * we expect to receive a single line that identifies the diagnostic
 * function to be run.
 *
 * We compare that against a static list of functions declared in
 * this file; if we find a match, we execute the diagnostic and it
 * prints its output over the socket.
 *
 * Afterwards, we shutdown and close the socket.
 *
 * There is no persistence and no authentication; it is single
 * command only.
 */

typedef void (*console_cmd)(ph_sock_t *sock);

// Query memory stats
static void cmd_memory(ph_sock_t *sock)
{
  ph_mem_stats_t stats[1];
  ph_memtype_t base = PH_MEMTYPE_FIRST;
  char name[29];

  ph_stm_printf(sock->stream,
      "%28s %9s %9s %9s %9s %9s\r\n",
      "WHAT", "BYTES", "OOM", "ALLOCS", "FREES", "REALLOC");

  while (1) {
    int n, i;

    n = ph_mem_stat_range(base,
          base + (sizeof(stats) / sizeof(stats[0])), stats);

    for (i = 0; i < n; i++) {
      ph_snprintf(name, sizeof(name),
          "%s/%s", stats[i].def->facility, stats[i].def->name);

      ph_stm_printf(sock->stream,
          "%28s "
          "%9"PRIu64" "
          "%9"PRIu64" "
          "%9"PRIu64" "
          "%9"PRIu64" "
          "%9"PRIu64" "
          "\r\n",
          name,
          stats[i].bytes, stats[i].oom, stats[i].allocs,
          stats[i].frees, stats[i].reallocs);
    }

    if ((uint32_t)n < sizeof(stats) / sizeof(stats[0])) {
      break;
    }

    base += n;
  }
}

struct counter_name_val {
  const char *scope_name;
  const char *name;
  int64_t val;
};

static int compare_counter_name_val(const void *a, const void *b)
{
  struct counter_name_val *A = (struct counter_name_val*)a;
  struct counter_name_val *B = (struct counter_name_val*)b;
  int diff;

  diff = strcmp(A->scope_name, B->scope_name);
  if (diff) return diff;

  return strcmp(A->name, B->name);
}

// Query all counters except for memory counters.
static void cmd_counters(ph_sock_t *sock)
{
#define NUM_SLOTS 64
#define NUM_COUNTERS 2048
  struct counter_name_val counter_data[NUM_COUNTERS];
  int64_t view_slots[NUM_SLOTS];
  const char *view_names[NUM_SLOTS];
  ph_counter_scope_iterator_t iter;
  uint32_t num_slots, i;
  uint32_t n_counters = 0;
  uint32_t longest_name = 0;
  char name[69];

  // Collect all counter data; it is returned in an undefined order.
  // For the sake of testing we want to order it, so we collect the data
  // and then sort it
  ph_counter_scope_iterator_init(&iter);
  ph_counter_scope_t *iter_scope;
  while ((iter_scope = ph_counter_scope_iterator_next(&iter)) != NULL) {
    uint32_t slen;

    if (strncmp(ph_counter_scope_get_name(iter_scope), "memory/", 7) == 0) {
      ph_counter_scope_delref(iter_scope);
      continue;
    }

    slen = strlen(ph_counter_scope_get_name(iter_scope));

    num_slots = ph_counter_scope_get_view(iter_scope, NUM_SLOTS,
        view_slots, view_names);

    for (i = 0; i < num_slots; i++) {
      uint32_t l = strlen(view_names[i]);

      longest_name = MAX(longest_name, l + slen + 1);
      counter_data[n_counters].scope_name =
        ph_counter_scope_get_name(iter_scope);
      counter_data[n_counters].name = view_names[i];
      counter_data[n_counters].val = view_slots[i];
      n_counters++;

      if (n_counters >= NUM_COUNTERS) {
        break;
      }
    }

    ph_counter_scope_delref(iter_scope);

    if (n_counters >= NUM_COUNTERS) {
      break;
    }
  }

  qsort(counter_data, n_counters, sizeof(struct counter_name_val),
      compare_counter_name_val);

  for (i = 0; i < n_counters; i++) {
    ph_snprintf(name, sizeof(name), "%s/%s",
        counter_data[i].scope_name,
        counter_data[i].name);
    ph_stm_printf(sock->stream, "%*s %16" PRIi64"\r\n",
        longest_name,
        name, counter_data[i].val);
  }

  if (n_counters >= NUM_COUNTERS) {
    ph_stm_printf(sock->stream,
        "WARNING: too many counters to sort, output truncated\r\n");
  }
}

static struct {
  const char *name;
  console_cmd func;
} funcs[] = {
  { "memory", cmd_memory },
  { "counters", cmd_counters },
};

static void debug_con_processor(ph_sock_t *sock, ph_iomask_t why, void *arg)
{
  if (why & (PH_IOMASK_ERR|PH_IOMASK_TIME)) {
done:
    ph_sock_shutdown(sock, PH_SOCK_SHUT_RDWR);
    ph_sock_free(sock);
    return;
  }
  if (arg && (why & PH_IOMASK_WRITE) && ph_bufq_len(sock->wbuf) == 0) {
    goto done;
  }

  if (arg == NULL && (why & PH_IOMASK_READ)) {
    ph_buf_t *buf;
    char *cmd;
    int len;
    uint32_t i;

    if (sock->job.data) {
      // We've already served one; call it done
      goto done;
    }

    buf = ph_sock_read_line(sock);
    if (!buf) {
      return;
    }

    sock->job.data = buf;
    ph_sock_shutdown(sock, PH_SOCK_SHUT_RD);

    cmd = (char*)ph_buf_mem(buf);
    len = ph_buf_len(buf);
    /* replace CRLF with NUL termination */
    cmd[len - 2] = '\0';

    for (i = 0; i < sizeof(funcs)/sizeof(funcs[0]); i++) {
      if (strcmp(funcs[i].name, cmd)) {
        continue;
      }

      funcs[i].func(sock);
      break;
    }

    ph_buf_delref(buf);
  }
}

static void acceptor(ph_listener_t *lstn, ph_sock_t *sock)
{
  ph_unused_parameter(lstn);

  sock->callback = debug_con_processor;
  ph_sock_enable(sock, true);
}


void ph_debug_console_start(const char *unix_sock_path)
{
  ph_sockaddr_t addr;
  ph_listener_t *lstn;

  if (unix_sock_path == NULL) {
    unix_sock_path = "/tmp/phenom-debug-console";
  }

  ph_sockaddr_set_unix(&addr, unix_sock_path, 0);
  lstn = ph_listener_new("debug-console", acceptor);
  ph_listener_bind(lstn, &addr);
  ph_listener_enable(lstn, true);
}

/* vim:ts=2:sw=2:et:
 */

