/* SPDX-License-Identifier: MIT */

#ifndef KOBOX2_TEST_SPLIT_VIRTQUEUE_H
#define KOBOX2_TEST_SPLIT_VIRTQUEUE_H

#include <kobox2/protocol.h>

#include <stddef.h>
#include <stdint.h>

#define KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT 1u
#define KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE 2u
#define KB2_TEST_VQ_DESCRIPTOR_FLAG_INDIRECT 4u

typedef enum kb2_test_vq_status {
    KB2_TEST_VQ_OK = 0,
    KB2_TEST_VQ_EMPTY,
    KB2_TEST_VQ_FULL,
    KB2_TEST_VQ_MALFORMED,
    KB2_TEST_VQ_INVALID_ARGUMENT,
} kb2_test_vq_status_t;

typedef struct kb2_test_vq_segment {
    uint8_t *data;
    uint64_t address;
    uint32_t length;
    uint16_t flags;
} kb2_test_vq_segment_t;

typedef struct kb2_test_vq {
    uint8_t *shared_memory;
    size_t shared_memory_size;
    kb2_protocol_queue_t queue;
    const kb2_protocol_region_t *regions;
    size_t region_count;
    uint64_t mapping_transport_base;
    uint8_t *descriptor_table;
    uint8_t *available_ring;
    uint8_t *used_ring;
    uint16_t available_producer_index;
    uint16_t available_consumer_index;
    uint16_t used_producer_index;
    uint16_t used_consumer_index;
    uint8_t outstanding[(KB2_PROTOCOL_QUEUE_SIZE_MAX + 7u) / 8u];
} kb2_test_vq_t;

kb2_test_vq_status_t kb2_test_vq_bind(kb2_test_vq_t *virtqueue,
                                      uint8_t *shared_memory,
                                      size_t shared_memory_size,
                                      const kb2_protocol_queue_t *queue,
                                      const kb2_protocol_region_t *regions,
                                      size_t region_count,
                                      int initialize);
kb2_test_vq_status_t kb2_test_vq_set_descriptor(kb2_test_vq_t *virtqueue,
                                                uint16_t index,
                                                uint64_t address,
                                                uint32_t length,
                                                uint16_t flags,
                                                uint16_t next);
kb2_test_vq_status_t kb2_test_vq_set_indirect_descriptor(kb2_test_vq_t *virtqueue,
                                                         uint64_t table_address,
                                                         uint16_t table_index,
                                                         uint16_t table_count,
                                                         uint64_t address,
                                                         uint32_t length,
                                                         uint16_t flags,
                                                         uint16_t next);
kb2_test_vq_status_t kb2_test_vq_publish(kb2_test_vq_t *virtqueue,
                                         uint16_t head,
                                         int *notify_out);
kb2_test_vq_status_t kb2_test_vq_take_available(kb2_test_vq_t *virtqueue,
                                                uint16_t *head_out);
kb2_test_vq_status_t kb2_test_vq_read_chain(kb2_test_vq_t *virtqueue,
                                            uint16_t head,
                                            kb2_test_vq_segment_t *segments_out,
                                            size_t segment_capacity,
                                            size_t *segment_count_out);
kb2_test_vq_status_t kb2_test_vq_complete(kb2_test_vq_t *virtqueue,
                                          uint16_t head,
                                          uint32_t length,
                                          int *notify_out);
kb2_test_vq_status_t kb2_test_vq_inject_used_id(kb2_test_vq_t *virtqueue,
                                                uint32_t head,
                                                uint32_t length,
                                                int *notify_out);
kb2_test_vq_status_t kb2_test_vq_take_used(kb2_test_vq_t *virtqueue,
                                           uint16_t *head_out,
                                           uint32_t *length_out);

#endif
