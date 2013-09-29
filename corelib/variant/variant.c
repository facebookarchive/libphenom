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

#include "phenom/variant.h"
#include "phenom/log.h"
#include "phenom/sysutil.h"

static struct {
  ph_memtype_t var, arr;
} mt;

static struct ph_memtype_def defs[] = {
  { "variant", "variant", sizeof(ph_variant_t), 0 },
  { "variant", "array",   0, 0 },
};

static ph_variant_t bool_true_variant  = { 1, PH_VAR_TRUE, { 0 } };
static ph_variant_t bool_false_variant = { 1, PH_VAR_FALSE, { 0 } };
static ph_variant_t null_variant       = { 1, PH_VAR_NULL, { 0 } };

static void init_variant(void)
{
  ph_memtype_register_block(sizeof(defs)/sizeof(defs[0]), defs, &mt.var);
}
PH_LIBRARY_INIT(init_variant, 0)

ph_variant_t *ph_var_bool(bool val)
{
  ph_variant_t *var = val ? &bool_true_variant : &bool_false_variant;

  ph_var_addref(var);
  return var;
}

ph_variant_t *ph_var_null(void)
{
  ph_var_addref(&null_variant);
  return &null_variant;
}

ph_variant_t *ph_var_double(double dval)
{
  ph_variant_t *var;

  var = ph_mem_alloc(mt.var);
  if (!var) {
    return NULL;
  }

  var->ref = 1;
  var->type = PH_VAR_REAL;
  var->u.dval = dval;

  return var;
}

ph_variant_t *ph_var_int(int64_t ival)
{
  ph_variant_t *var;

  var = ph_mem_alloc(mt.var);
  if (!var) {
    return NULL;
  }

  var->ref = 1;
  var->type = PH_VAR_INTEGER;
  var->u.ival = ival;

  return var;
}

ph_variant_t *ph_var_string_claim(ph_string_t *str)
{
  ph_variant_t *var;

  if (str == NULL) {
    return ph_var_null();
  }

  var = ph_mem_alloc(mt.var);
  if (!var) {
    return NULL;
  }

  var->ref = 1;
  var->type = PH_VAR_STRING;
  var->u.sval = str;

  return var;
}

ph_variant_t *ph_var_string_make(ph_string_t *str)
{
  ph_variant_t *var;

  if (!str) {
    return ph_var_null();
  }

  var = ph_var_string_claim(str);
  if (!var) {
    return NULL;
  }

  ph_string_addref(str);

  return var;
}

void ph_var_delref(ph_variant_t *var)
{
  if (!ph_refcnt_del(&var->ref)) {
    return;
  }

  switch (var->type) {
    case PH_VAR_TRUE:
    case PH_VAR_FALSE:
    case PH_VAR_NULL:
      ph_panic("You have a refcounting problem");

    case PH_VAR_ARRAY:
      if (var->u.aval.arr) {
        uint32_t i;

        for (i = 0; i < var->u.aval.len; i++) {
          ph_var_delref(var->u.aval.arr[i]);
        }
        ph_mem_free(mt.arr, var->u.aval.arr);
        var->u.aval.arr = 0;
      }
      break;

    case PH_VAR_OBJECT:
      ph_ht_destroy(&var->u.oval);
      break;

    case PH_VAR_STRING:
      if (var->u.sval) {
        ph_string_delref(var->u.sval);
        var->u.sval = 0;
      }
      break;

    default:
      ;
  }

  ph_mem_free(mt.var, var);
}

ph_variant_t *ph_var_array(uint32_t nelems)
{
  ph_variant_t *var;

  var = ph_mem_alloc(mt.var);
  if (!var) {
    return NULL;
  }

  var->ref = 1;
  var->type = PH_VAR_ARRAY;
  var->u.aval.len = 0;
  var->u.aval.alloc = nelems;
  var->u.aval.arr = ph_mem_alloc_size(mt.arr, nelems * sizeof(ph_variant_t*));

  if (!var->u.aval.arr) {
    ph_mem_free(mt.var, var);
    return NULL;
  }

  return var;
}

ph_result_t ph_var_array_append(ph_variant_t *arr, ph_variant_t *val)
{
  ph_result_t res;

  res = ph_var_array_append_claim(arr, val);

  if (res != PH_OK) {
    return res;
  }

  ph_var_addref(val);
  return PH_OK;
}

