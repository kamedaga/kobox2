/* SPDX-License-Identifier: Apache-2.0 */

#define _GNU_SOURCE

#include "linux_host_adapter.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#define KB2_TEST_IO_TIMEOUT_MILLISECONDS 5000

static int kb2_test_eventfd_write(int file_descriptor) {
    uint64_t value = 1;
    ssize_t bytes;

    do {
        bytes = write(file_descriptor, &value, sizeof(value));
    } while (bytes < 0 && errno == EINTR);
    return bytes == (ssize_t)sizeof(value);
}

static int kb2_test_eventfd_wait(int file_descriptor) {
    struct pollfd descriptor = {
        .fd = file_descriptor,
        .events = POLLIN,
    };
    uint64_t value;
    ssize_t bytes;
    int result;

    do {
        result = poll(&descriptor, 1, KB2_TEST_IO_TIMEOUT_MILLISECONDS);
    } while (result < 0 && errno == EINTR);
    if (result != 1 || (descriptor.revents & POLLIN) == 0) {
        return 0;
    }
    do {
        bytes = read(file_descriptor, &value, sizeof(value));
    } while (bytes < 0 && errno == EINTR);
    return bytes == (ssize_t)sizeof(value) && value != 0;
}

static uint64_t kb2_test_slot_address(uint32_t offset, uint16_t slot, uint32_t stride) {
    return KB2_TEST_TRANSPORT_BASE + offset + (uint64_t)slot * stride;
}

static int kb2_test_prepare_indirect_request(kb2_test_host_t *host,
                                             uint16_t slot,
                                             uint32_t opcode,
                                             uint64_t value,
                                             uint64_t *correlation_out) {
    uint64_t request_address = kb2_test_slot_address(
        KB2_TEST_REQUEST_BUFFER_OFFSET, slot, KB2_TEST_BUFFER_STRIDE);
    uint64_t response_address = kb2_test_slot_address(
        KB2_TEST_RESPONSE_BUFFER_OFFSET, slot, KB2_TEST_BUFFER_STRIDE);
    uint64_t table_address = kb2_test_slot_address(
        KB2_TEST_INDIRECT_TABLE_OFFSET, slot, 2u * 16u);
    size_t request_offset = (size_t)(request_address - KB2_TEST_TRANSPORT_BASE);
    uint64_t correlation;

    if (host->next_correlation_id == UINT64_MAX) {
        return 0;
    }
    correlation = ++host->next_correlation_id;
    if (!kb2_test_message_encode((uint8_t *)host->shared_memory + request_offset,
                                 KB2_TEST_MESSAGE_SIZE,
                                 opcode,
                                 0,
                                 host->generation,
                                 correlation,
                                 value) ||
        kb2_test_vq_set_indirect_descriptor(&host->request_queue,
                                            table_address,
                                            0,
                                            2,
                                            request_address,
                                            KB2_TEST_MESSAGE_SIZE,
                                            KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT,
                                            1) != KB2_TEST_VQ_OK ||
        kb2_test_vq_set_indirect_descriptor(&host->request_queue,
                                            table_address,
                                            1,
                                            2,
                                            response_address,
                                            KB2_TEST_MESSAGE_SIZE,
                                            KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE,
                                            0) != KB2_TEST_VQ_OK ||
        kb2_test_vq_set_descriptor(&host->request_queue,
                                   slot,
                                   table_address,
                                   2u * 16u,
                                   KB2_TEST_VQ_DESCRIPTOR_FLAG_INDIRECT,
                                   0) != KB2_TEST_VQ_OK) {
        return 0;
    }
    *correlation_out = correlation;
    return 1;
}

static int kb2_test_prepare_direct_request(kb2_test_host_t *host,
                                           uint32_t opcode,
                                           uint64_t value,
                                           uint64_t *correlation_out) {
    uint64_t request_address =
        kb2_test_slot_address(KB2_TEST_REQUEST_BUFFER_OFFSET, 0, KB2_TEST_BUFFER_STRIDE);
    uint64_t response_address =
        kb2_test_slot_address(KB2_TEST_RESPONSE_BUFFER_OFFSET, 0, KB2_TEST_BUFFER_STRIDE);
    size_t request_offset = (size_t)(request_address - KB2_TEST_TRANSPORT_BASE);
    uint64_t correlation;

    if (host->next_correlation_id == UINT64_MAX) {
        return 0;
    }
    correlation = ++host->next_correlation_id;
    if (!kb2_test_message_encode((uint8_t *)host->shared_memory + request_offset,
                                 KB2_TEST_MESSAGE_SIZE,
                                 opcode,
                                 0,
                                 host->generation,
                                 correlation,
                                 value) ||
        kb2_test_vq_set_descriptor(&host->request_queue,
                                   0,
                                   request_address,
                                   KB2_TEST_MESSAGE_SIZE,
                                   KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT,
                                   1) != KB2_TEST_VQ_OK ||
        kb2_test_vq_set_descriptor(&host->request_queue,
                                   1,
                                   response_address,
                                   KB2_TEST_MESSAGE_SIZE,
                                   KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE,
                                   0) != KB2_TEST_VQ_OK) {
        return 0;
    }
    *correlation_out = correlation;
    return 1;
}

