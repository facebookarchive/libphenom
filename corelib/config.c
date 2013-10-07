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
#include "phenom/configuration.h"
#include <ck_rwlock.h>
#include "phenom/stream.h"
#include "phenom/json.h"
#include "phenom/log.h"
#include "phenom/printf.h"

static ph_variant_t *global_config = NULL;
static ck_rwlock_t lock = CK_RWLOCK_INITIALIZER;

void ph_config_set_global(ph_variant_t *cfg)
{
  ph_variant_t *old;

  // global_conf will take a ref on cfg
  ph_var_addref(cfg);

  ck_rwlock_write_lock(&lock);
  {
    old = ck_pr_load_ptr(&global_config);
    ck_pr_store_ptr(&global_config, cfg);
  }
  ck_rwlock_write_unlock(&lock);

  if (old) {
    // We took the ref owned by global_config
    ph_var_delref(old);
  }
}

ph_variant_t *ph_config_get_global(void)
{
  ph_variant_t *ref;

  ck_rwlock_read_lock(&lock);
  ref = ck_pr_load_ptr(&global_config);
  if (ref) {
    ph_var_addref(ref);
  }
  ck_rwlock_read_unlock(&lock);
  if (!ref) {
    ref = ph_var_null();
  }
  return ref;
}

ph_variant_t *ph_config_query(const char *query)
{
  ph_variant_t *val, *g;

  g = ph_config_get_global();
  val = ph_var_jsonpath_get(g, query);
  if (val) {
    ph_var_addref(val);
  }
  ph_var_delref(g);

  return val;
}

int64_t ph_config_queryf_int(int64_t defval, const char *query, ...)
{
  char expanded[1024];
  int len;
  va_list ap;

  va_start(ap, query);
  len = ph_vsnprintf(expanded, sizeof(expanded), query, ap);
  va_end(ap);

  if ((uint32_t)len >= sizeof(expanded)-1) {
    ph_panic("query '%s' expansion too large", query);
  }

  return ph_config_query_int(expanded, defval);
}

int64_t ph_config_query_int(const char *query, int64_t defval)
{
  ph_variant_t *v = ph_config_query(query);
  int64_t retval = defval;

  if (v) {
    if (ph_var_is_int(v)) {
      retval = ph_var_int_val(v);
    }
    ph_var_delref(v);
  }

  return retval;
}

double ph_config_query_double(const char *query, double defval)
{
  ph_variant_t *v = ph_config_query(query);
  double retval = defval;

  if (v) {
    if (ph_var_is_double(v)) {
      retval = ph_var_double_val(v);
    }
    ph_var_delref(v);
  }

  return retval;
}

ph_string_t *ph_config_query_string(const char *query, ph_string_t *defval)
{
  ph_variant_t *v = ph_config_query(query);
  ph_string_t *retval = NULL;

  if (v) {
    if (ph_var_is_string(v)) {
      retval = ph_var_string_val(v);
      if (retval) {
        ph_string_addref(retval);
      }
    }
    ph_var_delref(v);
  }

  if (retval == NULL && defval) {
    retval = defval;
    ph_string_addref(retval);
  }

  return retval;
}

ph_string_t *ph_config_query_string_cstr(const char *query, const char *defval)
{
  ph_variant_t *v = ph_config_query(query);
  ph_string_t *retval = NULL;

  if (v) {
    if (ph_var_is_string(v)) {
      retval = ph_var_string_val(v);
      if (retval) {
        ph_string_addref(retval);
      }
    }
    ph_var_delref(v);
  }

  if (retval == NULL && defval) {
    v = ph_var_string_make_cstr(defval);
    retval = ph_var_string_val(v);
    ph_string_addref(retval);
    ph_var_delref(v);
  }

  return retval;
}


bool ph_config_load_config_file(const char *config_path)
{
  ph_stream_t *stm;
  ph_variant_t *v;
  ph_var_err_t err;

  if (!config_path) {
    config_path = getenv("PHENOM_CONFIG_FILE");
    if (config_path && strlen(config_path) &&
        access(config_path, R_OK)) {
      config_path = NULL;
    }
  }

  if (!config_path) {
    return false;
  }

  stm = ph_stm_file_open(config_path, O_RDONLY, 0);
  if (!stm) {
    return false;
  }
  v = ph_json_load_stream(stm, 0, &err);
  ph_stm_close(stm);

  if (!v) {
    ph_log(PH_LOG_ERR, "load_config(%s): %s",
        config_path, err.text);
    return false;
  }

  ph_config_set_global(v);
  ph_var_delref(v);
  return true;
}

/* vim:ts=2:sw=2:et:
 */

