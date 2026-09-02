/* SPDX-License-Identifier: Apache-2.0 */

#include <kobox2_test/split_virtqueue.h>

#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define TEST_BASE UINT64_C(0x100000000)

static void make_queue(kb2_protocol_queue_t *queue) {
    memset(queue, 0, sizeof(*queue));
    queue->queue_id = 1;
    queue->role = KB2_PROTOCOL_QUEUE_ROLE_REQUEST;
    queue->queue_size = 16;
    queue->descriptor_address = TEST_BASE + 0x1000;
    queue->available_address = TEST_BASE + 0x1100;
    queue->used_address = TEST_BASE + 0x1140;
    queue->available_notification_id = 1;
    queue->used_notification_id = 2;
    queue->max_chain_length = 16;
    queue->max_indirect_length = 16;
    queue->max_outstanding = 16;
}

static int test_chains(void) {
    uint8_t memory[65536] = {0};
    kb2_protocol_queue_t queue;
    kb2_protocol_region_t region = {
        .region_id = 1,
        .rights = KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE,
        .transport_base = TEST_BASE,
        .length = sizeof(memory),
    };
    kb2_test_vq_t virtqueue;
    kb2_test_vq_segment_t segments[2];
    size_t segment_count;

    make_queue(&queue);
    CHECK(kb2_test_vq_bind(&virtqueue, memory, sizeof(memory), &queue, &region, 1, 1) ==
          KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_set_descriptor(&virtqueue,
                                     0,
                                     TEST_BASE + 0x2000,
                                     32,
                                     KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT,
                                     1) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_set_descriptor(&virtqueue,
                                     1,
                                     TEST_BASE + 0x2100,
                                     32,
                                     KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE,
                                     0) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_read_chain(&virtqueue, 0, segments, 2, &segment_count) ==
          KB2_TEST_VQ_OK);
    CHECK(segment_count == 2);
    CHECK((segments[0].flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE) == 0);
    CHECK((segments[1].flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE) != 0);

    CHECK(kb2_test_vq_set_indirect_descriptor(&virtqueue,
                                              TEST_BASE + 0x3000,
                                              0,
                                              2,
                                              TEST_BASE + 0x4000,
                                              32,
                                              KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT,
                                              1) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_set_indirect_descriptor(&virtqueue,
                                              TEST_BASE + 0x3000,
                                              1,
                                              2,
                                              TEST_BASE + 0x4100,
                                              32,
                                              KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE,
                                              0) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_set_descriptor(&virtqueue,
                                     2,
                                     TEST_BASE + 0x3000,
                                     32,
                                     KB2_TEST_VQ_DESCRIPTOR_FLAG_INDIRECT,
                                     0) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_read_chain(&virtqueue, 2, segments, 2, &segment_count) ==
          KB2_TEST_VQ_OK);
    CHECK(segment_count == 2 && segments[0].address == TEST_BASE + 0x4000 &&
          segments[1].address == TEST_BASE + 0x4100);

    CHECK(kb2_test_vq_set_descriptor(&virtqueue,
                                     3,
                                     TEST_BASE + 0x4200,
                                     32,
                                     KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT,
                                     3) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_read_chain(&virtqueue, 3, segments, 2, &segment_count) ==
          KB2_TEST_VQ_MALFORMED);
    CHECK(kb2_test_vq_set_descriptor(
              &virtqueue, 4, TEST_BASE + sizeof(memory), 1, 0, 0) ==
          KB2_TEST_VQ_MALFORMED);
    return 0;
}

