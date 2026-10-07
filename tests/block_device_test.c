/* SPDX-License-Identifier: MIT */
#include <kobox2/block_device.h>

#include <assert.h>
#include <string.h>

int main(void) {
    uint8_t header_bytes[KB2_BLOCK_DEVICE_HEADER_SIZE];
    uint8_t disk_bytes[KB2_BLOCK_DEVICE_DISK_SIZE];
    kb2_block_header_t header = {.opcode = KB2_BLOCK_DEVICE_OP_READ,
        .generation = 7, .sequence = 9, .diskseq = 11, .major = 259,
        .minor = 1, .offset = 4096, .length = 4096};
    kb2_block_header_t decoded = {0};
    kb2_block_disk_t disk = {.diskseq = 11, .bytes = 1024 * 1024,
        .major = 259, .minor = 1, .logical_block_size = 512,
        .physical_block_size = 4096, .read_only = 0};
    kb2_block_disk_t decoded_disk = {0};
    memcpy(disk.name, "nvme0n1", 8);

    assert(kb2_block_header_encode(header_bytes, sizeof(header_bytes), &header) == 0);
    assert(kb2_block_header_decode(header_bytes, sizeof(header_bytes), &decoded) == 0);
    assert(decoded.generation == 7 && decoded.diskseq == 11 && decoded.offset == 4096);
    header_bytes[KB2_BLOCK_DEVICE_HEADER_RESERVED_OFFSET] = 1;
    assert(kb2_block_header_decode(header_bytes, sizeof(header_bytes), &decoded) != 0);
    header_bytes[KB2_BLOCK_DEVICE_HEADER_RESERVED_OFFSET] = 0;
    header_bytes[KB2_BLOCK_DEVICE_HEADER_LENGTH_OFFSET + 3] = 0xff;
    assert(kb2_block_header_decode(header_bytes, sizeof(header_bytes), &decoded) != 0);
    header_bytes[KB2_BLOCK_DEVICE_HEADER_LENGTH_OFFSET + 3] = 0;
    header_bytes[KB2_BLOCK_DEVICE_HEADER_COUNT_OFFSET] = KB2_BLOCK_DEVICE_MAX_DISKS + 1;
    assert(kb2_block_header_decode(header_bytes, sizeof(header_bytes), &decoded) != 0);
    header_bytes[KB2_BLOCK_DEVICE_HEADER_COUNT_OFFSET] = 0;
    assert(kb2_block_header_decode(header_bytes, sizeof(header_bytes), &decoded) == 0);

    kb2_block_header_t denied = {.opcode = KB2_BLOCK_DEVICE_OP_WRITE,
        .status = KB2_BLOCK_DEVICE_STATUS_READ_ONLY, .generation = 7,
        .sequence = 10, .diskseq = 11, .major = 259, .minor = 1,
        .offset = 4096, .length = 4096};
    assert(kb2_block_header_encode(header_bytes, sizeof(header_bytes), &denied) == 0);
    assert(kb2_block_header_decode(header_bytes, sizeof(header_bytes), &decoded) == 0);
    assert(decoded.status == KB2_BLOCK_DEVICE_STATUS_READ_ONLY);

    assert(kb2_block_disk_encode(disk_bytes, sizeof(disk_bytes), &disk) == 0);
    assert(kb2_block_disk_decode(disk_bytes, sizeof(disk_bytes), &decoded_disk) == 0);
    assert(decoded_disk.bytes == disk.bytes && !strcmp(decoded_disk.name, disk.name));
    memset(disk_bytes + KB2_BLOCK_DEVICE_DISK_NAME_OFFSET, 'x', 32);
    assert(kb2_block_disk_decode(disk_bytes, sizeof(disk_bytes), &decoded_disk) != 0);
    disk.read_only = 2;
    assert(kb2_block_disk_encode(disk_bytes, sizeof(disk_bytes), &disk) != 0);
    return 0;
}
