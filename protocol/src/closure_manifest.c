/* SPDX-License-Identifier: MIT */

#include <kobox2/closure_manifest.h>

#include <limits.h>
#include <string.h>

enum {
    ARTIFACTS = 0,
    DEPENDENCIES,
    EXPORTS,
    IMPORTS,
    RESOURCES,
    BINDINGS,
    STRINGS,
};

static const uint8_t schema_digest[KB2_CLOSURE_SCHEMA_DIGEST_SIZE] =
    KB2_CLOSURE_SCHEMA_SHA256_BYTES;
static const uint8_t abi_identity[KB2_CLOSURE_ABI_IDENTITY_SIZE] =
    KB2_CLOSURE_ABI_IDENTITY_BYTES;

static uint32_t load_u32(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) | ((uint32_t)source[3] << 24u);
}

static uint64_t load_u64(const uint8_t *source) {
    return (uint64_t)load_u32(source) | ((uint64_t)load_u32(source + 4) << 32u);
}

static void store_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static void store_u64(uint8_t *destination, uint64_t value) {
    store_u32(destination, (uint32_t)value);
    store_u32(destination + 4, (uint32_t)(value >> 32u));
}

static int add_size(size_t *value, size_t count, size_t item_size) {
    if (count != 0 && item_size > (SIZE_MAX - *value) / count) {
        return 0;
    }
    *value += count * item_size;
    return 1;
}

static int digest_is_zero(const uint8_t digest[KB2_CLOSURE_SCHEMA_DIGEST_SIZE]) {
    uint8_t combined = 0;
    size_t index;

    for (index = 0; index < KB2_CLOSURE_SCHEMA_DIGEST_SIZE; ++index) {
        combined = (uint8_t)(combined | digest[index]);
    }
    return combined == 0;
}

static int string_valid(kb2_closure_string_t string) {
    uint32_t index;

    if (string.data == NULL || string.length == 0 || string.length > 127u) {
        return 0;
    }
    for (index = 0; index < string.length; ++index) {
        unsigned char byte = (unsigned char)string.data[index];

        if (byte < 0x21u || byte > 0x7eu || byte == '/' || byte == '\\') {
            return 0;
        }
    }
    return 1;
}

static int source_valid(const kb2_closure_manifest_source_t *source) {
    size_t index;

    if (source == NULL || source->artifacts == NULL || source->artifact_count == 0 ||
        source->artifact_count > KB2_CLOSURE_MAX_ARTIFACTS ||
        source->dependency_count > KB2_CLOSURE_MAX_DEPENDENCIES ||
        source->export_count > KB2_CLOSURE_MAX_EXPORTS ||
        source->import_count > KB2_CLOSURE_MAX_IMPORTS ||
        source->resource_count > KB2_CLOSURE_MAX_RESOURCES ||
        source->binding_count > KB2_CLOSURE_MAX_RESOURCE_BINDINGS ||
        (source->dependency_count != 0 && source->dependencies == NULL) ||
        (source->export_count != 0 && source->exports == NULL) ||
        (source->import_count != 0 && source->imports == NULL) ||
        (source->resource_count != 0 && source->resources == NULL) ||
        (source->binding_count != 0 && source->bindings == NULL)) {
        return 0;
    }
    for (index = 0; index < source->resource_count; ++index) {
        if (digest_is_zero(source->resources[index].interface_schema_digest)) {
            return 0;
        }
    }
    return 1;
}

static int source_string_size(const kb2_closure_manifest_source_t *source, size_t *size_out) {
    size_t size = 0;
    size_t index;

    for (index = 0; index < source->artifact_count; ++index) {
        const kb2_closure_manifest_artifact_t *artifact = &source->artifacts[index];

        if (!string_valid(artifact->namespace_name) || !string_valid(artifact->init_symbol) ||
            !string_valid(artifact->quiesce_symbol) || !string_valid(artifact->cleanup_symbol) ||
            !add_size(&size, 1, artifact->namespace_name.length) ||
            !add_size(&size, 1, artifact->init_symbol.length) ||
            !add_size(&size, 1, artifact->quiesce_symbol.length) ||
            !add_size(&size, 1, artifact->cleanup_symbol.length)) {
            return 0;
        }
    }
    for (index = 0; index < source->export_count; ++index) {
        if (!string_valid(source->exports[index].name) ||
            !add_size(&size, 1, source->exports[index].name.length)) {
            return 0;
        }
    }
    for (index = 0; index < source->import_count; ++index) {
        if (!string_valid(source->imports[index].consumer_name) ||
            !string_valid(source->imports[index].provider_name) ||
            !add_size(&size, 1, source->imports[index].consumer_name.length) ||
            !add_size(&size, 1, source->imports[index].provider_name.length)) {
            return 0;
        }
    }
    *size_out = size;
    return size <= UINT32_MAX;
}

