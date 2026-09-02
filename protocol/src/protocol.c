/* SPDX-License-Identifier: MIT */

#include <kobox2/protocol.h>

#include <limits.h>
#include <string.h>

static const uint8_t kb2_protocol_abi_identity[KB2_PROTOCOL_ABI_IDENTITY_SIZE] =
    KB2_PROTOCOL_ABI_IDENTITY_BYTES;
static const uint8_t kb2_protocol_schema_digest[KB2_PROTOCOL_SCHEMA_DIGEST_SIZE] =
    KB2_PROTOCOL_SCHEMA_SHA256_BYTES;

static uint32_t kb2_load_u32(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) | ((uint32_t)source[3] << 24u);
}

static uint64_t kb2_load_u64(const uint8_t *source) {
    return (uint64_t)kb2_load_u32(source) | ((uint64_t)kb2_load_u32(source + 4) << 32u);
}

static void kb2_store_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static void kb2_store_u64(uint8_t *destination, uint64_t value) {
    kb2_store_u32(destination, (uint32_t)value);
    kb2_store_u32(destination + 4, (uint32_t)(value >> 32u));
}

static int kb2_size_add_multiply(size_t base, size_t count, size_t width, size_t *result_out) {
    if (count != 0 && width > (SIZE_MAX - base) / count) {
        return 0;
    }
    *result_out = base + count * width;
    return 1;
}

static int kb2_u64_range_valid(uint64_t base, uint64_t length) {
    return length != 0 && base <= UINT64_MAX - length;
}

static int kb2_ranges_overlap(uint64_t left_base,
                              uint64_t left_length,
                              uint64_t right_base,
                              uint64_t right_length) {
    return left_base < right_base + right_length && right_base < left_base + left_length;
}

static int kb2_range_has_rights(uint64_t base,
                                uint64_t length,
                                uint32_t required_rights,
                                const kb2_protocol_region_t *regions,
                                size_t region_count) {
    size_t index;

    if (!kb2_u64_range_valid(base, length)) {
        return 0;
    }
    for (index = 0; index < region_count; ++index) {
        const kb2_protocol_region_t *region = &regions[index];

        if ((region->rights & required_rights) == required_rights &&
            base >= region->transport_base &&
            base + length <= region->transport_base + region->length) {
            return 1;
        }
    }
    return 0;
}

static uint64_t kb2_queue_range_length(const kb2_protocol_queue_t *queue, size_t range_index) {
    switch (range_index) {
    case 0:
        return (uint64_t)queue->queue_size * 16u;
    case 1:
        return 6u + (uint64_t)queue->queue_size * 2u;
    case 2:
        return 6u + (uint64_t)queue->queue_size * 8u;
    default:
        return 0;
    }
}

static uint64_t kb2_queue_range_base(const kb2_protocol_queue_t *queue, size_t range_index) {
    switch (range_index) {
    case 0:
        return queue->descriptor_address;
    case 1:
        return queue->available_address;
    case 2:
        return queue->used_address;
    default:
        return 0;
    }
}

static int kb2_validate_regions(const kb2_protocol_region_t *regions, size_t region_count) {
    const uint32_t known_rights =
        KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE;
    size_t left;
    size_t right;

    for (left = 0; left < region_count; ++left) {
        const kb2_protocol_region_t *region = &regions[left];

        if (region->region_id == 0 || region->rights == 0 ||
            (region->rights & ~known_rights) != 0 ||
            !kb2_u64_range_valid(region->transport_base, region->length)) {
            return 0;
        }
        for (right = 0; right < left; ++right) {
            if (region->region_id == regions[right].region_id ||
                kb2_ranges_overlap(region->transport_base,
                                   region->length,
                                   regions[right].transport_base,
                                   regions[right].length)) {
                return 0;
            }
        }
    }
    return 1;
}

static int kb2_queue_identity_unique(const kb2_protocol_queue_t *queues, size_t index) {
    size_t previous;

    for (previous = 0; previous < index; ++previous) {
        if (queues[index].queue_id == queues[previous].queue_id ||
            queues[index].available_notification_id ==
                queues[previous].available_notification_id ||
            queues[index].available_notification_id == queues[previous].used_notification_id ||
            queues[index].used_notification_id ==
                queues[previous].available_notification_id ||
            queues[index].used_notification_id == queues[previous].used_notification_id) {
            return 0;
        }
    }
    return queues[index].available_notification_id != queues[index].used_notification_id;
}

