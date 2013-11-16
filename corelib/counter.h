// Copyright 2004-present Facebook. All Rights Reserved

#ifndef CORELIB_COUNTER_H
#define CORELIB_COUNTER_H

struct ph_counter_scope {
  ph_refcnt_t refcnt;
  uint32_t scope_id;
  unsigned long hash; // NOLINT(runtime/int)

  uint8_t num_slots, next_slot;
  /* points to just after slot_names */
  char *scope_name;
  char *full_scope_name;

  /* variable size array; the remainder of this struct
   * holds num_slots elements */
  char *slot_names[1];
};


#endif

/* vim:ts=2:sw=2:et:
 */