const char *kb2_closure_schema_sha256_hex(void) {
    return KB2_CLOSURE_SCHEMA_SHA256_HEX;
}

kb2_protocol_status_t kb2_closure_copy_schema_digest(uint8_t *digest_out,
                                                      size_t digest_size) {
    if (digest_out == NULL || digest_size != sizeof(schema_digest)) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    memcpy(digest_out, schema_digest, sizeof(schema_digest));
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_closure_manifest_encoded_size(
    const kb2_closure_manifest_source_t *source, size_t *size_out) {
    size_t size = KB2_CLOSURE_MANIFEST_HEADER_SIZE;
    size_t string_size;

    if (size_out == NULL || !source_valid(source) || !source_string_size(source, &string_size) ||
        !add_size(&size, source->artifact_count, KB2_CLOSURE_ARTIFACT_DESCRIPTOR_SIZE) ||
        !add_size(&size, source->dependency_count, KB2_CLOSURE_DEPENDENCY_DESCRIPTOR_SIZE) ||
        !add_size(&size, source->export_count, KB2_CLOSURE_SYMBOL_DESCRIPTOR_SIZE) ||
        !add_size(&size, source->import_count, KB2_CLOSURE_IMPORT_DESCRIPTOR_SIZE) ||
        !add_size(&size, source->resource_count, KB2_CLOSURE_RESOURCE_DESCRIPTOR_SIZE) ||
        !add_size(&size, source->binding_count, KB2_CLOSURE_RESOURCE_BINDING_SIZE) ||
        !add_size(&size, 1, string_size) || size > UINT32_MAX) {
        return source == NULL || size_out == NULL ? KB2_PROTOCOL_INVALID_ARGUMENT
                                                  : KB2_PROTOCOL_OVERFLOW;
    }
    *size_out = size;
    return KB2_PROTOCOL_OK;
}

static uint32_t append_string(uint8_t *buffer,
                              uint32_t string_table_offset,
                              uint32_t *cursor,
                              kb2_closure_string_t string) {
    uint32_t offset = *cursor;

    memcpy(buffer + string_table_offset + offset, string.data, string.length);
    *cursor += string.length;
    return offset;
}

static void store_string_reference(uint8_t *record,
                                   uint32_t offset_field,
                                   uint32_t length_field,
                                   uint8_t *buffer,
                                   uint32_t string_table_offset,
                                   uint32_t *cursor,
                                   kb2_closure_string_t string) {
    store_u32(record + offset_field,
              append_string(buffer, string_table_offset, cursor, string));
    store_u32(record + length_field, string.length);
}

kb2_protocol_status_t kb2_closure_manifest_encode(
    uint8_t *buffer,
    size_t buffer_size,
    size_t *encoded_size_out,
    const kb2_closure_manifest_source_t *source) {
    static const uint32_t sizes[6] = {
        KB2_CLOSURE_ARTIFACT_DESCRIPTOR_SIZE,
        KB2_CLOSURE_DEPENDENCY_DESCRIPTOR_SIZE,
        KB2_CLOSURE_SYMBOL_DESCRIPTOR_SIZE,
        KB2_CLOSURE_IMPORT_DESCRIPTOR_SIZE,
        KB2_CLOSURE_RESOURCE_DESCRIPTOR_SIZE,
        KB2_CLOSURE_RESOURCE_BINDING_SIZE,
    };
    size_t encoded_size;
    uint32_t counts[6];
    uint32_t offsets[7];
    uint32_t string_cursor = 0;
    size_t index;

    if (buffer == NULL || encoded_size_out == NULL || source == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (kb2_closure_manifest_encoded_size(source, &encoded_size) != KB2_PROTOCOL_OK) {
        return KB2_PROTOCOL_MALFORMED;
    }
    if (buffer_size < encoded_size) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }
    counts[ARTIFACTS] = (uint32_t)source->artifact_count;
    counts[DEPENDENCIES] = (uint32_t)source->dependency_count;
    counts[EXPORTS] = (uint32_t)source->export_count;
    counts[IMPORTS] = (uint32_t)source->import_count;
    counts[RESOURCES] = (uint32_t)source->resource_count;
    counts[BINDINGS] = (uint32_t)source->binding_count;
    offsets[ARTIFACTS] = KB2_CLOSURE_MANIFEST_HEADER_SIZE;
    for (index = 0; index < 6; ++index) {
        offsets[index + 1] = offsets[index] + counts[index] * sizes[index];
    }
    memset(buffer, 0, encoded_size);
    store_u32(buffer + KB2_CLOSURE_MANIFEST_HEADER_TOTAL_SIZE_OFFSET, (uint32_t)encoded_size);
    memcpy(buffer + KB2_CLOSURE_MANIFEST_HEADER_ABI_IDENTITY_OFFSET,
           abi_identity,
           sizeof(abi_identity));
    memcpy(buffer + KB2_CLOSURE_MANIFEST_HEADER_SCHEMA_DIGEST_OFFSET,
           schema_digest,
           sizeof(schema_digest));
    for (index = 0; index < 6; ++index) {
        store_u32(buffer + KB2_CLOSURE_MANIFEST_HEADER_ARTIFACT_COUNT_OFFSET + index * 4u,
                  counts[index]);
        store_u32(buffer + KB2_CLOSURE_MANIFEST_HEADER_ARTIFACT_TABLE_OFFSET_OFFSET +
                      index * 4u,
                  offsets[index]);
    }
    store_u32(buffer + KB2_CLOSURE_MANIFEST_HEADER_STRING_TABLE_OFFSET_OFFSET,
              offsets[STRINGS]);
    store_u32(buffer + KB2_CLOSURE_MANIFEST_HEADER_STRING_TABLE_SIZE_OFFSET,
              (uint32_t)(encoded_size - offsets[STRINGS]));

    for (index = 0; index < source->artifact_count; ++index) {
        const kb2_closure_manifest_artifact_t *artifact = &source->artifacts[index];
        uint8_t *record = buffer + offsets[ARTIFACTS] +
                          index * KB2_CLOSURE_ARTIFACT_DESCRIPTOR_SIZE;

        store_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_NODE_ID_OFFSET, artifact->node_id);
        store_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_KIND_OFFSET, artifact->kind);
        store_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_FLAGS_OFFSET, artifact->flags);
        store_u64(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_CONTENT_SIZE_OFFSET,
                  artifact->content_size);
        memcpy(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_CONTENT_DIGEST_OFFSET,
               artifact->content_digest,
               sizeof(artifact->content_digest));
        store_string_reference(record,
                               KB2_CLOSURE_ARTIFACT_DESCRIPTOR_NAMESPACE_OFFSET_OFFSET,
                               KB2_CLOSURE_ARTIFACT_DESCRIPTOR_NAMESPACE_LENGTH_OFFSET,
                               buffer,
                               offsets[STRINGS],
                               &string_cursor,
                               artifact->namespace_name);
        store_string_reference(record,
                               KB2_CLOSURE_ARTIFACT_DESCRIPTOR_INIT_OFFSET_OFFSET,
                               KB2_CLOSURE_ARTIFACT_DESCRIPTOR_INIT_LENGTH_OFFSET,
                               buffer,
                               offsets[STRINGS],
                               &string_cursor,
                               artifact->init_symbol);
        store_string_reference(record,
                               KB2_CLOSURE_ARTIFACT_DESCRIPTOR_QUIESCE_OFFSET_OFFSET,
                               KB2_CLOSURE_ARTIFACT_DESCRIPTOR_QUIESCE_LENGTH_OFFSET,
                               buffer,
                               offsets[STRINGS],
                               &string_cursor,
                               artifact->quiesce_symbol);
        store_string_reference(record,
                               KB2_CLOSURE_ARTIFACT_DESCRIPTOR_CLEANUP_OFFSET_OFFSET,
                               KB2_CLOSURE_ARTIFACT_DESCRIPTOR_CLEANUP_LENGTH_OFFSET,
                               buffer,
                               offsets[STRINGS],
                               &string_cursor,
                               artifact->cleanup_symbol);
    }
    for (index = 0; index < source->dependency_count; ++index) {
        uint8_t *record = buffer + offsets[DEPENDENCIES] +
                          index * KB2_CLOSURE_DEPENDENCY_DESCRIPTOR_SIZE;

        store_u32(record + KB2_CLOSURE_DEPENDENCY_DESCRIPTOR_CONSUMER_NODE_ID_OFFSET,
                  source->dependencies[index].consumer_node_id);
        store_u32(record + KB2_CLOSURE_DEPENDENCY_DESCRIPTOR_PROVIDER_NODE_ID_OFFSET,
                  source->dependencies[index].provider_node_id);
    }
    for (index = 0; index < source->export_count; ++index) {
        uint8_t *record = buffer + offsets[EXPORTS] +
                          index * KB2_CLOSURE_SYMBOL_DESCRIPTOR_SIZE;

        store_u32(record + KB2_CLOSURE_SYMBOL_DESCRIPTOR_NODE_ID_OFFSET,
                  source->exports[index].node_id);
        store_u32(record + KB2_CLOSURE_SYMBOL_DESCRIPTOR_KIND_OFFSET,
                  source->exports[index].kind);
        store_string_reference(record,
                               KB2_CLOSURE_SYMBOL_DESCRIPTOR_NAME_OFFSET_OFFSET,
                               KB2_CLOSURE_SYMBOL_DESCRIPTOR_NAME_LENGTH_OFFSET,
                               buffer,
                               offsets[STRINGS],
                               &string_cursor,
                               source->exports[index].name);
    }
    for (index = 0; index < source->import_count; ++index) {
        uint8_t *record = buffer + offsets[IMPORTS] +
                          index * KB2_CLOSURE_IMPORT_DESCRIPTOR_SIZE;

        store_u32(record + KB2_CLOSURE_IMPORT_DESCRIPTOR_CONSUMER_NODE_ID_OFFSET,
                  source->imports[index].consumer_node_id);
        store_u32(record + KB2_CLOSURE_IMPORT_DESCRIPTOR_PROVIDER_NODE_ID_OFFSET,
                  source->imports[index].provider_node_id);
        store_u32(record + KB2_CLOSURE_IMPORT_DESCRIPTOR_KIND_OFFSET,
                  source->imports[index].kind);
        store_u32(record + KB2_CLOSURE_IMPORT_DESCRIPTOR_FLAGS_OFFSET,
                  source->imports[index].flags);
        store_string_reference(record,
                               KB2_CLOSURE_IMPORT_DESCRIPTOR_CONSUMER_NAME_OFFSET_OFFSET,
                               KB2_CLOSURE_IMPORT_DESCRIPTOR_CONSUMER_NAME_LENGTH_OFFSET,
                               buffer,
                               offsets[STRINGS],
                               &string_cursor,
                               source->imports[index].consumer_name);
        store_string_reference(record,
                               KB2_CLOSURE_IMPORT_DESCRIPTOR_PROVIDER_NAME_OFFSET_OFFSET,
                               KB2_CLOSURE_IMPORT_DESCRIPTOR_PROVIDER_NAME_LENGTH_OFFSET,
                               buffer,
                               offsets[STRINGS],
                               &string_cursor,
                               source->imports[index].provider_name);
    }
    for (index = 0; index < source->resource_count; ++index) {
        const kb2_closure_manifest_resource_t *resource = &source->resources[index];
        uint8_t *record = buffer + offsets[RESOURCES] +
                          index * KB2_CLOSURE_RESOURCE_DESCRIPTOR_SIZE;

        store_u32(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_SLOT_ID_OFFSET, resource->slot_id);
        store_u32(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_TYPE_OFFSET, resource->type);
        store_u32(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_MINIMUM_COUNT_OFFSET,
                  resource->minimum_count);
        store_u32(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_MAXIMUM_COUNT_OFFSET,
                  resource->maximum_count);
        store_u64(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_REQUIRED_RIGHTS_OFFSET,
                  resource->required_rights);
        store_u64(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_MAXIMUM_RIGHTS_OFFSET,
                  resource->maximum_rights);
        store_u32(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_FLAGS_OFFSET, resource->flags);
        memcpy(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_INTERFACE_SCHEMA_DIGEST_OFFSET,
               resource->interface_schema_digest,
               sizeof(resource->interface_schema_digest));
    }
    for (index = 0; index < source->binding_count; ++index) {
        uint8_t *record = buffer + offsets[BINDINGS] +
                          index * KB2_CLOSURE_RESOURCE_BINDING_SIZE;

        store_u32(record + KB2_CLOSURE_RESOURCE_BINDING_SLOT_ID_OFFSET,
                  source->bindings[index].slot_id);
        store_u32(record + KB2_CLOSURE_RESOURCE_BINDING_NODE_ID_OFFSET,
                  source->bindings[index].node_id);
    }
    *encoded_size_out = encoded_size;
    return KB2_PROTOCOL_OK;
}