static int kb2_queue_ranges_unique(const kb2_protocol_queue_t *queues, size_t index) {
    size_t current_range;
    size_t previous;
    size_t previous_range;

    for (current_range = 0; current_range < 3; ++current_range) {
        uint64_t current_base = kb2_queue_range_base(&queues[index], current_range);
        uint64_t current_length = kb2_queue_range_length(&queues[index], current_range);

        for (previous = 0; previous <= index; ++previous) {
            size_t range_limit = previous == index ? current_range : 3;

            for (previous_range = 0; previous_range < range_limit; ++previous_range) {
                if (kb2_ranges_overlap(current_base,
                                       current_length,
                                       kb2_queue_range_base(&queues[previous], previous_range),
                                       kb2_queue_range_length(&queues[previous], previous_range))) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int kb2_validate_queues(const kb2_protocol_queue_t *queues,
                               size_t queue_count,
                               const kb2_protocol_region_t *regions,
                               size_t region_count) {
    const uint32_t ring_rights =
        KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE;
    size_t event_count = 0;
    size_t index;

    for (index = 0; index < queue_count; ++index) {
        const kb2_protocol_queue_t *queue = &queues[index];
        uint64_t descriptor_length;
        uint64_t available_length;
        uint64_t used_length;

        if (queue->queue_id == 0 || queue->flags != 0 ||
            (queue->role != KB2_PROTOCOL_QUEUE_ROLE_EVENT &&
             queue->role != KB2_PROTOCOL_QUEUE_ROLE_REQUEST) ||
            queue->queue_size < KB2_PROTOCOL_QUEUE_SIZE_MIN ||
            queue->queue_size > KB2_PROTOCOL_QUEUE_SIZE_MAX ||
            (queue->queue_size & (queue->queue_size - 1u)) != 0 ||
            queue->available_notification_id == 0 || queue->used_notification_id == 0 ||
            queue->max_chain_length == 0 || queue->max_chain_length > queue->queue_size ||
            queue->max_indirect_length == 0 ||
            queue->max_indirect_length > KB2_PROTOCOL_QUEUE_SIZE_MAX ||
            queue->max_outstanding == 0 || queue->max_outstanding > queue->queue_size ||
            !kb2_queue_identity_unique(queues, index)) {
            return 0;
        }
        if (queue->role == KB2_PROTOCOL_QUEUE_ROLE_EVENT) {
            ++event_count;
        }

        descriptor_length = kb2_queue_range_length(queue, 0);
        available_length = kb2_queue_range_length(queue, 1);
        used_length = kb2_queue_range_length(queue, 2);
        if ((queue->descriptor_address & 15u) != 0 || (queue->available_address & 1u) != 0 ||
            (queue->used_address & 3u) != 0 ||
            !kb2_range_has_rights(queue->descriptor_address,
                                  descriptor_length,
                                  ring_rights,
                                  regions,
                                  region_count) ||
            !kb2_range_has_rights(queue->available_address,
                                  available_length,
                                  ring_rights,
                                  regions,
                                  region_count) ||
            !kb2_range_has_rights(
                queue->used_address, used_length, ring_rights, regions, region_count) ||
            !kb2_queue_ranges_unique(queues, index)) {
            return 0;
        }
    }
    return event_count == 1;
}

static int kb2_validate_channel_model(const kb2_protocol_channel_t *channel,
                                      const kb2_protocol_queue_t *queues,
                                      size_t queue_count,
                                      const kb2_protocol_region_t *regions,
                                      size_t region_count) {
    if (channel == NULL || queues == NULL || regions == NULL || queue_count < 2 ||
        queue_count > KB2_PROTOCOL_MAX_QUEUES || region_count == 0 ||
        region_count > KB2_PROTOCOL_MAX_REGIONS ||
        channel->feature_bits != KB2_PROTOCOL_TRANSPORT_FEATURES_REQUIRED ||
        channel->channel_id == 0 || channel->generation == 0 || channel->protocol_id == 0 ||
        (channel->flags != KB2_PROTOCOL_CHANNEL_FLAG_MANAGEMENT &&
         channel->flags != KB2_PROTOCOL_CHANNEL_FLAG_DATA)) {
        return 0;
    }
    return kb2_validate_regions(regions, region_count) &&
           kb2_validate_queues(queues, queue_count, regions, region_count);
}

const char *kb2_protocol_schema_sha256_hex(void) {
    return KB2_PROTOCOL_SCHEMA_SHA256_HEX;
}

kb2_protocol_status_t kb2_protocol_copy_schema_digest(uint8_t *digest_out, size_t digest_size) {
    if (digest_out == NULL || digest_size != sizeof(kb2_protocol_schema_digest)) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    memcpy(digest_out, kb2_protocol_schema_digest, sizeof(kb2_protocol_schema_digest));
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_protocol_channel_encoded_size(size_t queue_count,
                                                        size_t region_count,
                                                        size_t *size_out) {
    size_t size;

    if (size_out == NULL || queue_count < 2 || queue_count > KB2_PROTOCOL_MAX_QUEUES ||
        region_count == 0 || region_count > KB2_PROTOCOL_MAX_REGIONS) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (!kb2_size_add_multiply(
            KB2_PROTOCOL_CHANNEL_HEADER_SIZE,
            queue_count,
            KB2_PROTOCOL_QUEUE_DESCRIPTOR_SIZE,
            &size) ||
        !kb2_size_add_multiply(size,
                               region_count,
                               KB2_PROTOCOL_REGION_DESCRIPTOR_SIZE,
                               &size)) {
        return KB2_PROTOCOL_OVERFLOW;
    }
    if (size > UINT32_MAX) {
        return KB2_PROTOCOL_OVERFLOW;
    }
    *size_out = size;
    return KB2_PROTOCOL_OK;
}

static void kb2_encode_queue(uint8_t *destination, const kb2_protocol_queue_t *queue) {
    kb2_store_u32(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_QUEUE_ID_OFFSET, queue->queue_id);
    kb2_store_u32(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_ROLE_OFFSET, queue->role);
    kb2_store_u32(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_QUEUE_SIZE_OFFSET,
                  queue->queue_size);
    kb2_store_u32(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_FLAGS_OFFSET, queue->flags);
    kb2_store_u64(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_DESCRIPTOR_ADDRESS_OFFSET,
                  queue->descriptor_address);
    kb2_store_u64(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_AVAILABLE_ADDRESS_OFFSET,
                  queue->available_address);
    kb2_store_u64(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_USED_ADDRESS_OFFSET,
                  queue->used_address);
    kb2_store_u32(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_AVAILABLE_NOTIFICATION_ID_OFFSET,
                  queue->available_notification_id);
    kb2_store_u32(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_USED_NOTIFICATION_ID_OFFSET,
                  queue->used_notification_id);
    kb2_store_u32(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_MAX_CHAIN_LENGTH_OFFSET,
                  queue->max_chain_length);
    kb2_store_u32(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_MAX_INDIRECT_LENGTH_OFFSET,
                  queue->max_indirect_length);
    kb2_store_u32(destination + KB2_PROTOCOL_QUEUE_DESCRIPTOR_MAX_OUTSTANDING_OFFSET,
                  queue->max_outstanding);
}

static void kb2_encode_region(uint8_t *destination, const kb2_protocol_region_t *region) {
    kb2_store_u32(destination + KB2_PROTOCOL_REGION_DESCRIPTOR_REGION_ID_OFFSET,
                  region->region_id);
    kb2_store_u32(destination + KB2_PROTOCOL_REGION_DESCRIPTOR_RIGHTS_OFFSET, region->rights);
    kb2_store_u64(destination + KB2_PROTOCOL_REGION_DESCRIPTOR_TRANSPORT_BASE_OFFSET,
                  region->transport_base);
    kb2_store_u64(destination + KB2_PROTOCOL_REGION_DESCRIPTOR_LENGTH_OFFSET, region->length);
}

kb2_protocol_status_t kb2_protocol_channel_encode(uint8_t *buffer,
                                                  size_t buffer_size,
                                                  size_t *encoded_size_out,
                                                  const kb2_protocol_channel_t *channel,
                                                  const kb2_protocol_queue_t *queues,
                                                  size_t queue_count,
                                                  const kb2_protocol_region_t *regions,
                                                  size_t region_count) {
    kb2_protocol_status_t status;
    size_t encoded_size;
    size_t queue_table_offset = KB2_PROTOCOL_CHANNEL_HEADER_SIZE;
    size_t region_table_offset;
    size_t index;

    if (buffer == NULL || encoded_size_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    status = kb2_protocol_channel_encoded_size(queue_count, region_count, &encoded_size);
    if (status != KB2_PROTOCOL_OK) {
        return status;
    }
    *encoded_size_out = encoded_size;
    if (buffer_size < encoded_size) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }
    if (!kb2_validate_channel_model(channel, queues, queue_count, regions, region_count)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    region_table_offset =
        queue_table_offset + queue_count * KB2_PROTOCOL_QUEUE_DESCRIPTOR_SIZE;

    memset(buffer, 0, encoded_size);
    kb2_store_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_TOTAL_SIZE_OFFSET,
                  (uint32_t)encoded_size);
    memcpy(buffer + KB2_PROTOCOL_CHANNEL_HEADER_ABI_IDENTITY_OFFSET,
           kb2_protocol_abi_identity,
           sizeof(kb2_protocol_abi_identity));
    memcpy(buffer + KB2_PROTOCOL_CHANNEL_HEADER_SCHEMA_DIGEST_OFFSET,
           kb2_protocol_schema_digest,
           sizeof(kb2_protocol_schema_digest));
    kb2_store_u64(buffer + KB2_PROTOCOL_CHANNEL_HEADER_FEATURE_BITS_OFFSET,
                  channel->feature_bits);
    kb2_store_u64(buffer + KB2_PROTOCOL_CHANNEL_HEADER_CHANNEL_ID_OFFSET, channel->channel_id);
    kb2_store_u64(buffer + KB2_PROTOCOL_CHANNEL_HEADER_GENERATION_OFFSET, channel->generation);
    kb2_store_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_PROTOCOL_ID_OFFSET, channel->protocol_id);
    kb2_store_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_FLAGS_OFFSET, channel->flags);
    kb2_store_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_QUEUE_COUNT_OFFSET, (uint32_t)queue_count);
    kb2_store_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_REGION_COUNT_OFFSET,
                  (uint32_t)region_count);
    kb2_store_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_QUEUE_TABLE_OFFSET_OFFSET,
                  (uint32_t)queue_table_offset);
    kb2_store_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_REGION_TABLE_OFFSET_OFFSET,
                  (uint32_t)region_table_offset);

    for (index = 0; index < queue_count; ++index) {
        kb2_encode_queue(buffer + queue_table_offset + index * KB2_PROTOCOL_QUEUE_DESCRIPTOR_SIZE,
                         &queues[index]);
    }
    for (index = 0; index < region_count; ++index) {
        kb2_encode_region(
            buffer + region_table_offset + index * KB2_PROTOCOL_REGION_DESCRIPTOR_SIZE,
            &regions[index]);
    }
    return KB2_PROTOCOL_OK;
}

