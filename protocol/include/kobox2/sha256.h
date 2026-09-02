/* SPDX-License-Identifier: MIT */

#ifndef KOBOX2_SHA256_H
#define KOBOX2_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KB2_SHA256_DIGEST_SIZE 32u

typedef struct kb2_sha256_context {
    uint32_t state[8];
    uint64_t byte_count;
    uint8_t block[64];
    size_t block_size;
} kb2_sha256_context_t;

void kb2_sha256_initialize(kb2_sha256_context_t *context);
void kb2_sha256_update(kb2_sha256_context_t *context, const void *data, size_t size);
void kb2_sha256_finish(kb2_sha256_context_t *context,
                       uint8_t digest[KB2_SHA256_DIGEST_SIZE]);
void kb2_sha256(const void *data, size_t size, uint8_t digest[KB2_SHA256_DIGEST_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