static int kb2_test_prepare_malformed_request(kb2_test_host_t *host,
                                              kb2_test_fault_scenario_t scenario,
                                              uint64_t *correlation_out) {
    uint64_t request_address = kb2_test_slot_address(
        KB2_TEST_REQUEST_BUFFER_OFFSET, 0, KB2_TEST_BUFFER_STRIDE);
    uint64_t response_address = kb2_test_slot_address(
        KB2_TEST_RESPONSE_BUFFER_OFFSET, 0, KB2_TEST_BUFFER_STRIDE);
    uint64_t message_address = scenario == KB2_TEST_FAULT_BAD_RIGHTS ? response_address
                                                                    : request_address;
    size_t message_offset = (size_t)(message_address - KB2_TEST_TRANSPORT_BASE);
    uint16_t first_flags = scenario == KB2_TEST_FAULT_BAD_RIGHTS
                               ? KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE |
                                     KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT
                               : KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT;
    uint32_t first_length = scenario == KB2_TEST_FAULT_BAD_LENGTH
                                ? KB2_TEST_MESSAGE_SIZE - 1u
                                : KB2_TEST_MESSAGE_SIZE;
    uint64_t correlation;

    if (host->next_correlation_id == UINT64_MAX) {
        return 0;
    }
    correlation = ++host->next_correlation_id;
    if (!kb2_test_message_encode((uint8_t *)host->shared_memory + message_offset,
                                 KB2_TEST_MESSAGE_SIZE,
                                 KB2_TEST_REQUEST_ECHO,
                                 0,
                                 host->generation,
                                 correlation,
                                 scenario) ||
        kb2_test_vq_set_descriptor(&host->request_queue,
                                   0,
                                   message_address,
                                   first_length,
                                   scenario == KB2_TEST_FAULT_BAD_CHAIN ? 0 : first_flags,
                                   1) != KB2_TEST_VQ_OK ||
        (scenario != KB2_TEST_FAULT_BAD_CHAIN &&
         kb2_test_vq_set_descriptor(&host->request_queue,
                                    1,
                                    response_address,
                                    KB2_TEST_MESSAGE_SIZE,
                                    KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE,
                                    0) != KB2_TEST_VQ_OK)) {
        return 0;
    }
    *correlation_out = correlation;
    return 1;
}

static int kb2_test_publish_request(kb2_test_host_t *host, uint16_t head, int signal) {
    int notify;

    if (kb2_test_vq_publish(&host->request_queue, head, &notify) != KB2_TEST_VQ_OK) {
        return 0;
    }
    return !signal || !notify || kb2_test_eventfd_write(host->notification_fds[2]);
}

static int kb2_test_receive_response(kb2_test_host_t *host,
                                     uint16_t expected_head,
                                     uint32_t expected_opcode,
                                     uint64_t expected_correlation,
                                     uint64_t expected_value) {
    uint64_t response_address = kb2_test_slot_address(
        KB2_TEST_RESPONSE_BUFFER_OFFSET, expected_head, KB2_TEST_BUFFER_STRIDE);
    size_t response_offset = (size_t)(response_address - KB2_TEST_TRANSPORT_BASE);
    uint64_t correlation;
    uint64_t value;
    uint32_t opcode;
    uint32_t length;
    uint16_t head;

    if (kb2_test_vq_take_used(&host->request_queue, &head, &length) != KB2_TEST_VQ_OK ||
        head != expected_head || length != KB2_TEST_MESSAGE_SIZE ||
        !kb2_test_message_decode((const uint8_t *)host->shared_memory + response_offset,
                                 length,
                                 KB2_TEST_MESSAGE_FLAG_RESPONSE,
                                 host->generation,
                                 &opcode,
                                 &correlation,
                                 &value) ||
        opcode != expected_opcode || correlation != expected_correlation ||
        value != expected_value) {
        return 0;
    }
    return 1;
}

