/* SPDX-License-Identifier: Apache-2.0 */

#include <kobox2/closure_manifest.h>
#include <kobox2/protocol.h>
#include <kobox2/sha256.h>

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

static int test_sha256(void) {
    static const uint8_t expected[KB2_SHA256_DIGEST_SIZE] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    uint8_t digest[KB2_SHA256_DIGEST_SIZE];

    kb2_sha256("abc", 3, digest);
    CHECK(memcmp(digest, expected, sizeof(expected)) == 0);
    return 0;
}

#define TEST_STRING(value) {(value), (uint32_t)(sizeof(value) - 1u)}

static int test_closure_manifest(void) {
    kb2_closure_manifest_artifact_t artifact = {
        .node_id = 9,
        .kind = KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE,
        .flags = KB2_CLOSURE_ARTIFACT_FLAG_ROOT,
        .content_size = 64,
        .namespace_name = TEST_STRING("test_module"),
        .init_symbol = TEST_STRING("test_init"),
        .quiesce_symbol = TEST_STRING("test_quiesce"),
        .cleanup_symbol = TEST_STRING("test_cleanup"),
    };
    kb2_closure_manifest_source_t source = {
        .artifacts = &artifact,
        .artifact_count = 1,
    };
    kb2_closure_manifest_artifact_t decoded_artifact;
    kb2_closure_manifest_t manifest;
    uint8_t buffer[512];
    size_t encoded_size;

    memset(artifact.content_digest, 0x5a, sizeof(artifact.content_digest));
    CHECK(kb2_closure_manifest_encoded_size(&source, &encoded_size) == KB2_PROTOCOL_OK);
    CHECK(encoded_size < sizeof(buffer));
    CHECK(kb2_closure_manifest_encode(
              buffer, sizeof(buffer), &encoded_size, &source) == KB2_PROTOCOL_OK);
    CHECK(kb2_closure_manifest_decode(buffer, encoded_size, &manifest) == KB2_PROTOCOL_OK);
    CHECK(kb2_closure_manifest_artifact_count(&manifest) == 1);
    CHECK(kb2_closure_manifest_artifact(&manifest, 0, &decoded_artifact) ==
          KB2_PROTOCOL_OK);
    CHECK(decoded_artifact.node_id == artifact.node_id);
    CHECK(decoded_artifact.content_size == artifact.content_size);
    CHECK(decoded_artifact.namespace_name.length == artifact.namespace_name.length);
    CHECK(memcmp(decoded_artifact.namespace_name.data,
                 artifact.namespace_name.data,
                 artifact.namespace_name.length) == 0);

    buffer[KB2_CLOSURE_MANIFEST_HEADER_SCHEMA_DIGEST_OFFSET] ^= 1u;
    CHECK(kb2_closure_manifest_decode(buffer, encoded_size, &manifest) ==
          KB2_PROTOCOL_SCHEMA_MISMATCH);
    buffer[KB2_CLOSURE_MANIFEST_HEADER_SCHEMA_DIGEST_OFFSET] ^= 1u;
    buffer[KB2_CLOSURE_MANIFEST_HEADER_RESERVED_OFFSET] = 1;
    CHECK(kb2_closure_manifest_decode(buffer, encoded_size, &manifest) ==
          KB2_PROTOCOL_MALFORMED);
    buffer[KB2_CLOSURE_MANIFEST_HEADER_RESERVED_OFFSET] = 0;
    buffer[KB2_CLOSURE_MANIFEST_HEADER_SIZE +
           KB2_CLOSURE_ARTIFACT_DESCRIPTOR_FLAGS_OFFSET] = 0;
    CHECK(kb2_closure_manifest_decode(buffer, encoded_size, &manifest) ==
          KB2_PROTOCOL_MALFORMED);
    buffer[KB2_CLOSURE_MANIFEST_HEADER_SIZE +
           KB2_CLOSURE_ARTIFACT_DESCRIPTOR_FLAGS_OFFSET] =
        KB2_CLOSURE_ARTIFACT_FLAG_ROOT;
    buffer[KB2_CLOSURE_MANIFEST_HEADER_SIZE +
           KB2_CLOSURE_ARTIFACT_DESCRIPTOR_NAMESPACE_LENGTH_OFFSET] = 0;
    CHECK(kb2_closure_manifest_decode(buffer, encoded_size, &manifest) ==
          KB2_PROTOCOL_MALFORMED);
    return 0;
}

int main(void) {
    CHECK(test_channel_round_trip() == 0);
    CHECK(test_channel_rejection() == 0);
    CHECK(test_message_envelope() == 0);
    CHECK(test_sha256() == 0);
    CHECK(test_closure_manifest() == 0);
    return 0;
}
