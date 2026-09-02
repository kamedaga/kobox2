/* SPDX-License-Identifier: Apache-2.0 */

#include <kobox2/closure_manifest.h>
#include <kobox2/protocol.h>
#include <kobox2/resource_grant.h>
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

#define TEST_STRING(value) {(value), (uint32_t)(sizeof(value) - 1u)}

typedef struct test_package {
    uint8_t manifest_bytes[1024];
    size_t manifest_size;
    kb2_closure_manifest_t manifest;
    uint8_t interface_digest[KB2_RESOURCE_GRANT_SCHEMA_DIGEST_SIZE];
    kb2_resource_grant_slot_source_t slots[2];
    kb2_resource_grant_object_source_t objects[2];
    kb2_resource_grant_handle_binding_t bindings[3];
    kb2_resource_grant_source_t source;
} test_package_t;

static void store_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static int make_package(test_package_t *package) {
    kb2_closure_manifest_artifact_t artifact = {
        .node_id = 1,
        .kind = KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE,
        .flags = KB2_CLOSURE_ARTIFACT_FLAG_ROOT,
        .content_size = 64,
        .namespace_name = TEST_STRING("module"),
        .init_symbol = TEST_STRING("init"),
        .quiesce_symbol = TEST_STRING("quiesce"),
        .cleanup_symbol = TEST_STRING("cleanup"),
    };
    kb2_closure_manifest_resource_t resources[2] = {
        {
            .slot_id = 1,
            .type = KB2_CLOSURE_RESOURCE_CHANNEL,
            .minimum_count = 1,
            .maximum_count = 2,
            .required_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND,
            .maximum_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND |
                              KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE,
            .flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED |
                     KB2_CLOSURE_RESOURCE_FLAG_SHARED,
        },
        {
            .slot_id = 2,
            .type = KB2_CLOSURE_RESOURCE_NOTIFICATION,
            .minimum_count = 0,
            .maximum_count = 1,
            .required_rights = KB2_CLOSURE_NOTIFICATION_RIGHT_WAIT,
            .maximum_rights = KB2_CLOSURE_NOTIFICATION_RIGHT_WAIT |
                              KB2_CLOSURE_NOTIFICATION_RIGHT_SIGNAL,
        },
    };
    const kb2_closure_manifest_binding_t resource_bindings[2] = {{1, 1}, {2, 1}};
    kb2_closure_manifest_source_t manifest_source = {
        .artifacts = &artifact,
        .artifact_count = 1,
        .resources = resources,
        .resource_count = 2,
        .bindings = resource_bindings,
        .binding_count = 2,
    };
    size_t actual_size;

    memset(package, 0, sizeof(*package));
    memset(artifact.content_digest, 0x61, sizeof(artifact.content_digest));
    if (kb2_protocol_copy_schema_digest(package->interface_digest,
                                        sizeof(package->interface_digest)) !=
        KB2_PROTOCOL_OK) {
        return 0;
    }
    memcpy(resources[0].interface_schema_digest,
           package->interface_digest,
           sizeof(package->interface_digest));
    memcpy(resources[1].interface_schema_digest,
           package->interface_digest,
           sizeof(package->interface_digest));
    if (kb2_closure_manifest_encode(package->manifest_bytes,
                                    sizeof(package->manifest_bytes),
                                    &actual_size,
                                    &manifest_source) != KB2_PROTOCOL_OK ||
        kb2_closure_manifest_decode(
            package->manifest_bytes, actual_size, &package->manifest) != KB2_PROTOCOL_OK) {
        return 0;
    }
    package->manifest_size = actual_size;

    package->slots[0].slot_id = 1;
    package->slots[0].resource_type = KB2_CLOSURE_RESOURCE_CHANNEL;
    package->slots[0].state = KB2_RESOURCE_GRANT_SLOT_PRESENT;
    package->slots[1].slot_id = 2;
    package->slots[1].resource_type = KB2_CLOSURE_RESOURCE_NOTIFICATION;
    package->slots[1].state = KB2_RESOURCE_GRANT_SLOT_ABSENT;
    memcpy(package->slots[0].interface_schema_digest,
           package->interface_digest,
           sizeof(package->interface_digest));
    memcpy(package->slots[1].interface_schema_digest,
           package->interface_digest,
           sizeof(package->interface_digest));

    package->objects[0].slot_id = 1;
    package->objects[0].object_id = 10;
    package->objects[0].granted_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND;
    package->objects[1].slot_id = 1;
    package->objects[1].object_id = 11;
    package->objects[1].granted_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND |
                                         KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE;

    package->bindings[0].object_id = 10;
    package->bindings[0].role = 0;
    package->bindings[0].transfer_handle_index = 0;
    package->bindings[1].object_id = 10;
    package->bindings[1].role = 1;
    package->bindings[1].transfer_handle_index = 1;
    package->bindings[2].object_id = 11;
    package->bindings[2].role = 0;
    package->bindings[2].transfer_handle_index = 2;

    package->source.generation = 7;
    kb2_sha256(package->manifest_bytes,
               package->manifest_size,
               package->source.closure_manifest_digest);
    package->source.slots = package->slots;
    package->source.slot_count = 2;
    package->source.objects = package->objects;
    package->source.object_count = 2;
    package->source.handle_bindings = package->bindings;
    package->source.handle_binding_count = 3;
    return 1;
}

