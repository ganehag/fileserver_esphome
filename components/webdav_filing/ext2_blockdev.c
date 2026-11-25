// SPDX-License-Identifier: MIT
#include "ext2_blockdev.h"

#include <stddef.h>

bool ext2_blockdev_valid(const ext2_blockdev_t *device)
{
    return device != NULL && device->block_size != 0 &&
           device->block_count != 0 && device->read != NULL &&
           device->write != NULL;
}

bool ext2_blockdev_read(ext2_blockdev_t *device, uint32_t lba, void *buffer,
                        uint32_t blocks)
{
    if (!ext2_blockdev_valid(device) || buffer == NULL ||
        blocks == 0 || lba > device->block_count ||
        blocks > device->block_count - lba) {
        return false;
    }
    return device->read(device->context, lba, buffer, blocks);
}

bool ext2_blockdev_write(ext2_blockdev_t *device, uint32_t lba,
                         const void *buffer, uint32_t blocks)
{
    if (!ext2_blockdev_valid(device) || buffer == NULL ||
        blocks == 0 || lba > device->block_count ||
        blocks > device->block_count - lba) {
        return false;
    }
    return device->write(device->context, lba, buffer, blocks);
}

bool ext2_blockdev_erase(ext2_blockdev_t *device, uint32_t lba,
                         uint32_t blocks)
{
    if (!ext2_blockdev_valid(device) || blocks == 0 ||
        lba > device->block_count ||
        blocks > device->block_count - lba) {
        return false;
    }
    return device->erase != NULL &&
           device->erase(device->context, lba, blocks);
}

bool ext2_blockdev_sync(ext2_blockdev_t *device)
{
    if (!ext2_blockdev_valid(device)) return false;
    return device->sync == NULL || device->sync(device->context);
}

bool ext2_blockdev_flush(ext2_blockdev_t *device)
{
    if (!ext2_blockdev_valid(device)) return false;
    return device->flush == NULL || device->flush(device->context);
}

bool ext2_blockdev_flush_some(ext2_blockdev_t *device, unsigned max_blocks,
                              bool *out_more)
{
    if (out_more != NULL) *out_more = false;
    if (!ext2_blockdev_valid(device)) return false;
    if (device->flush_some != NULL) {
        return device->flush_some(device->context, max_blocks, out_more);
    }
    return device->flush == NULL || device->flush(device->context);
}

bool ext2_blockdev_flush_range(ext2_blockdev_t *device, uint32_t lba,
                               uint32_t blocks)
{
    if (!ext2_blockdev_valid(device)) return false;
    if (blocks == 0) return true;
    if (lba > device->block_count || blocks > device->block_count - lba)
        return false;
    if (device->flush_range != NULL) {
        return device->flush_range(device->context, lba, blocks);
    }
    return device->flush == NULL || device->flush(device->context);
}