static void kb2_decode_queue(const uint8_t *source, kb2_protocol_queue_t *queue) {
    queue->queue_id = kb2_load_u32(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_QUEUE_ID_OFFSET);
    queue->role = kb2_load_u32(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_ROLE_OFFSET);
    queue->queue_size =
        kb2_load_u32(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_QUEUE_SIZE_OFFSET);
    queue->flags = kb2_load_u32(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_FLAGS_OFFSET);
    queue->descriptor_address =
        kb2_load_u64(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_DESCRIPTOR_ADDRESS_OFFSET);
    queue->available_address =
        kb2_load_u64(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_AVAILABLE_ADDRESS_OFFSET);
    queue->used_address =
        kb2_load_u64(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_USED_ADDRESS_OFFSET);
    queue->available_notification_id =
        kb2_load_u32(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_AVAILABLE_NOTIFICATION_ID_OFFSET);
    queue->used_notification_id =
        kb2_load_u32(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_USED_NOTIFICATION_ID_OFFSET);
    queue->max_chain_length =
        kb2_load_u32(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_MAX_CHAIN_LENGTH_OFFSET);
    queue->max_indirect_length =
        kb2_load_u32(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_MAX_INDIRECT_LENGTH_OFFSET);
    queue->max_outstanding =
        kb2_load_u32(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_MAX_OUTSTANDING_OFFSET);
}

