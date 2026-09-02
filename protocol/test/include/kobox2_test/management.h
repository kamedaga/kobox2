/* SPDX-License-Identifier: MIT */

#ifndef KOBOX2_TEST_MANAGEMENT_FIXTURE_H
#define KOBOX2_TEST_MANAGEMENT_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

#define KB2_TEST_PROTOCOL_ID UINT32_C(0x74657374)
#define KB2_TEST_MESSAGE_SIZE 48u
#define KB2_TEST_TRANSPORT_BASE UINT64_C(0x100000000)
#define KB2_TEST_QUEUE_SIZE 16u
#define KB2_TEST_REGION_COUNT 5u
#define KB2_TEST_EVENT_BUFFER_OFFSET 0x3000u
#define KB2_TEST_REQUEST_BUFFER_OFFSET 0x4000u
#define KB2_TEST_RESPONSE_BUFFER_OFFSET 0x5000u
#define KB2_TEST_INDIRECT_TABLE_OFFSET 0x6000u
#define KB2_TEST_BUFFER_STRIDE 128u
#define KB2_TEST_EVENT_BUFFER_STRIDE 64u
#define KB2_TEST_MESSAGE_FLAG_RESPONSE 1u
#define KB2_TEST_MESSAGE_FLAG_EVENT 2u

#define KB2_TEST_REQUEST_ECHO 1u
#define KB2_TEST_REQUEST_QUIESCE 2u
#define KB2_TEST_REQUEST_PAUSE_AFTER_ACQUIRE 3u
#define KB2_TEST_REQUEST_PAUSE_AFTER_USED 4u
#define KB2_TEST_REQUEST_BAD_USED_ID 5u
#define KB2_TEST_REQUEST_BAD_GENERATION 6u
#define KB2_TEST_REQUEST_BAD_ENVELOPE 7u
#define KB2_TEST_REQUEST_RUN_FIXTURE 8u

#define KB2_TEST_FIXTURE_RESULT UINT64_C(0x0002000200020001)

#define KB2_TEST_EVENT_READY 0x100u
#define KB2_TEST_EVENT_ACQUIRED 0x101u
#define KB2_TEST_EVENT_USED_PUBLISHED 0x102u
#define KB2_TEST_EVENT_FAULT 0x103u
#define KB2_TEST_EVENT_STOPPED 0x104u

#define KB2_TEST_PROTOCOL_FAULT_MALFORMED_DESCRIPTOR 1u

typedef enum kb2_test_fault_scenario {
    KB2_TEST_FAULT_KILL_BEFORE_ACQUIRE = 1,
    KB2_TEST_FAULT_KILL_AFTER_ACQUIRE,
    KB2_TEST_FAULT_KILL_AFTER_USED,
    KB2_TEST_FAULT_BAD_USED_ID,
    KB2_TEST_FAULT_BAD_GENERATION,
    KB2_TEST_FAULT_BAD_ENVELOPE,
    KB2_TEST_FAULT_BAD_CHAIN,
    KB2_TEST_FAULT_BAD_LENGTH,
    KB2_TEST_FAULT_BAD_RIGHTS,
} kb2_test_fault_scenario_t;

int kb2_test_message_encode(uint8_t *buffer,
                            size_t buffer_size,
                            uint32_t opcode,
                            uint32_t flags,
                            uint64_t generation,
                            uint64_t correlation_id,
                            uint64_t value);
int kb2_test_message_decode(const uint8_t *buffer,
                            size_t buffer_size,
                            uint32_t expected_flags,
                            uint64_t expected_generation,
                            uint32_t *opcode_out,
                            uint64_t *correlation_id_out,
                            uint64_t *value_out);

#endif
