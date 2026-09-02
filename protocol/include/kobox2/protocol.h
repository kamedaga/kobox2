/* SPDX-License-Identifier: MIT */

#ifndef KOBOX2_PROTOCOL_H
#define KOBOX2_PROTOCOL_H

#include <kobox2/protocol_layout.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum kb2_protocol_status {
    KB2_PROTOCOL_OK = 0,
    KB2_PROTOCOL_INVALID_ARGUMENT,
    KB2_PROTOCOL_BUFFER_TOO_SMALL,
    KB2_PROTOCOL_OVERFLOW,
    KB2_PROTOCOL_MALFORMED,
    KB2_PROTOCOL_SCHEMA_MISMATCH,
} kb2_protocol_status_t;

/* Host-native decoded values. These structures are never mapped onto wire bytes. */
typedef struct kb2_protocol_channel {
    uint64_t feature_bits;
    uint64_t channel_id;
    uint64_t generation;
    uint32_t protocol_id;
    uint32_t flags;
} kb2_protocol_channel_t;

typedef struct kb2_protocol_queue {
    uint32_t queue_id;
    uint32_t role;
    uint32_t queue_size;
    uint32_t flags;
    uint64_t descriptor_address;
    uint64_t available_address;
    uint64_t used_address;
    uint32_t available_notification_id;
    uint32_t used_notification_id;
    uint32_t max_chain_length;
    uint32_t max_indirect_length;
    uint32_t max_outstanding;
} kb2_protocol_queue_t;

typedef struct kb2_protocol_region {
    uint32_t region_id;
    uint32_t rights;
    uint64_t transport_base;
    uint64_t length;
} kb2_protocol_region_t;

typedef struct kb2_protocol_message_envelope {
    uint32_t protocol_id;
    uint32_t opcode;
    uint32_t flags;
    uint64_t generation;
    uint64_t correlation_id;
    uint32_t payload_length;
} kb2_protocol_message_envelope_t;

const char *kb2_protocol_schema_sha256_hex(void);
kb2_protocol_status_t kb2_protocol_copy_schema_digest(uint8_t *digest_out, size_t digest_size);

kb2_protocol_status_t kb2_protocol_channel_encoded_size(size_t queue_count,
                                                        size_t region_count,
                                                        size_t *size_out);
kb2_protocol_status_t kb2_protocol_channel_encode(uint8_t *buffer,
                                                  size_t buffer_size,
                                                  size_t *encoded_size_out,
                                                  const kb2_protocol_channel_t *channel,
                                                  const kb2_protocol_queue_t *queues,
                                                  size_t queue_count,
                                                  const kb2_protocol_region_t *regions,
                                                  size_t region_count);
/* Decode outputs contain complete values only when KB2_PROTOCOL_OK is returned. */
kb2_protocol_status_t kb2_protocol_channel_decode(const uint8_t *buffer,
                                                  size_t buffer_size,
                                                  kb2_protocol_channel_t *channel_out,
                                                  kb2_protocol_queue_t *queues_out,
                                                  size_t queue_capacity,
                                                  size_t *queue_count_out,
                                                  kb2_protocol_region_t *regions_out,
                                                  size_t region_capacity,
                                                  size_t *region_count_out);

/* Envelope encoding writes only the fixed header and validates room for the payload. */
kb2_protocol_status_t
kb2_protocol_message_envelope_encode(uint8_t *buffer,
                                     size_t buffer_size,
                                     const kb2_protocol_message_envelope_t *envelope);
kb2_protocol_status_t
kb2_protocol_message_envelope_decode(const uint8_t *buffer,
                                     size_t buffer_size,
                                     kb2_protocol_message_envelope_t *envelope_out);

#ifdef __cplusplus
}
#endif

#endif