static int kb2_test_receive_event(kb2_test_host_t *host,
                                  uint32_t expected_opcode,
                                  uint64_t *value_out) {
    uint64_t event_address;
    uint64_t correlation;
    uint64_t value;
    uint32_t opcode;
    uint32_t length;
    uint16_t head;
    int notify;

    if (!kb2_test_eventfd_wait(host->notification_fds[1]) ||
        kb2_test_vq_take_used(&host->event_queue, &head, &length) != KB2_TEST_VQ_OK ||
        length != KB2_TEST_MESSAGE_SIZE) {
        return 0;
    }
    event_address = kb2_test_slot_address(
        KB2_TEST_EVENT_BUFFER_OFFSET, head, KB2_TEST_EVENT_BUFFER_STRIDE);
    if (!kb2_test_message_decode(
            (const uint8_t *)host->shared_memory +
                (size_t)(event_address - KB2_TEST_TRANSPORT_BASE),
            length,
            KB2_TEST_MESSAGE_FLAG_EVENT,
            host->generation,
            &opcode,
            &correlation,
            &value) ||
        opcode != expected_opcode || correlation != 0 ||
        kb2_test_vq_publish(&host->event_queue, head, &notify) != KB2_TEST_VQ_OK ||
        (notify && !kb2_test_eventfd_write(host->notification_fds[0]))) {
        return 0;
    }
    *value_out = value;
    return 1;
}

int kb2_test_host_echo(kb2_test_host_t *host, uint64_t value, int indirect) {
    uint64_t correlation;
    uint16_t head = 0;

    if (host == NULL || !host->resources_transferred || host->resources_revoked ||
        (indirect ? !kb2_test_prepare_indirect_request(
                        host, head, KB2_TEST_REQUEST_ECHO, value, &correlation)
                  : !kb2_test_prepare_direct_request(
                        host, KB2_TEST_REQUEST_ECHO, value, &correlation)) ||
        !kb2_test_publish_request(host, head, 1) ||
        !kb2_test_eventfd_wait(host->notification_fds[3]) ||
        !kb2_test_receive_response(
            host, head, KB2_TEST_REQUEST_ECHO, correlation, value)) {
        return 0;
    }
    return 1;
}

int kb2_test_host_run_fixture(kb2_test_host_t *host) {
    uint64_t correlation;

    if (host == NULL || !host->resources_transferred || host->resources_revoked ||
        !kb2_test_prepare_indirect_request(
            host, 0, KB2_TEST_REQUEST_RUN_FIXTURE, 0, &correlation) ||
        !kb2_test_publish_request(host, 0, 1) ||
        !kb2_test_eventfd_wait(host->notification_fds[3]) ||
        !kb2_test_receive_response(host,
                                   0,
                                   KB2_TEST_REQUEST_RUN_FIXTURE,
                                   correlation,
                                   KB2_TEST_FIXTURE_RESULT)) {
        return 0;
    }
    return 1;
}

int kb2_test_host_echo_batch(kb2_test_host_t *host, uint64_t first_value, size_t count) {
    uint64_t correlations[KB2_TEST_QUEUE_SIZE];
    size_t index;
    int any_notify = 0;

    if (host == NULL || count == 0 || count > KB2_TEST_QUEUE_SIZE ||
        !host->resources_transferred || host->resources_revoked) {
        return 0;
    }
    for (index = 0; index < count; ++index) {
        int notify;

        if (!kb2_test_prepare_indirect_request(host,
                                               (uint16_t)index,
                                               KB2_TEST_REQUEST_ECHO,
                                               first_value + index,
                                               &correlations[index]) ||
            kb2_test_vq_publish(&host->request_queue, (uint16_t)index, &notify) !=
                KB2_TEST_VQ_OK) {
            return 0;
        }
        any_notify |= notify;
    }
    if ((any_notify && !kb2_test_eventfd_write(host->notification_fds[2])) ||
        !kb2_test_eventfd_wait(host->notification_fds[3])) {
        return 0;
    }
    for (index = 0; index < count; ++index) {
        if (!kb2_test_receive_response(host,
                                       (uint16_t)index,
                                       KB2_TEST_REQUEST_ECHO,
                                       correlations[index],
                                       first_value + index)) {
            return 0;
        }
    }
    return 1;
}

int kb2_test_host_request_quiesce(kb2_test_host_t *host) {
    uint64_t correlation;
    uint64_t event_value;

    return kb2_test_prepare_indirect_request(
               host, 0, KB2_TEST_REQUEST_QUIESCE, 0, &correlation) &&
           kb2_test_publish_request(host, 0, 1) &&
           kb2_test_eventfd_wait(host->notification_fds[3]) &&
           kb2_test_receive_response(
               host, 0, KB2_TEST_REQUEST_QUIESCE, correlation, 0) &&
           kb2_test_receive_event(host, KB2_TEST_EVENT_STOPPED, &event_value) &&
           event_value == 0;
}

int kb2_test_host_receive_ready(kb2_test_host_t *host) {
    uint64_t value;

    return kb2_test_receive_event(host, KB2_TEST_EVENT_READY, &value) &&
           value == host->generation;
}

static int kb2_test_kill(kb2_test_host_t *host) {
    if (kill(host->process_id, SIGKILL) != 0) {
        return 0;
    }
    host->abnormal_exit_allowed = 1;
    return 1;
}

