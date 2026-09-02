/* SPDX-License-Identifier: MIT */

#include <kobox2_test/management.h>

#include <kobox2/protocol.h>

static void kb2_test_store_u64(uint8_t *destination, uint64_t value) {
    size_t index;

    for (index = 0; index < 8; ++index) {
        destination[index] = (uint8_t)(value >> (index * 8u));
    }
}

static uint64_t kb2_test_load_u64(const uint8_t *source) {
    uint64_t value = 0;
    size_t index;

    for (index = 0; index < 8; ++index) {
        value |= (uint64_t)source[index] << (index * 8u);
    }
    return value;
}

int kb2_test_message_encode(uint8_t *buffer,
                            size_t buffer_size,
                            uint32_t opcode,
                            uint32_t flags,
                            uint64_t generation,
                            uint64_t correlation_id,
                            uint64_t value) {
    kb2_protocol_message_envelope_t envelope = {
        .protocol_id = KB2_TEST_PROTOCOL_ID,
        .opcode = opcode,
        .flags = flags,
        .generation = generation,
        .correlation_id = correlation_id,
        .payload_length = 8,
    };

    if (buffer == NULL || buffer_size < KB2_TEST_MESSAGE_SIZE ||
        kb2_protocol_message_envelope_encode(buffer, buffer_size, &envelope) !=
            KB2_PROTOCOL_OK) {
        return 0;
    }
    kb2_test_store_u64(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE, value);
    return 1;
}

int kb2_test_message_decode(const uint8_t *buffer,
                            size_t buffer_size,
                            uint32_t expected_flags,
                            uint64_t expected_generation,
                            uint32_t *opcode_out,
                            uint64_t *correlation_id_out,
                            uint64_t *value_out) {
    kb2_protocol_message_envelope_t envelope;

    if (buffer == NULL || opcode_out == NULL || correlation_id_out == NULL ||
        value_out == NULL || buffer_size != KB2_TEST_MESSAGE_SIZE ||
        kb2_protocol_message_envelope_decode(buffer, buffer_size, &envelope) !=
            KB2_PROTOCOL_OK ||
        envelope.protocol_id != KB2_TEST_PROTOCOL_ID || envelope.flags != expected_flags ||
        envelope.generation != expected_generation || envelope.payload_length != 8) {
        return 0;
    }
    *opcode_out = envelope.opcode;
    *correlation_id_out = envelope.correlation_id;
    *value_out = kb2_test_load_u64(buffer + KB2_PROTOCOL_MESSAGE_ENVELOPE_SIZE);
    return 1;
}