static int encode_grant(const kb2_resource_grant_source_t *source,
                        uint8_t *buffer,
                        size_t capacity,
                        size_t *size_out) {
    return kb2_resource_grant_encode(buffer, capacity, size_out, source) ==
           KB2_PROTOCOL_OK;
}

static int test_round_trip_and_canonical_encoding(void) {
    test_package_t package;
    kb2_resource_grant_t grant;
    kb2_resource_grant_slot_t slot;
    kb2_resource_grant_object_t object;
    kb2_resource_grant_handle_binding_t binding;
    uint8_t first[1024];
    uint8_t second[1024];
    uint8_t schema_digest[KB2_RESOURCE_GRANT_SCHEMA_DIGEST_SIZE];
    size_t first_size;
    size_t second_size;

    CHECK(make_package(&package));
    CHECK(encode_grant(&package.source, first, sizeof(first), &first_size));
    CHECK(encode_grant(&package.source, second, sizeof(second), &second_size));
    CHECK(first_size == second_size && memcmp(first, second, first_size) == 0);
    CHECK(kb2_resource_grant_decode(first, first_size, &grant) == KB2_PROTOCOL_OK);
    CHECK(kb2_resource_grant_validate_manifest(&grant, &package.manifest) ==
          KB2_PROTOCOL_OK);
    CHECK(grant.generation == package.source.generation);
    CHECK(kb2_resource_grant_slot_count(&grant) == 2);
    CHECK(kb2_resource_grant_object_count(&grant) == 2);
    CHECK(kb2_resource_grant_handle_binding_count(&grant) == 3);
    CHECK(kb2_resource_grant_slot(&grant, 1, &slot) == KB2_PROTOCOL_OK);
    CHECK(slot.slot_id == 2 && slot.state == KB2_RESOURCE_GRANT_SLOT_ABSENT &&
          slot.object_count == 0);
    CHECK(kb2_resource_grant_object(&grant, 1, &object) == KB2_PROTOCOL_OK);
    CHECK(object.object_id == 11 && object.handle_start == 2 && object.handle_count == 1);
    CHECK(kb2_resource_grant_handle_binding(&grant, 2, &binding) == KB2_PROTOCOL_OK);
    CHECK(binding.object_id == 11 && binding.role == 0 &&
          binding.transfer_handle_index == 2);
    CHECK(kb2_resource_grant_copy_schema_digest(schema_digest, sizeof(schema_digest)) ==
          KB2_PROTOCOL_OK);
    CHECK(memcmp(first + KB2_RESOURCE_GRANT_GRANT_HEADER_SCHEMA_DIGEST_OFFSET,
                 schema_digest,
                 sizeof(schema_digest)) == 0);
    return 0;
}

