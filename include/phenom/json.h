
#ifndef PHENOM_JSON_H
#define PHENOM_JSON_H

#include "phenom/defs.h"
#include "phenom/stream.h"
#include "phenom/variant.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PH_JSON_ERROR_TEXT_LENGTH    160
#define PH_JSON_ERROR_SOURCE_LENGTH   80

#define PH_JSON_REJECT_DUPLICATES 0x1
#define PH_JSON_DISABLE_EOF_CHECK 0x2
#define PH_JSON_DECODE_ANY        0x4

struct ph_json_err {
  uint32_t line, column, position;
  char text[PH_JSON_ERROR_TEXT_LENGTH];
  char source[PH_JSON_ERROR_SOURCE_LENGTH];
};
typedef struct ph_json_err ph_json_err_t;

/** Parse JSON from a stream
 *
 * Attempt to parse and decode a JSON encoded data stream.
 * Returns a variant instance if successful.
 * On failure, updates the provided `err` pointer to contain
 * some context on the failure.
 */
ph_variant_t *ph_json_load_stream(ph_stream_t *stm, uint32_t flags,
    ph_json_err_t *err);

/** Parse JSON from a string
 *
 * Attempt to parse and decode a JSON encoded string.
 * Returns a variant instance if successful.
 * On failure, updates the provided `err` pointer to contain
 * some context on the failure.
 */
ph_variant_t *ph_json_load_string(ph_string_t *str, uint32_t flags,
    ph_json_err_t *err);

/** Parse JSON from a C-string
 */
ph_variant_t *ph_json_load_cstr(const char *cstr, uint32_t flags,
    ph_json_err_t *err);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

