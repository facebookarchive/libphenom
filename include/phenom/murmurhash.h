/*
 * Murmur Hash is in the public domain. This version from Peter Scott,
 * translated from the original version from Austin Appleby.
 */

#ifndef MURMURHASH_H
#define MURMURHASH_H

#include <stdint.h>
#include <ck_cc.h>

#ifdef __cplusplus
extern "C" {
#endif

void MurmurHash3_x64_128(const void *key, const int len, const uint32_t seed, void *out);

#ifdef __cplusplus
}
#endif

#endif

/* vim:ts=2:sw=2:et:
 */