static int bytes_are_zero(const uint8_t *bytes, size_t size) {
    size_t index;

    for (index = 0; index < size; ++index) {
        if (bytes[index] != 0) {
            return 0;
        }
    }
    return 1;
}

static int decoded_string(const kb2_closure_manifest_t *manifest,
                          uint32_t offset,
                          uint32_t length,
                          kb2_closure_string_t *string_out) {
    kb2_closure_string_t string;

    if (offset > manifest->string_table_size || length > manifest->string_table_size - offset) {
        return 0;
    }
    string.data = (const char *)manifest->bytes + manifest->offsets[STRINGS] + offset;
    string.length = length;
    if (!string_valid(string)) {
        return 0;
    }
    *string_out = string;
    return 1;
}

static int manifest_has_node(const kb2_closure_manifest_t *manifest, uint32_t node_id) {
    size_t left = 0;
    size_t right = manifest->counts[ARTIFACTS];

    while (left < right) {
        kb2_closure_manifest_artifact_t artifact;
        size_t middle = left + (right - left) / 2u;

        if (kb2_closure_manifest_artifact(manifest, middle, &artifact) != KB2_PROTOCOL_OK) {
            return 0;
        }
        if (artifact.node_id == node_id) {
            return 1;
        }
        if (artifact.node_id < node_id) {
            left = middle + 1u;
        } else {
            right = middle;
        }
    }
    return 0;
}

