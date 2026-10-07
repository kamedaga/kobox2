/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_BLOCK_DEVICE_H
#define KOBOX2_BLOCK_DEVICE_H

#include <kobox2/block_device_layout.h>
#include <stddef.h>
#include <stdint.h>

/* Decoded values only. A shared-memory buffer always contains the explicit
 * little-endian representation produced by these codecs. */
typedef struct kb2_block_header {
    uint32_t opcode;
    uint32_t status;
    uint32_t count;
    uint64_t generation;
    uint64_t sequence;
    uint64_t diskseq;
    uint64_t offset;
    uint32_t major;
    uint32_t minor;
    uint32_t length;
} kb2_block_header_t;

typedef struct kb2_block_disk {
    uint64_t diskseq;
    uint64_t bytes;
    uint32_t major;
    uint32_t minor;
    uint32_t logical_block_size;
    uint32_t physical_block_size;
    uint32_t removable;
    uint32_t read_only;
    char name[32];
} kb2_block_disk_t;

int kb2_block_header_encode(uint8_t *bytes, size_t capacity,
                            const kb2_block_header_t *header);
int kb2_block_header_decode(const uint8_t *bytes, size_t size,
                            kb2_block_header_t *header);
int kb2_block_disk_encode(uint8_t *bytes, size_t capacity,
                          const kb2_block_disk_t *disk);
int kb2_block_disk_decode(const uint8_t *bytes, size_t size,
                          kb2_block_disk_t *disk);

#endif