static int test_source_rejection(void) {
    test_package_t package;
    kb2_resource_grant_source_t source;
    kb2_resource_grant_slot_source_t slots[2];
    kb2_resource_grant_object_source_t objects[2];
    kb2_resource_grant_handle_binding_t bindings[3];
    size_t encoded_size;

    CHECK(make_package(&package));
    source = package.source;
    memcpy(slots, package.slots, sizeof(slots));
    memcpy(objects, package.objects, sizeof(objects));
    memcpy(bindings, package.bindings, sizeof(bindings));
    source.slots = slots;
    source.objects = objects;
    source.handle_bindings = bindings;

    slots[0] = package.slots[1];
    slots[1] = package.slots[0];
    CHECK(kb2_resource_grant_encoded_size(&source, &encoded_size) ==
          KB2_PROTOCOL_MALFORMED);
    memcpy(slots, package.slots, sizeof(slots));

    objects[0] = package.objects[1];
    objects[1] = package.objects[0];
    CHECK(kb2_resource_grant_encoded_size(&source, &encoded_size) ==
          KB2_PROTOCOL_MALFORMED);
    memcpy(objects, package.objects, sizeof(objects));

    objects[1].object_id = objects[0].object_id;
    CHECK(kb2_resource_grant_encoded_size(&source, &encoded_size) ==
          KB2_PROTOCOL_MALFORMED);
    objects[1] = package.objects[1];

    bindings[1].role = bindings[0].role;
    CHECK(kb2_resource_grant_encoded_size(&source, &encoded_size) ==
          KB2_PROTOCOL_MALFORMED);
    bindings[1] = package.bindings[1];

    bindings[1].transfer_handle_index = 2;
    CHECK(kb2_resource_grant_encoded_size(&source, &encoded_size) ==
          KB2_PROTOCOL_MALFORMED);
    bindings[1].transfer_handle_index = 0;
    CHECK(kb2_resource_grant_encoded_size(&source, &encoded_size) ==
          KB2_PROTOCOL_MALFORMED);
    bindings[1] = package.bindings[1];

    slots[0].state = KB2_RESOURCE_GRANT_SLOT_ABSENT;
    CHECK(kb2_resource_grant_encoded_size(&source, &encoded_size) ==
          KB2_PROTOCOL_MALFORMED);
    slots[0] = package.slots[0];
    slots[1].state = KB2_RESOURCE_GRANT_SLOT_PRESENT;
    CHECK(kb2_resource_grant_encoded_size(&source, &encoded_size) ==
          KB2_PROTOCOL_MALFORMED);
    slots[1] = package.slots[1];

    memset(slots[0].interface_schema_digest, 0, sizeof(slots[0].interface_schema_digest));
    CHECK(kb2_resource_grant_encoded_size(&source, &encoded_size) ==
          KB2_PROTOCOL_MALFORMED);
    slots[0] = package.slots[0];
    objects[0].granted_rights = UINT64_C(1) << 40;
    CHECK(kb2_resource_grant_encoded_size(&source, &encoded_size) ==
          KB2_PROTOCOL_MALFORMED);
    return 0;
}

static int test_decoded_rejection(void) {
    test_package_t package;
    kb2_resource_grant_t grant;
    uint8_t buffer[1024];
    uint8_t original_digest[KB2_RESOURCE_GRANT_SCHEMA_DIGEST_SIZE];
    size_t size;
    size_t slot_offset;
    size_t object_offset;
    size_t binding_offset;

    CHECK(make_package(&package));
    CHECK(encode_grant(&package.source, buffer, sizeof(buffer), &size));
    slot_offset = KB2_RESOURCE_GRANT_GRANT_HEADER_SIZE;
    object_offset = slot_offset + 2u * KB2_RESOURCE_GRANT_SLOT_GRANT_SIZE;
    binding_offset = object_offset + 2u * KB2_RESOURCE_GRANT_OBJECT_SIZE;

    buffer[KB2_RESOURCE_GRANT_GRANT_HEADER_SCHEMA_DIGEST_OFFSET] ^= 1u;
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) ==
          KB2_PROTOCOL_SCHEMA_MISMATCH);
    buffer[KB2_RESOURCE_GRANT_GRANT_HEADER_SCHEMA_DIGEST_OFFSET] ^= 1u;

    store_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_OBJECT_TABLE_OFFSET_OFFSET,
              (uint32_t)object_offset + 1u);
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_MALFORMED);
    store_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_OBJECT_TABLE_OFFSET_OFFSET,
              (uint32_t)object_offset);

    store_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_SLOT_COUNT_OFFSET, 3);
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_MALFORMED);
    store_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_SLOT_COUNT_OFFSET, 2);

    buffer[slot_offset + KB2_RESOURCE_GRANT_SLOT_GRANT_INTERFACE_SCHEMA_DIGEST_OFFSET] = 0;
    memcpy(original_digest,
           package.interface_digest,
           sizeof(original_digest));
    memset(buffer + slot_offset + KB2_RESOURCE_GRANT_SLOT_GRANT_INTERFACE_SCHEMA_DIGEST_OFFSET,
           0,
           sizeof(original_digest));
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_MALFORMED);
    memcpy(buffer + slot_offset + KB2_RESOURCE_GRANT_SLOT_GRANT_INTERFACE_SCHEMA_DIGEST_OFFSET,
           original_digest,
           sizeof(original_digest));

    store_u32(buffer + object_offset + KB2_RESOURCE_GRANT_OBJECT_HANDLE_START_OFFSET, 1);
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_MALFORMED);
    store_u32(buffer + object_offset + KB2_RESOURCE_GRANT_OBJECT_HANDLE_START_OFFSET, 0);

    memcpy(buffer + object_offset + KB2_RESOURCE_GRANT_OBJECT_SIZE +
               KB2_RESOURCE_GRANT_OBJECT_OBJECT_ID_OFFSET,
           buffer + object_offset + KB2_RESOURCE_GRANT_OBJECT_OBJECT_ID_OFFSET,
           sizeof(uint64_t));
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_MALFORMED);
    CHECK(encode_grant(&package.source, buffer, sizeof(buffer), &size));

    store_u32(buffer + binding_offset + KB2_RESOURCE_GRANT_HANDLE_BINDING_SIZE +
                  KB2_RESOURCE_GRANT_HANDLE_BINDING_ROLE_OFFSET,
              0);
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_MALFORMED);
    CHECK(encode_grant(&package.source, buffer, sizeof(buffer), &size));
    store_u32(buffer + binding_offset + KB2_RESOURCE_GRANT_HANDLE_BINDING_SIZE +
                  KB2_RESOURCE_GRANT_HANDLE_BINDING_TRANSFER_HANDLE_INDEX_OFFSET,
              7);
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_MALFORMED);
    return 0;
}