ph_result_t ph_var_array_append_claim(ph_variant_t *arr, ph_variant_t *val)
{
  if (arr->type != PH_VAR_ARRAY) {
    return PH_ERR;
  }

  if (arr->u.aval.len + 1 >= arr->u.aval.alloc) {
    ph_variant_t **narr;
    uint32_t nsize = ph_power_2(arr->u.aval.alloc * 2);

    narr = ph_mem_realloc(mt.arr, arr->u.aval.arr,
              nsize * sizeof(ph_variant_t*));
    if (!narr) {
      return PH_NOMEM;
    }

    arr->u.aval.alloc = nsize;
    arr->u.aval.arr = narr;
  }

  arr->u.aval.arr[arr->u.aval.len++] = val;
  return PH_OK;
}

ph_result_t ph_var_array_set_claim(ph_variant_t *arr, uint32_t pos,
    ph_variant_t *val)
{
  if (arr->type != PH_VAR_ARRAY) {
    return PH_ERR;
  }

  if (pos == arr->u.aval.len) {
    return ph_var_array_append_claim(arr, val);
  }

  if (pos >= arr->u.aval.len) {
    return PH_NOENT;
  }

  ph_var_delref(arr->u.aval.arr[pos]);
  arr->u.aval.arr[pos] = val;
  return PH_OK;
}

ph_result_t ph_var_array_set(ph_variant_t *arr, uint32_t pos,
    ph_variant_t *val)
{
  ph_result_t res = ph_var_array_set_claim(arr, pos, val);

  if (res != PH_OK) {
    return res;
  }

  ph_var_addref(val);
  return PH_OK;
}

static bool var_copy(const void *src, void *dest)
{
  ph_variant_t *avar = *(ph_variant_t**)src;
  ph_variant_t **bvar = (ph_variant_t**)dest;

  *bvar = avar;
  ph_var_addref(avar);

  return true;
}

static void var_del(void *key)
{
  ph_variant_t **varp = (ph_variant_t**)key;

  if (*varp) {
    ph_var_delref(*varp);
    *varp = 0;
  }
}

static struct ph_ht_val_def var_val_def = {
  sizeof(ph_variant_t*),
  var_copy,
  var_del
};

ph_variant_t *ph_var_object(uint32_t nelems)
{
  ph_variant_t *var;
  ph_result_t res;

  var = ph_mem_alloc(mt.var);
  if (!var) {
    return NULL;
  }

  var->ref = 1;
  var->type = PH_VAR_OBJECT;

  res = ph_ht_init(&var->u.oval, nelems, &ph_ht_string_key_def, &var_val_def);
  if (res != PH_OK) {
    ph_mem_free(mt.var, var);
    return 0;
  }

  return var;
}

ph_result_t ph_var_object_set_claim_kv(ph_variant_t *obj,
    ph_string_t *key, ph_variant_t *val)
{
  if (obj->type != PH_VAR_OBJECT) {
    return PH_ERR;
  }

  return ph_ht_insert(&obj->u.oval, &key, &val, PH_HT_REPLACE|PH_HT_CLAIM);
}

ph_result_t ph_var_object_set(ph_variant_t *obj,
    ph_string_t *key, ph_variant_t *val)
{
  if (obj->type != PH_VAR_OBJECT) {
    return PH_ERR;
  }

  return ph_ht_insert(&obj->u.oval, &key, &val, PH_HT_REPLACE|PH_HT_COPY);
}

ph_variant_t *ph_var_object_get(ph_variant_t *obj, ph_string_t *key)
{
  ph_result_t res;
  ph_variant_t *val;

  if (obj->type != PH_VAR_OBJECT) {
    return 0;
  }

  res = ph_ht_lookup(&obj->u.oval, &key, &val, false);
  if (res != PH_OK) {
    return 0;
  }

  return val;
}

ph_variant_t *ph_var_object_get_cstr(ph_variant_t *obj, const char *key)
{
  ph_string_t kstr;
  uint32_t len = strlen(key);

  ph_string_init_claim(&kstr, PH_STRING_STATIC, (char*)key, len, len);
  return ph_var_object_get(obj, &kstr);
}