static int test_indices_and_notifications(void) {
    uint8_t memory[65536] = {0};
    kb2_protocol_queue_t queue;
    kb2_protocol_region_t region = {
        .region_id = 1,
        .rights = KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE,
        .transport_base = TEST_BASE,
        .length = sizeof(memory),
    };
    kb2_test_vq_t virtqueue;
    uint32_t length;
    uint32_t iteration;
    uint16_t head;
    int notify;

    make_queue(&queue);
    CHECK(kb2_test_vq_bind(&virtqueue, memory, sizeof(memory), &queue, &region, 1, 1) ==
          KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_set_descriptor(&virtqueue, 0, TEST_BASE + 0x2000, 32, 0, 0) ==
          KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_set_descriptor(&virtqueue, 1, TEST_BASE + 0x2100, 32, 0, 0) ==
          KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_publish(&virtqueue, 0, &notify) == KB2_TEST_VQ_OK && notify);
    CHECK(kb2_test_vq_publish(&virtqueue, 1, &notify) == KB2_TEST_VQ_OK && !notify);
    CHECK(kb2_test_vq_take_available(&virtqueue, &head) == KB2_TEST_VQ_OK && head == 0);
    CHECK(kb2_test_vq_take_available(&virtqueue, &head) == KB2_TEST_VQ_OK && head == 1);
    CHECK(kb2_test_vq_complete(&virtqueue, 1, 8, &notify) == KB2_TEST_VQ_OK && notify);
    CHECK(kb2_test_vq_complete(&virtqueue, 0, 8, &notify) == KB2_TEST_VQ_OK && !notify);
    CHECK(kb2_test_vq_take_used(&virtqueue, &head, &length) == KB2_TEST_VQ_OK && head == 1 &&
          length == 8);
    CHECK(kb2_test_vq_take_used(&virtqueue, &head, &length) == KB2_TEST_VQ_OK && head == 0 &&
          length == 8);

    for (iteration = 0; iteration < 70000; ++iteration) {
        CHECK(kb2_test_vq_publish(&virtqueue, 0, &notify) == KB2_TEST_VQ_OK);
        CHECK(kb2_test_vq_take_available(&virtqueue, &head) == KB2_TEST_VQ_OK && head == 0);
        CHECK(kb2_test_vq_complete(&virtqueue, head, iteration, &notify) == KB2_TEST_VQ_OK);
        CHECK(kb2_test_vq_take_used(&virtqueue, &head, &length) == KB2_TEST_VQ_OK &&
              head == 0 && length == iteration);
    }
    CHECK(virtqueue.available_producer_index == (uint16_t)(70002u));
    CHECK(virtqueue.used_consumer_index == (uint16_t)(70002u));
    return 0;
}

static int test_region_rights(void) {
    uint8_t memory[65536] = {0};
    kb2_protocol_queue_t queue;
    kb2_protocol_region_t regions[3] = {
        {
            .region_id = 1,
            .rights = KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE,
            .transport_base = TEST_BASE,
            .length = 0x2000,
        },
        {
            .region_id = 2,
            .rights = KB2_PROTOCOL_REGION_RIGHT_READ,
            .transport_base = TEST_BASE + 0x2000,
            .length = 0x1000,
        },
        {
            .region_id = 3,
            .rights = KB2_PROTOCOL_REGION_RIGHT_WRITE,
            .transport_base = TEST_BASE + 0x3000,
            .length = 0x1000,
        },
    };
    kb2_test_vq_t virtqueue;

    make_queue(&queue);
    CHECK(kb2_test_vq_bind(&virtqueue, memory, sizeof(memory), &queue, regions, 3, 1) ==
          KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_set_descriptor(
              &virtqueue, 0, TEST_BASE + 0x2000, 32, 0, 0) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_set_descriptor(&virtqueue,
                                     0,
                                     TEST_BASE + 0x2000,
                                     32,
                                     KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE,
                                     0) == KB2_TEST_VQ_MALFORMED);
    CHECK(kb2_test_vq_set_descriptor(&virtqueue,
                                     0,
                                     TEST_BASE + 0x3000,
                                     32,
                                     KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE,
                                     0) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_set_descriptor(
              &virtqueue, 0, TEST_BASE + 0x3000, 32, 0, 0) == KB2_TEST_VQ_MALFORMED);
    CHECK(kb2_test_vq_set_descriptor(
              &virtqueue, 0, TEST_BASE + 0x2ff0, 32, 0, 0) == KB2_TEST_VQ_MALFORMED);
    return 0;
}

static int test_completion_id_validation(void) {
    uint8_t memory[65536] = {0};
    kb2_protocol_queue_t queue;
    kb2_protocol_region_t region = {
        .region_id = 1,
        .rights = KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE,
        .transport_base = TEST_BASE,
        .length = sizeof(memory),
    };
    kb2_test_vq_t virtqueue;
    uint32_t length;
    uint16_t head;
    int notify;

    make_queue(&queue);
    CHECK(kb2_test_vq_bind(&virtqueue, memory, sizeof(memory), &queue, &region, 1, 1) ==
          KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_publish(&virtqueue, 0, &notify) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_publish(&virtqueue, 1, &notify) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_take_available(&virtqueue, &head) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_take_available(&virtqueue, &head) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_inject_used_id(&virtqueue, 0, 8, &notify) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_inject_used_id(&virtqueue, 0, 8, &notify) == KB2_TEST_VQ_OK);
    CHECK(kb2_test_vq_take_used(&virtqueue, &head, &length) == KB2_TEST_VQ_OK && head == 0);
    CHECK(kb2_test_vq_take_used(&virtqueue, &head, &length) == KB2_TEST_VQ_MALFORMED);
    return 0;
}

int main(void) {
    CHECK(test_chains() == 0);
    CHECK(test_indices_and_notifications() == 0);
    CHECK(test_region_rights() == 0);
    CHECK(test_completion_id_validation() == 0);
    return 0;
}