int kb2_test_host_inject_fault(kb2_test_host_t *host,
                               kb2_test_fault_scenario_t scenario,
                               kb2_fault_kind_t *fault_kind_out,
                               uint64_t *fault_code_out) {
    uint32_t request_opcode;
    uint32_t length;
    uint16_t head;
    uint64_t correlation;
    uint64_t event_value;
    int signal = 1;

    if (host == NULL || fault_kind_out == NULL || fault_code_out == NULL ||
        !host->resources_transferred || host->resources_revoked) {
        return 0;
    }
    switch (scenario) {
    case KB2_TEST_FAULT_KILL_BEFORE_ACQUIRE:
        request_opcode = KB2_TEST_REQUEST_PAUSE_AFTER_ACQUIRE;
        signal = 0;
        break;
    case KB2_TEST_FAULT_KILL_AFTER_ACQUIRE:
        request_opcode = KB2_TEST_REQUEST_PAUSE_AFTER_ACQUIRE;
        break;
    case KB2_TEST_FAULT_KILL_AFTER_USED:
        request_opcode = KB2_TEST_REQUEST_PAUSE_AFTER_USED;
        break;
    case KB2_TEST_FAULT_BAD_USED_ID:
        request_opcode = KB2_TEST_REQUEST_BAD_USED_ID;
        break;
    case KB2_TEST_FAULT_BAD_GENERATION:
        request_opcode = KB2_TEST_REQUEST_BAD_GENERATION;
        break;
    case KB2_TEST_FAULT_BAD_ENVELOPE:
        request_opcode = KB2_TEST_REQUEST_BAD_ENVELOPE;
        break;
    case KB2_TEST_FAULT_BAD_CHAIN:
    case KB2_TEST_FAULT_BAD_LENGTH:
    case KB2_TEST_FAULT_BAD_RIGHTS:
        request_opcode = 0;
        break;
    default:
        return 0;
    }
    if (((scenario == KB2_TEST_FAULT_BAD_CHAIN ||
          scenario == KB2_TEST_FAULT_BAD_LENGTH ||
          scenario == KB2_TEST_FAULT_BAD_RIGHTS)
             ? !kb2_test_prepare_malformed_request(host, scenario, &correlation)
             : !kb2_test_prepare_indirect_request(
                   host, 0, request_opcode, scenario, &correlation)) ||
        !kb2_test_publish_request(host, 0, signal)) {
        return 0;
    }
    if (scenario == KB2_TEST_FAULT_KILL_BEFORE_ACQUIRE) {
        if (!kb2_test_kill(host)) {
            return 0;
        }
        *fault_kind_out = KB2_FAULT_PROCESS_EXIT;
    } else if (scenario == KB2_TEST_FAULT_KILL_AFTER_ACQUIRE) {
        if (!kb2_test_receive_event(host, KB2_TEST_EVENT_ACQUIRED, &event_value) ||
            event_value != correlation || !kb2_test_kill(host)) {
            return 0;
        }
        *fault_kind_out = KB2_FAULT_PROCESS_EXIT;
    } else if (scenario == KB2_TEST_FAULT_KILL_AFTER_USED) {
        if (!kb2_test_receive_event(host, KB2_TEST_EVENT_USED_PUBLISHED, &event_value) ||
            event_value != correlation || !kb2_test_kill(host)) {
            return 0;
        }
        *fault_kind_out = KB2_FAULT_PROCESS_EXIT;
    } else if (scenario == KB2_TEST_FAULT_BAD_USED_ID) {
        if (!kb2_test_eventfd_wait(host->notification_fds[3]) ||
            kb2_test_vq_take_used(&host->request_queue, &head, &length) !=
                KB2_TEST_VQ_MALFORMED) {
            return 0;
        }
        host->abnormal_exit_allowed = 1;
        *fault_kind_out = KB2_FAULT_PROTOCOL;
    } else if (scenario == KB2_TEST_FAULT_BAD_GENERATION ||
               scenario == KB2_TEST_FAULT_BAD_ENVELOPE) {
        if (!kb2_test_eventfd_wait(host->notification_fds[3])) {
            return 0;
        }
        if (kb2_test_receive_response(
                host, 0, request_opcode, correlation, scenario)) {
            return 0;
        }
        host->abnormal_exit_allowed = 1;
        *fault_kind_out = KB2_FAULT_PROTOCOL;
    } else {
        if (!kb2_test_receive_event(host, KB2_TEST_EVENT_FAULT, &event_value) ||
            event_value != KB2_TEST_PROTOCOL_FAULT_MALFORMED_DESCRIPTOR) {
            return 0;
        }
        host->abnormal_exit_allowed = 1;
        *fault_kind_out = KB2_FAULT_PROTOCOL;
    }
    *fault_code_out = (uint64_t)scenario;
    return 1;
}
