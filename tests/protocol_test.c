/* SPDX-License-Identifier: Apache-2.0 */

#include <kobox2/protocol.h>

#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static void make_channel(kb2_protocol_channel_t *channel,
                         kb2_protocol_queue_t queues[2],
                         kb2_protocol_region_t *region) {
    memset(channel, 0, sizeof(*channel));
    memset(queues, 0, sizeof(*queues) * 2u);
    memset(region, 0, sizeof(*region));

    channel->feature_bits = KB2_PROTOCOL_TRANSPORT_FEATURES_REQUIRED;
    channel->channel_id = 5;
    channel->generation = 9;
    channel->protocol_id = 1;
    channel->flags = KB2_PROTOCOL_CHANNEL_FLAG_MANAGEMENT;

    queues[0].queue_id = 1;
    queues[0].role = KB2_PROTOCOL_QUEUE_ROLE_EVENT;
    queues[0].queue_size = 16;
    queues[0].descriptor_address = 0x100000;
    queues[0].available_address = 0x100100;
    queues[0].used_address = 0x100128;
    queues[0].available_notification_id = 1;
    queues[0].used_notification_id = 2;
    queues[0].max_chain_length = 16;
    queues[0].max_indirect_length = 128;
    queues[0].max_outstanding = 16;

    queues[1].queue_id = 2;
    queues[1].role = KB2_PROTOCOL_QUEUE_ROLE_REQUEST;
    queues[1].queue_size = 16;
    queues[1].descriptor_address = 0x100200;
    queues[1].available_address = 0x100300;
    queues[1].used_address = 0x100328;
    queues[1].available_notification_id = 3;
    queues[1].used_notification_id = 4;
    queues[1].max_chain_length = 16;
    queues[1].max_indirect_length = 128;
    queues[1].max_outstanding = 16;

    region->region_id = 1;
    region->rights = KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE;
    region->transport_base = 0x100000;
    region->length = 0x1000;
}

static int test_channel_round_trip(void) {
    uint8_t buffer[512];
    uint8_t digest[32];
    kb2_protocol_channel_t channel;
    kb2_protocol_channel_t decoded_channel;
    kb2_protocol_queue_t queues[2];
    kb2_protocol_queue_t decoded_queues[2];
    kb2_protocol_region_t region;
    kb2_protocol_region_t decoded_region;
    size_t encoded_size;
    size_t queue_count;
    size_t region_count;

    make_channel(&channel, queues, &region);
    CHECK(kb2_protocol_channel_encoded_size(2, 1, &encoded_size) == KB2_PROTOCOL_OK);
    CHECK(encoded_size == 256);
    CHECK(kb2_protocol_channel_encode(
              buffer, sizeof(buffer), &encoded_size, &channel, queues, 2, &region, 1) ==
          KB2_PROTOCOL_OK);
    CHECK(kb2_protocol_channel_decode(buffer,
                                      encoded_size,
                                      &decoded_channel,
                                      decoded_queues,
                                      2,
                                      &queue_count,
                                      &decoded_region,
                                      1,
                                      &region_count) == KB2_PROTOCOL_OK);
    CHECK(queue_count == 2 && region_count == 1);
    CHECK(decoded_channel.channel_id == channel.channel_id);
    CHECK(decoded_channel.generation == channel.generation);
    CHECK(decoded_queues[1].role == KB2_PROTOCOL_QUEUE_ROLE_REQUEST);
    CHECK(decoded_queues[1].used_address == queues[1].used_address);
    CHECK(decoded_region.transport_base == region.transport_base);
    CHECK(kb2_protocol_copy_schema_digest(digest, sizeof(digest)) == KB2_PROTOCOL_OK);
    CHECK(memcmp(buffer + KB2_PROTOCOL_CHANNEL_HEADER_SCHEMA_DIGEST_OFFSET,
                 digest,
                 sizeof(digest)) == 0);
    return 0;
}