kb2_protocol_status_t kb2_closure_manifest_decode(const uint8_t *buffer,
                                                   size_t buffer_size,
                                                   kb2_closure_manifest_t *manifest_out) {
    static const uint32_t limits[6] = {
        KB2_CLOSURE_MAX_ARTIFACTS,
        KB2_CLOSURE_MAX_DEPENDENCIES,
        KB2_CLOSURE_MAX_EXPORTS,
        KB2_CLOSURE_MAX_IMPORTS,
        KB2_CLOSURE_MAX_RESOURCES,
        KB2_CLOSURE_MAX_RESOURCE_BINDINGS,
    };
    static const uint32_t sizes[6] = {
        KB2_CLOSURE_ARTIFACT_DESCRIPTOR_SIZE,
        KB2_CLOSURE_DEPENDENCY_DESCRIPTOR_SIZE,
        KB2_CLOSURE_SYMBOL_DESCRIPTOR_SIZE,
        KB2_CLOSURE_IMPORT_DESCRIPTOR_SIZE,
        KB2_CLOSURE_RESOURCE_DESCRIPTOR_SIZE,
        KB2_CLOSURE_RESOURCE_BINDING_SIZE,
    };
    kb2_closure_manifest_t manifest = {0};
    uint32_t expected_offset = KB2_CLOSURE_MANIFEST_HEADER_SIZE;
    uint32_t prior_node_id = 0;
    size_t root_count = 0;
    size_t index;

    if (buffer == NULL || manifest_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    memset(manifest_out, 0, sizeof(*manifest_out));
    if (buffer_size < KB2_CLOSURE_MANIFEST_HEADER_SIZE) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }
    if (buffer_size > UINT32_MAX) {
        return KB2_PROTOCOL_OVERFLOW;
    }
    if (memcmp(buffer + KB2_CLOSURE_MANIFEST_HEADER_ABI_IDENTITY_OFFSET,
               abi_identity,
               sizeof(abi_identity)) != 0 ||
        memcmp(buffer + KB2_CLOSURE_MANIFEST_HEADER_SCHEMA_DIGEST_OFFSET,
               schema_digest,
               sizeof(schema_digest)) != 0) {
        return KB2_PROTOCOL_SCHEMA_MISMATCH;
    }
    if (load_u32(buffer + KB2_CLOSURE_MANIFEST_HEADER_TOTAL_SIZE_OFFSET) != buffer_size) {
        return KB2_PROTOCOL_MALFORMED;
    }
    if (!bytes_are_zero(buffer + KB2_CLOSURE_MANIFEST_HEADER_RESERVED_OFFSET, 32u)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    manifest.bytes = buffer;
    manifest.size = buffer_size;
    for (index = 0; index < 6; ++index) {
        uint64_t next;

        manifest.counts[index] =
            load_u32(buffer + KB2_CLOSURE_MANIFEST_HEADER_ARTIFACT_COUNT_OFFSET + index * 4u);
        manifest.offsets[index] = load_u32(
            buffer + KB2_CLOSURE_MANIFEST_HEADER_ARTIFACT_TABLE_OFFSET_OFFSET + index * 4u);
        if (manifest.counts[index] > limits[index] || manifest.offsets[index] != expected_offset) {
            return KB2_PROTOCOL_MALFORMED;
        }
        next = (uint64_t)expected_offset + (uint64_t)manifest.counts[index] * sizes[index];
        if (next > buffer_size) {
            return KB2_PROTOCOL_MALFORMED;
        }
        expected_offset = (uint32_t)next;
    }
    manifest.offsets[STRINGS] =
        load_u32(buffer + KB2_CLOSURE_MANIFEST_HEADER_STRING_TABLE_OFFSET_OFFSET);
    manifest.string_table_size =
        load_u32(buffer + KB2_CLOSURE_MANIFEST_HEADER_STRING_TABLE_SIZE_OFFSET);
    if (manifest.counts[ARTIFACTS] == 0 || manifest.offsets[STRINGS] != expected_offset ||
        manifest.string_table_size != buffer_size - expected_offset) {
        return KB2_PROTOCOL_MALFORMED;
    }
    *manifest_out = manifest;
    for (index = 0; index < manifest.counts[ARTIFACTS]; ++index) {
        kb2_closure_manifest_artifact_t artifact;

        if (kb2_closure_manifest_artifact(manifest_out, index, &artifact) != KB2_PROTOCOL_OK) {
            memset(manifest_out, 0, sizeof(*manifest_out));
            return KB2_PROTOCOL_MALFORMED;
        }
        if (artifact.node_id <= prior_node_id) {
            memset(manifest_out, 0, sizeof(*manifest_out));
            return KB2_PROTOCOL_MALFORMED;
        }
        prior_node_id = artifact.node_id;
        root_count += (artifact.flags & KB2_CLOSURE_ARTIFACT_FLAG_ROOT) != 0;
    }
    if (root_count == 0) {
        memset(manifest_out, 0, sizeof(*manifest_out));
        return KB2_PROTOCOL_MALFORMED;
    }
    for (index = 0; index < manifest.counts[DEPENDENCIES]; ++index) {
        kb2_closure_manifest_dependency_t dependency;

        if (kb2_closure_manifest_dependency(manifest_out, index, &dependency) !=
                KB2_PROTOCOL_OK ||
            !manifest_has_node(manifest_out, dependency.consumer_node_id) ||
            !manifest_has_node(manifest_out, dependency.provider_node_id)) {
            memset(manifest_out, 0, sizeof(*manifest_out));
            return KB2_PROTOCOL_MALFORMED;
        }
    }
    for (index = 0; index < manifest.counts[EXPORTS]; ++index) {
        kb2_closure_manifest_symbol_t symbol;

        if (kb2_closure_manifest_export(manifest_out, index, &symbol) != KB2_PROTOCOL_OK) {
            memset(manifest_out, 0, sizeof(*manifest_out));
            return KB2_PROTOCOL_MALFORMED;
        }
        if (!manifest_has_node(manifest_out, symbol.node_id)) {
            memset(manifest_out, 0, sizeof(*manifest_out));
            return KB2_PROTOCOL_MALFORMED;
        }
    }
    for (index = 0; index < manifest.counts[IMPORTS]; ++index) {
        kb2_closure_manifest_import_t import_record;

        if (kb2_closure_manifest_import(manifest_out, index, &import_record) !=
                KB2_PROTOCOL_OK ||
            !manifest_has_node(manifest_out, import_record.consumer_node_id) ||
            (import_record.provider_node_id != 0 &&
             !manifest_has_node(manifest_out, import_record.provider_node_id))) {
            memset(manifest_out, 0, sizeof(*manifest_out));
            return KB2_PROTOCOL_MALFORMED;
        }
    }
    for (index = 0; index < manifest.counts[RESOURCES]; ++index) {
        kb2_closure_manifest_resource_t resource;

        if (kb2_closure_manifest_resource(manifest_out, index, &resource) != KB2_PROTOCOL_OK) {
            memset(manifest_out, 0, sizeof(*manifest_out));
            return KB2_PROTOCOL_MALFORMED;
        }
    }
    for (index = 0; index < manifest.counts[BINDINGS]; ++index) {
        kb2_closure_manifest_binding_t binding;

        if (kb2_closure_manifest_binding(manifest_out, index, &binding) != KB2_PROTOCOL_OK) {
            memset(manifest_out, 0, sizeof(*manifest_out));
            return KB2_PROTOCOL_MALFORMED;
        }
        if (!manifest_has_node(manifest_out, binding.node_id)) {
            memset(manifest_out, 0, sizeof(*manifest_out));
            return KB2_PROTOCOL_MALFORMED;
        }
    }
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_closure_manifest_artifact(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_artifact_t *artifact_out) {
    const uint8_t *record;

    if (manifest == NULL || artifact_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (index >= manifest->counts[ARTIFACTS]) {
        return KB2_PROTOCOL_MALFORMED;
    }
    record = manifest->bytes + manifest->offsets[ARTIFACTS] +
             index * KB2_CLOSURE_ARTIFACT_DESCRIPTOR_SIZE;
    memset(artifact_out, 0, sizeof(*artifact_out));
    artifact_out->node_id = load_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_NODE_ID_OFFSET);
    artifact_out->kind = load_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_KIND_OFFSET);
    artifact_out->flags = load_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_FLAGS_OFFSET);
    artifact_out->content_size =
        load_u64(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_CONTENT_SIZE_OFFSET);
    memcpy(artifact_out->content_digest,
           record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_CONTENT_DIGEST_OFFSET,
           sizeof(artifact_out->content_digest));
    if (artifact_out->node_id == 0 || artifact_out->content_size == 0 ||
        artifact_out->kind > KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE ||
        (artifact_out->flags & ~KB2_CLOSURE_ARTIFACT_FLAG_ROOT) != 0 ||
        !bytes_are_zero(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_RESERVED_0_OFFSET, 4u) ||
        !bytes_are_zero(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_RESERVED_1_OFFSET, 8u) ||
        !decoded_string(manifest,
                        load_u32(record +
                                 KB2_CLOSURE_ARTIFACT_DESCRIPTOR_NAMESPACE_OFFSET_OFFSET),
                        load_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_NAMESPACE_LENGTH_OFFSET),
                        &artifact_out->namespace_name) ||
        !decoded_string(manifest,
                        load_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_INIT_OFFSET_OFFSET),
                        load_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_INIT_LENGTH_OFFSET),
                        &artifact_out->init_symbol) ||
        !decoded_string(manifest,
                        load_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_QUIESCE_OFFSET_OFFSET),
                        load_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_QUIESCE_LENGTH_OFFSET),
                        &artifact_out->quiesce_symbol) ||
        !decoded_string(manifest,
                        load_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_CLEANUP_OFFSET_OFFSET),
                        load_u32(record + KB2_CLOSURE_ARTIFACT_DESCRIPTOR_CLEANUP_LENGTH_OFFSET),
                        &artifact_out->cleanup_symbol)) {
        memset(artifact_out, 0, sizeof(*artifact_out));
        return KB2_PROTOCOL_MALFORMED;
    }
    return KB2_PROTOCOL_OK;
}