static int test_manifest_validation(void) {
    test_package_t package;
    kb2_resource_grant_t grant;
    kb2_resource_grant_source_t source;
    kb2_resource_grant_slot_source_t slots[2];
    kb2_resource_grant_object_source_t objects[3];
    uint8_t buffer[1024];
    size_t size;
    size_t slot_offset;
    size_t object_offset;

    CHECK(make_package(&package));
    source = package.source;
    memcpy(slots, package.slots, sizeof(slots));
    memcpy(objects, package.objects, sizeof(package.objects));
    source.slots = slots;
    source.objects = objects;
    CHECK(encode_grant(&package.source, buffer, sizeof(buffer), &size));
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_OK);
    slot_offset = KB2_RESOURCE_GRANT_GRANT_HEADER_SIZE;
    object_offset = slot_offset + 2u * KB2_RESOURCE_GRANT_SLOT_GRANT_SIZE;

    buffer[slot_offset + KB2_RESOURCE_GRANT_SLOT_GRANT_INTERFACE_SCHEMA_DIGEST_OFFSET] ^=
        1u;
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_OK);
    CHECK(kb2_resource_grant_validate_manifest(&grant, &package.manifest) ==
          KB2_PROTOCOL_SCHEMA_MISMATCH);
    CHECK(encode_grant(&package.source, buffer, sizeof(buffer), &size));

    buffer[object_offset + KB2_RESOURCE_GRANT_OBJECT_GRANTED_RIGHTS_OFFSET] =
        KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE;
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_OK);
    CHECK(kb2_resource_grant_validate_manifest(&grant, &package.manifest) ==
          KB2_PROTOCOL_MALFORMED);
    CHECK(encode_grant(&package.source, buffer, sizeof(buffer), &size));

    buffer[KB2_RESOURCE_GRANT_GRANT_HEADER_CLOSURE_MANIFEST_DIGEST_OFFSET] ^= 1u;
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_OK);
    CHECK(kb2_resource_grant_validate_manifest(&grant, &package.manifest) ==
          KB2_PROTOCOL_SCHEMA_MISMATCH);

    slots[0].state = KB2_RESOURCE_GRANT_SLOT_ABSENT;
    source.object_count = 0;
    source.handle_binding_count = 0;
    CHECK(encode_grant(&source, buffer, sizeof(buffer), &size));
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_OK);
    CHECK(kb2_resource_grant_validate_manifest(&grant, &package.manifest) ==
          KB2_PROTOCOL_MALFORMED);

    slots[0] = package.slots[0];
    objects[2].slot_id = 1;
    objects[2].object_id = 12;
    objects[2].granted_rights = KB2_CLOSURE_CHANNEL_RIGHT_SEND;
    source.object_count = 3;
    source.handle_binding_count = package.source.handle_binding_count;
    CHECK(encode_grant(&source, buffer, sizeof(buffer), &size));
    CHECK(kb2_resource_grant_decode(buffer, size, &grant) == KB2_PROTOCOL_OK);
    CHECK(kb2_resource_grant_validate_manifest(&grant, &package.manifest) ==
          KB2_PROTOCOL_MALFORMED);
    return 0;
}

int main(void) {
    CHECK(test_round_trip_and_canonical_encoding() == 0);
    CHECK(test_source_rejection() == 0);
    CHECK(test_decoded_rejection() == 0);
    CHECK(test_manifest_validation() == 0);
    return 0;
}