static int decode_channel(uint8_t *buffer, size_t buffer_size) {
    kb2_protocol_channel_t channel;
    kb2_protocol_queue_t queues[2];
    kb2_protocol_region_t region;
    size_t queue_count;
    size_t region_count;

    return kb2_protocol_channel_decode(buffer,
                                       buffer_size,
                                       &channel,
                                       queues,
                                       2,
                                       &queue_count,
                                       &region,
                                       1,
                                       &region_count);
}

static int test_channel_rejection(void) {
    uint8_t buffer[512];
    kb2_protocol_channel_t channel;
    kb2_protocol_queue_t queues[2];
    kb2_protocol_region_t region;
    size_t encoded_size;
    size_t queue_offset = KB2_PROTOCOL_CHANNEL_HEADER_SIZE;
    size_t region_offset =
        queue_offset + 2u * KB2_PROTOCOL_QUEUE_DESCRIPTOR_SIZE;

    make_channel(&channel, queues, &region);
    CHECK(kb2_protocol_channel_encode(
              buffer, sizeof(buffer), &encoded_size, &channel, queues, 2, &region, 1) ==
          KB2_PROTOCOL_OK);

    buffer[KB2_PROTOCOL_CHANNEL_HEADER_ABI_IDENTITY_OFFSET] ^= 1u;
    CHECK(decode_channel(buffer, encoded_size) == KB2_PROTOCOL_SCHEMA_MISMATCH);
    buffer[KB2_PROTOCOL_CHANNEL_HEADER_ABI_IDENTITY_OFFSET] ^= 1u;

    buffer[queue_offset + KB2_PROTOCOL_QUEUE_DESCRIPTOR_QUEUE_SIZE_OFFSET] = 15;
    CHECK(decode_channel(buffer, encoded_size) == KB2_PROTOCOL_MALFORMED);
    buffer[queue_offset + KB2_PROTOCOL_QUEUE_DESCRIPTOR_QUEUE_SIZE_OFFSET] = 16;

    buffer[region_offset + KB2_PROTOCOL_REGION_DESCRIPTOR_RIGHTS_OFFSET] =
        KB2_PROTOCOL_REGION_RIGHT_READ;
    CHECK(decode_channel(buffer, encoded_size) == KB2_PROTOCOL_MALFORMED);
    buffer[region_offset + KB2_PROTOCOL_REGION_DESCRIPTOR_RIGHTS_OFFSET] =
        KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE;

    buffer[KB2_PROTOCOL_CHANNEL_HEADER_RESERVED_0_OFFSET] = 1;
    CHECK(decode_channel(buffer, encoded_size) == KB2_PROTOCOL_MALFORMED);
    CHECK(decode_channel(buffer, KB2_PROTOCOL_CHANNEL_HEADER_SIZE - 1u) ==
          KB2_PROTOCOL_BUFFER_TOO_SMALL);
    return 0;
}

static int test_message_envelope(void) {
    uint8_t buffer[64] = {0};
    kb2_protocol_message_envelope_t envelope = {
        .protocol_id = 7,
        .opcode = 12,
        .flags = 3,
        .generation = 9,
        .correlation_id = 44,
        .payload_length = 8,
    };
    kb2_protocol_message_envelope_t decoded;

    CHECK(kb2_protocol_message_envelope_encode(buffer, sizeof(buffer), &envelope) ==
          KB2_PROTOCOL_OK);
    CHECK(kb2_protocol_message_envelope_decode(buffer, 48, &decoded) == KB2_PROTOCOL_OK);
    CHECK(decoded.protocol_id == envelope.protocol_id);
    CHECK(decoded.opcode == envelope.opcode);
    CHECK(decoded.generation == envelope.generation);
    CHECK(decoded.correlation_id == envelope.correlation_id);
    CHECK(decoded.payload_length == envelope.payload_length);

    buffer[KB2_PROTOCOL_MESSAGE_ENVELOPE_ABI_IDENTITY_OFFSET] ^= 1u;
    CHECK(kb2_protocol_message_envelope_decode(buffer, 48, &decoded) ==
          KB2_PROTOCOL_SCHEMA_MISMATCH);
    return 0;
}

int main(void) {
    CHECK(test_channel_round_trip() == 0);
    CHECK(test_channel_rejection() == 0);
    CHECK(test_message_envelope() == 0);
    return 0;
}