bool ph_var_object_iter_first(ph_variant_t *obj, ph_ht_iter_t *iter,
    ph_string_t **key, ph_variant_t **val)
{
  void **a, **b;

  if (!ph_ht_iter_first(&obj->u.oval, iter,
        (void**)(void*)&a, (void**)(void*)&b)) {
    return false;
  }

  if (key) {
    *key = *a;
  }
  if (val) {
    *val = *b;
  }

  return true;
}

bool ph_var_object_iter_next(ph_variant_t *obj, ph_ht_iter_t *iter,
    ph_string_t **key, ph_variant_t **val)
{
  void **a, **b;

  if (!ph_ht_iter_next(&obj->u.oval, iter,
        (void**)(void*)&a, (void**)(void*)&b)) {
    return false;
  }

  if (key) {
    *key = *a;
  }
  if (val) {
    *val = *b;
  }

  return true;
}

bool ph_var_object_ordered_iter_first(ph_variant_t *obj,
    ph_ht_ordered_iter_t *iter,
    ph_string_t **key, ph_variant_t **val)
{
  void **a, **b;

  if (!ph_ht_ordered_iter_first(&obj->u.oval, iter,
        (void**)(void*)&a, (void**)(void*)&b)) {
    return false;
  }

  if (key) {
    *key = *a;
  }
  if (val) {
    *val = *b;
  }

  return true;
}

bool ph_var_object_ordered_iter_next(ph_variant_t *obj,
    ph_ht_ordered_iter_t *iter,
    ph_string_t **key, ph_variant_t **val)
{
  void **a, **b;

  if (!ph_ht_ordered_iter_next(&obj->u.oval, iter,
        (void**)(void*)&a, (void**)(void*)&b)) {
    return false;
  }

  if (key) {
    *key = *a;
  }
  if (val) {
    *val = *b;
  }

  return true;
}

void ph_var_object_ordered_iter_end(ph_variant_t *obj,
    ph_ht_ordered_iter_t *iter)
{
  ph_ht_ordered_iter_end(&obj->u.oval, iter);
}

ph_result_t ph_var_object_del(ph_variant_t *obj, ph_string_t *key)
{
  if (obj->type != PH_VAR_OBJECT) {
    return PH_ERR;
  }
  return ph_ht_del(&obj->u.oval, &key);
}

uint32_t ph_var_object_size(ph_variant_t *var)
{
  if (var->type != PH_VAR_OBJECT) {
    return 0;
  }
  return ph_ht_size(&var->u.oval);
}

static bool obj_equal(ph_variant_t *a, ph_variant_t *b)
{
  ph_string_t *key;
  ph_variant_t *v1, *v2;
  ph_ht_iter_t iter;

  if (ph_var_object_size(a) != ph_var_object_size(b)) {
    return false;
  }
  if (ph_var_object_size(a) == 0) {
    return true;
  }

  if (ph_var_object_iter_first(a, &iter, &key, &v1)) do {
    v2 = ph_var_object_get(b, key);
    if (!v2) {
      return false;
    }
    if (!ph_var_equal(v1, v2)) {
      return false;
    }
  } while (ph_var_object_iter_next(a, &iter, &key, &v1));

  return true;
}

static bool arr_equal(ph_variant_t *a, ph_variant_t *b)
{
  uint32_t i, n;

  n = ph_var_array_size(a);
  if (ph_var_array_size(b) != n) {
    return false;
  }

  for (i = 0; i < n; i++) {
    if (!ph_var_equal(ph_var_array_get(a, i), ph_var_array_get(b, i))) {
      return false;
    }
  }

  return true;
}

bool ph_var_equal(ph_variant_t *a, ph_variant_t *b)
{
  if (a == b) {
    return true;
  }
  if (a->type != b->type) {
    return false;
  }

  switch (a->type) {
    case PH_VAR_INTEGER:
      return a->u.ival == b->u.ival;
    case PH_VAR_REAL:
      return a->u.dval == b->u.dval;
    case PH_VAR_STRING:
      return ph_string_equal(a->u.sval, b->u.sval);
    case PH_VAR_OBJECT:
      return obj_equal(a, b);
    case PH_VAR_ARRAY:
      return arr_equal(a, b);
    default:
      return false;
  }
}

/* vim:ts=2:sw=2:et:
 */

