/* SPDX-License-Identifier: MIT */
#include <kobox2/block_device.h>

#include <string.h>

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}

static void put64(uint8_t *p, uint64_t value) {
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static int valid_opcode(uint32_t opcode) {
    return opcode >= KB2_BLOCK_DEVICE_OP_SNAPSHOT &&
           opcode <= KB2_BLOCK_DEVICE_OP_FLUSH;
}

static int name_terminated(const char *name) {
    for (size_t i = 0; i < 32; ++i)
        if (!name[i]) return 1;
    return 0;
}

int kb2_block_header_encode(uint8_t *bytes, size_t capacity,
                            const kb2_block_header_t *header) {
    static const uint8_t identity[4] = KB2_BLOCK_DEVICE_ABI_IDENTITY_BYTES;
    if (!bytes || !header || capacity < KB2_BLOCK_DEVICE_HEADER_SIZE ||
        !valid_opcode(header->opcode) ||
        header->status > KB2_BLOCK_DEVICE_STATUS_UNSUPPORTED ||
        header->count > KB2_BLOCK_DEVICE_MAX_DISKS ||
        header->length > KB2_BLOCK_DEVICE_MAX_TRANSFER ||
        !header->generation || !header->sequence)
        return -1;
    memset(bytes, 0, KB2_BLOCK_DEVICE_HEADER_SIZE);
    memcpy(bytes, identity, sizeof(identity));
    put32(bytes + KB2_BLOCK_DEVICE_HEADER_OPCODE_OFFSET, header->opcode);
    put32(bytes + KB2_BLOCK_DEVICE_HEADER_STATUS_OFFSET, header->status);
    put32(bytes + KB2_BLOCK_DEVICE_HEADER_COUNT_OFFSET, header->count);
    put64(bytes + KB2_BLOCK_DEVICE_HEADER_GENERATION_OFFSET, header->generation);
    put64(bytes + KB2_BLOCK_DEVICE_HEADER_SEQUENCE_OFFSET, header->sequence);
    put64(bytes + KB2_BLOCK_DEVICE_HEADER_DISKSEQ_OFFSET, header->diskseq);
    put64(bytes + KB2_BLOCK_DEVICE_HEADER_OFFSET_OFFSET, header->offset);
    put32(bytes + KB2_BLOCK_DEVICE_HEADER_MAJOR_OFFSET, header->major);
    put32(bytes + KB2_BLOCK_DEVICE_HEADER_MINOR_OFFSET, header->minor);
    put32(bytes + KB2_BLOCK_DEVICE_HEADER_LENGTH_OFFSET, header->length);
    return 0;
}

int kb2_block_header_decode(const uint8_t *bytes, size_t size,
                            kb2_block_header_t *header) {
    static const uint8_t identity[4] = KB2_BLOCK_DEVICE_ABI_IDENTITY_BYTES;
    kb2_block_header_t value;
    if (!bytes || !header || size < KB2_BLOCK_DEVICE_HEADER_SIZE ||
        memcmp(bytes, identity, sizeof(identity)) ||
        get32(bytes + KB2_BLOCK_DEVICE_HEADER_RESERVED_OFFSET))
        return -1;
    memset(&value, 0, sizeof(value));
    value.opcode = get32(bytes + KB2_BLOCK_DEVICE_HEADER_OPCODE_OFFSET);
    value.status = get32(bytes + KB2_BLOCK_DEVICE_HEADER_STATUS_OFFSET);
    value.count = get32(bytes + KB2_BLOCK_DEVICE_HEADER_COUNT_OFFSET);
    value.generation = get64(bytes + KB2_BLOCK_DEVICE_HEADER_GENERATION_OFFSET);
    value.sequence = get64(bytes + KB2_BLOCK_DEVICE_HEADER_SEQUENCE_OFFSET);
    value.diskseq = get64(bytes + KB2_BLOCK_DEVICE_HEADER_DISKSEQ_OFFSET);
    value.offset = get64(bytes + KB2_BLOCK_DEVICE_HEADER_OFFSET_OFFSET);
    value.major = get32(bytes + KB2_BLOCK_DEVICE_HEADER_MAJOR_OFFSET);
    value.minor = get32(bytes + KB2_BLOCK_DEVICE_HEADER_MINOR_OFFSET);
    value.length = get32(bytes + KB2_BLOCK_DEVICE_HEADER_LENGTH_OFFSET);
    if (!valid_opcode(value.opcode) ||
        value.status > KB2_BLOCK_DEVICE_STATUS_UNSUPPORTED ||
        value.count > KB2_BLOCK_DEVICE_MAX_DISKS ||
        value.length > KB2_BLOCK_DEVICE_MAX_TRANSFER ||
        !value.generation || !value.sequence)
        return -1;
    *header = value;
    return 0;
}

int kb2_block_disk_encode(uint8_t *bytes, size_t capacity,
                          const kb2_block_disk_t *disk) {
    if (!bytes || !disk || capacity < KB2_BLOCK_DEVICE_DISK_SIZE ||
        !disk->diskseq || !disk->bytes || !disk->logical_block_size ||
        !disk->physical_block_size || disk->removable > 1 ||
        disk->read_only > 1 || !name_terminated(disk->name))
        return -1;
    memset(bytes, 0, KB2_BLOCK_DEVICE_DISK_SIZE);
    put64(bytes + KB2_BLOCK_DEVICE_DISK_DISKSEQ_OFFSET, disk->diskseq);
    put64(bytes + KB2_BLOCK_DEVICE_DISK_BYTES_OFFSET, disk->bytes);
    put32(bytes + KB2_BLOCK_DEVICE_DISK_MAJOR_OFFSET, disk->major);
    put32(bytes + KB2_BLOCK_DEVICE_DISK_MINOR_OFFSET, disk->minor);
    put32(bytes + KB2_BLOCK_DEVICE_DISK_LOGICAL_BLOCK_SIZE_OFFSET,
          disk->logical_block_size);
    put32(bytes + KB2_BLOCK_DEVICE_DISK_PHYSICAL_BLOCK_SIZE_OFFSET,
          disk->physical_block_size);
    put32(bytes + KB2_BLOCK_DEVICE_DISK_REMOVABLE_OFFSET, disk->removable);
    put32(bytes + KB2_BLOCK_DEVICE_DISK_READ_ONLY_OFFSET, disk->read_only);
    memcpy(bytes + KB2_BLOCK_DEVICE_DISK_NAME_OFFSET, disk->name, sizeof(disk->name));
    return 0;
}

int kb2_block_disk_decode(const uint8_t *bytes, size_t size,
                          kb2_block_disk_t *disk) {
    kb2_block_disk_t value;
    if (!bytes || !disk || size < KB2_BLOCK_DEVICE_DISK_SIZE ||
        !name_terminated((const char *)bytes + KB2_BLOCK_DEVICE_DISK_NAME_OFFSET))
        return -1;
    memset(&value, 0, sizeof(value));
    value.diskseq = get64(bytes + KB2_BLOCK_DEVICE_DISK_DISKSEQ_OFFSET);
    value.bytes = get64(bytes + KB2_BLOCK_DEVICE_DISK_BYTES_OFFSET);
    value.major = get32(bytes + KB2_BLOCK_DEVICE_DISK_MAJOR_OFFSET);
    value.minor = get32(bytes + KB2_BLOCK_DEVICE_DISK_MINOR_OFFSET);
    value.logical_block_size = get32(bytes + KB2_BLOCK_DEVICE_DISK_LOGICAL_BLOCK_SIZE_OFFSET);
    value.physical_block_size = get32(bytes + KB2_BLOCK_DEVICE_DISK_PHYSICAL_BLOCK_SIZE_OFFSET);
    value.removable = get32(bytes + KB2_BLOCK_DEVICE_DISK_REMOVABLE_OFFSET);
    value.read_only = get32(bytes + KB2_BLOCK_DEVICE_DISK_READ_ONLY_OFFSET);
    memcpy(value.name, bytes + KB2_BLOCK_DEVICE_DISK_NAME_OFFSET, sizeof(value.name));
    if (!value.diskseq || !value.bytes || !value.logical_block_size ||
        !value.physical_block_size || value.removable > 1 || value.read_only > 1)
        return -1;
    *disk = value;
    return 0;
}