static void kb2_decode_region(const uint8_t *source, kb2_protocol_region_t *region) {
    region->region_id = kb2_load_u32(source + KB2_PROTOCOL_REGION_DESCRIPTOR_REGION_ID_OFFSET);
    region->rights = kb2_load_u32(source + KB2_PROTOCOL_REGION_DESCRIPTOR_RIGHTS_OFFSET);
    region->transport_base =
        kb2_load_u64(source + KB2_PROTOCOL_REGION_DESCRIPTOR_TRANSPORT_BASE_OFFSET);
    region->length = kb2_load_u64(source + KB2_PROTOCOL_REGION_DESCRIPTOR_LENGTH_OFFSET);
}

kb2_protocol_status_t kb2_protocol_channel_decode(const uint8_t *buffer,
                                                  size_t buffer_size,
                                                  kb2_protocol_channel_t *channel_out,
                                                  kb2_protocol_queue_t *queues_out,
                                                  size_t queue_capacity,
                                                  size_t *queue_count_out,
                                                  kb2_protocol_region_t *regions_out,
                                                  size_t region_capacity,
                                                  size_t *region_count_out) {
    size_t expected_size;
    size_t queue_count;
    size_t region_count;
    size_t queue_table_offset;
    size_t region_table_offset;
    size_t index;

    if (buffer == NULL || channel_out == NULL || queues_out == NULL || queue_count_out == NULL ||
        regions_out == NULL || region_count_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (buffer_size < KB2_PROTOCOL_CHANNEL_HEADER_SIZE) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }
    if (memcmp(buffer + KB2_PROTOCOL_CHANNEL_HEADER_ABI_IDENTITY_OFFSET,
               kb2_protocol_abi_identity,
               sizeof(kb2_protocol_abi_identity)) != 0 ||
        memcmp(buffer + KB2_PROTOCOL_CHANNEL_HEADER_SCHEMA_DIGEST_OFFSET,
               kb2_protocol_schema_digest,
               sizeof(kb2_protocol_schema_digest)) != 0) {
        return KB2_PROTOCOL_SCHEMA_MISMATCH;
    }
    if (kb2_load_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_RESERVED_0_OFFSET) != 0 ||
        kb2_load_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_RESERVED_1_OFFSET) != 0) {
        return KB2_PROTOCOL_MALFORMED;
    }

    queue_count = kb2_load_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_QUEUE_COUNT_OFFSET);
    region_count = kb2_load_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_REGION_COUNT_OFFSET);
    *queue_count_out = queue_count;
    *region_count_out = region_count;
    if (kb2_protocol_channel_encoded_size(queue_count, region_count, &expected_size) !=
        KB2_PROTOCOL_OK) {
        return KB2_PROTOCOL_MALFORMED;
    }
    queue_table_offset =
        kb2_load_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_QUEUE_TABLE_OFFSET_OFFSET);
    region_table_offset =
        kb2_load_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_REGION_TABLE_OFFSET_OFFSET);
    if (kb2_load_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_TOTAL_SIZE_OFFSET) != expected_size ||
        queue_table_offset != KB2_PROTOCOL_CHANNEL_HEADER_SIZE ||
        region_table_offset !=
            queue_table_offset + queue_count * KB2_PROTOCOL_QUEUE_DESCRIPTOR_SIZE) {
        return KB2_PROTOCOL_MALFORMED;
    }
    if (buffer_size < expected_size || queue_capacity < queue_count ||
        region_capacity < region_count) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }

    channel_out->feature_bits =
        kb2_load_u64(buffer + KB2_PROTOCOL_CHANNEL_HEADER_FEATURE_BITS_OFFSET);
    channel_out->channel_id =
        kb2_load_u64(buffer + KB2_PROTOCOL_CHANNEL_HEADER_CHANNEL_ID_OFFSET);
    channel_out->generation =
        kb2_load_u64(buffer + KB2_PROTOCOL_CHANNEL_HEADER_GENERATION_OFFSET);
    channel_out->protocol_id =
        kb2_load_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_PROTOCOL_ID_OFFSET);
    channel_out->flags = kb2_load_u32(buffer + KB2_PROTOCOL_CHANNEL_HEADER_FLAGS_OFFSET);

    for (index = 0; index < queue_count; ++index) {
        const uint8_t *source =
            buffer + queue_table_offset + index * KB2_PROTOCOL_QUEUE_DESCRIPTOR_SIZE;

        if (kb2_load_u32(source + KB2_PROTOCOL_QUEUE_DESCRIPTOR_RESERVED_OFFSET) != 0) {
            return KB2_PROTOCOL_MALFORMED;
        }
        kb2_decode_queue(source, &queues_out[index]);
    }
    for (index = 0; index < region_count; ++index) {
        const uint8_t *source =
            buffer + region_table_offset + index * KB2_PROTOCOL_REGION_DESCRIPTOR_SIZE;

        if (kb2_load_u64(source + KB2_PROTOCOL_REGION_DESCRIPTOR_RESERVED_OFFSET) != 0) {
            return KB2_PROTOCOL_MALFORMED;
        }
        kb2_decode_region(source, &regions_out[index]);
    }
    if (!kb2_validate_channel_model(
            channel_out, queues_out, queue_count, regions_out, region_count)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t
kb2_protocol_message_envelope_encode(uint8_t *buffer,
                                     size_t buffer_size,
                                     const kb2_protocol_message_envelope_t *envelope) {
    size_t total_size;

    if (buffer == NULL || envelope == NULL || envelope->protocol_id == 0 ||
        envelope->generation == 0) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (!kb2_size_add_multiply(KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                               envelope->payload_length,
                               1,
                               &total_size)) {
        return KB2_PROTOCOL_OVERFLOW;
    }
    if (buffer_size < total_size) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }

    memset(buffer, 0, KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE);
    kb2_store_u32(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_PROTOCOL_ID_OFFSET,
                  envelope->protocol_id);
    memcpy(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_ABI_IDENTITY_OFFSET,
           kb2_protocol_abi_identity,
           sizeof(kb2_protocol_abi_identity));
    kb2_store_u32(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_OPCODE_OFFSET, envelope->opcode);
    kb2_store_u32(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_FLAGS_OFFSET, envelope->flags);
    kb2_store_u64(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_GENERATION_OFFSET,
                  envelope->generation);
    kb2_store_u64(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_CORRELATION_ID_OFFSET,
                  envelope->correlation_id);
    kb2_store_u32(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_PAYLOAD_LENGTH_OFFSET,
                  envelope->payload_length);
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t
kb2_protocol_message_envelope_decode(const uint8_t *buffer,
                                     size_t buffer_size,
                                     kb2_protocol_message_envelope_t *envelope_out) {
    size_t total_size;

    if (buffer == NULL || envelope_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (buffer_size < KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }
    if (memcmp(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_ABI_IDENTITY_OFFSET,
               kb2_protocol_abi_identity,
               sizeof(kb2_protocol_abi_identity)) != 0) {
        return KB2_PROTOCOL_SCHEMA_MISMATCH;
    }
    if (kb2_load_u32(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_RESERVED_OFFSET) != 0) {
        return KB2_PROTOCOL_MALFORMED;
    }

    envelope_out->protocol_id =
        kb2_load_u32(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_PROTOCOL_ID_OFFSET);
    envelope_out->opcode = kb2_load_u32(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_OPCODE_OFFSET);
    envelope_out->flags = kb2_load_u32(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_FLAGS_OFFSET);
    envelope_out->generation =
        kb2_load_u64(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_GENERATION_OFFSET);
    envelope_out->correlation_id =
        kb2_load_u64(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_CORRELATION_ID_OFFSET);
    envelope_out->payload_length =
        kb2_load_u32(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_PAYLOAD_LENGTH_OFFSET);
    if (envelope_out->protocol_id == 0 || envelope_out->generation == 0) {
        return KB2_PROTOCOL_MALFORMED;
    }
    if (!kb2_size_add_multiply(KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE,
                               envelope_out->payload_length,
                               1,
                               &total_size)) {
        return KB2_PROTOCOL_OVERFLOW;
    }
    if (buffer_size < total_size) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }
    return KB2_PROTOCOL_OK;
}