size_t kb2_closure_manifest_artifact_count(const kb2_closure_manifest_t *manifest) {
    return manifest == NULL ? 0 : manifest->counts[ARTIFACTS];
}

size_t kb2_closure_manifest_dependency_count(const kb2_closure_manifest_t *manifest) {
    return manifest == NULL ? 0 : manifest->counts[DEPENDENCIES];
}

size_t kb2_closure_manifest_export_count(const kb2_closure_manifest_t *manifest) {
    return manifest == NULL ? 0 : manifest->counts[EXPORTS];
}

size_t kb2_closure_manifest_import_count(const kb2_closure_manifest_t *manifest) {
    return manifest == NULL ? 0 : manifest->counts[IMPORTS];
}

size_t kb2_closure_manifest_resource_count(const kb2_closure_manifest_t *manifest) {
    return manifest == NULL ? 0 : manifest->counts[RESOURCES];
}

size_t kb2_closure_manifest_binding_count(const kb2_closure_manifest_t *manifest) {
    return manifest == NULL ? 0 : manifest->counts[BINDINGS];
}

kb2_protocol_status_t kb2_closure_manifest_dependency(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_dependency_t *dependency_out) {
    const uint8_t *record;

    if (manifest == NULL || dependency_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (index >= manifest->counts[DEPENDENCIES]) {
        return KB2_PROTOCOL_MALFORMED;
    }
    record = manifest->bytes + manifest->offsets[DEPENDENCIES] +
             index * KB2_CLOSURE_DEPENDENCY_DESCRIPTOR_SIZE;
    dependency_out->consumer_node_id =
        load_u32(record + KB2_CLOSURE_DEPENDENCY_DESCRIPTOR_CONSUMER_NODE_ID_OFFSET);
    dependency_out->provider_node_id =
        load_u32(record + KB2_CLOSURE_DEPENDENCY_DESCRIPTOR_PROVIDER_NODE_ID_OFFSET);
    return dependency_out->consumer_node_id != 0 && dependency_out->provider_node_id != 0 &&
                   dependency_out->consumer_node_id != dependency_out->provider_node_id
               ? KB2_PROTOCOL_OK
               : KB2_PROTOCOL_MALFORMED;
}

kb2_protocol_status_t kb2_closure_manifest_import(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_import_t *import_out) {
    const uint8_t *record;

    if (manifest == NULL || import_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (index >= manifest->counts[IMPORTS]) {
        return KB2_PROTOCOL_MALFORMED;
    }
    record = manifest->bytes + manifest->offsets[IMPORTS] +
             index * KB2_CLOSURE_IMPORT_DESCRIPTOR_SIZE;
    memset(import_out, 0, sizeof(*import_out));
    import_out->consumer_node_id =
        load_u32(record + KB2_CLOSURE_IMPORT_DESCRIPTOR_CONSUMER_NODE_ID_OFFSET);
    import_out->provider_node_id =
        load_u32(record + KB2_CLOSURE_IMPORT_DESCRIPTOR_PROVIDER_NODE_ID_OFFSET);
    import_out->kind = load_u32(record + KB2_CLOSURE_IMPORT_DESCRIPTOR_KIND_OFFSET);
    import_out->flags = load_u32(record + KB2_CLOSURE_IMPORT_DESCRIPTOR_FLAGS_OFFSET);
    if (import_out->consumer_node_id == 0 ||
        import_out->consumer_node_id == import_out->provider_node_id ||
        import_out->kind > KB2_CLOSURE_SYMBOL_OBJECT ||
        (import_out->flags & ~KB2_CLOSURE_IMPORT_FLAG_OPTIONAL) != 0 ||
        !decoded_string(manifest,
                        load_u32(record +
                                 KB2_CLOSURE_IMPORT_DESCRIPTOR_CONSUMER_NAME_OFFSET_OFFSET),
                        load_u32(record +
                                 KB2_CLOSURE_IMPORT_DESCRIPTOR_CONSUMER_NAME_LENGTH_OFFSET),
                        &import_out->consumer_name) ||
        !decoded_string(manifest,
                        load_u32(record +
                                 KB2_CLOSURE_IMPORT_DESCRIPTOR_PROVIDER_NAME_OFFSET_OFFSET),
                        load_u32(record +
                                 KB2_CLOSURE_IMPORT_DESCRIPTOR_PROVIDER_NAME_LENGTH_OFFSET),
                        &import_out->provider_name)) {
        memset(import_out, 0, sizeof(*import_out));
        return KB2_PROTOCOL_MALFORMED;
    }
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_closure_manifest_export(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_symbol_t *export_out) {
    const uint8_t *record;

    if (manifest == NULL || export_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (index >= manifest->counts[EXPORTS]) {
        return KB2_PROTOCOL_MALFORMED;
    }
    record = manifest->bytes + manifest->offsets[EXPORTS] +
             index * KB2_CLOSURE_SYMBOL_DESCRIPTOR_SIZE;
    memset(export_out, 0, sizeof(*export_out));
    export_out->node_id = load_u32(record + KB2_CLOSURE_SYMBOL_DESCRIPTOR_NODE_ID_OFFSET);
    export_out->kind = load_u32(record + KB2_CLOSURE_SYMBOL_DESCRIPTOR_KIND_OFFSET);
    if (export_out->node_id == 0 || export_out->kind > KB2_CLOSURE_SYMBOL_OBJECT ||
        !decoded_string(manifest,
                        load_u32(record + KB2_CLOSURE_SYMBOL_DESCRIPTOR_NAME_OFFSET_OFFSET),
                        load_u32(record + KB2_CLOSURE_SYMBOL_DESCRIPTOR_NAME_LENGTH_OFFSET),
                        &export_out->name)) {
        memset(export_out, 0, sizeof(*export_out));
        return KB2_PROTOCOL_MALFORMED;
    }
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_closure_manifest_resource(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_resource_t *resource_out) {
    const uint8_t *record;

    if (manifest == NULL || resource_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (index >= manifest->counts[RESOURCES]) {
        return KB2_PROTOCOL_MALFORMED;
    }
    record = manifest->bytes + manifest->offsets[RESOURCES] +
             index * KB2_CLOSURE_RESOURCE_DESCRIPTOR_SIZE;
    memset(resource_out, 0, sizeof(*resource_out));
    resource_out->slot_id = load_u32(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_SLOT_ID_OFFSET);
    resource_out->type = load_u32(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_TYPE_OFFSET);
    resource_out->minimum_count =
        load_u32(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_MINIMUM_COUNT_OFFSET);
    resource_out->maximum_count =
        load_u32(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_MAXIMUM_COUNT_OFFSET);
    resource_out->required_rights =
        load_u64(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_REQUIRED_RIGHTS_OFFSET);
    resource_out->maximum_rights =
        load_u64(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_MAXIMUM_RIGHTS_OFFSET);
    resource_out->flags = load_u32(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_FLAGS_OFFSET);
    memcpy(resource_out->interface_schema_digest,
           record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_INTERFACE_SCHEMA_DIGEST_OFFSET,
           sizeof(resource_out->interface_schema_digest));
    if (resource_out->slot_id == 0 || resource_out->minimum_count > resource_out->maximum_count ||
        (resource_out->required_rights & ~resource_out->maximum_rights) != 0 ||
        digest_is_zero(resource_out->interface_schema_digest) ||
        !bytes_are_zero(record + KB2_CLOSURE_RESOURCE_DESCRIPTOR_RESERVED_OFFSET, 4u)) {
        memset(resource_out, 0, sizeof(*resource_out));
        return KB2_PROTOCOL_MALFORMED;
    }
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_closure_manifest_binding(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_binding_t *binding_out) {
    const uint8_t *record;

    if (manifest == NULL || binding_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (index >= manifest->counts[BINDINGS]) {
        return KB2_PROTOCOL_MALFORMED;
    }
    record = manifest->bytes + manifest->offsets[BINDINGS] +
             index * KB2_CLOSURE_RESOURCE_BINDING_SIZE;
    binding_out->slot_id = load_u32(record + KB2_CLOSURE_RESOURCE_BINDING_SLOT_ID_OFFSET);
    binding_out->node_id = load_u32(record + KB2_CLOSURE_RESOURCE_BINDING_NODE_ID_OFFSET);
    return binding_out->slot_id != 0 && binding_out->node_id != 0 ? KB2_PROTOCOL_OK
                                                                  : KB2_PROTOCOL_MALFORMED;
}
