// SPDX-License-Identifier: MIT
#include "ext2_ram_blockdev.h"

#include <string.h>

static bool ram_read(void *context, uint32_t lba, void *buffer,
                     uint32_t blocks)
{
    ext2_ram_blockdev_t *const device = context;
    const size_t offset = (size_t)lba * device->device.block_size;
    const size_t length = (size_t)blocks * device->device.block_size;
    memcpy(buffer, device->storage + offset, length);
    return true;
}

static bool ram_write(void *context, uint32_t lba, const void *buffer,
                      uint32_t blocks)
{
    ext2_ram_blockdev_t *const device = context;
    const size_t offset = (size_t)lba * device->device.block_size;
    const size_t length = (size_t)blocks * device->device.block_size;
    memcpy(device->storage + offset, buffer, length);
    return true;
}

static bool ram_erase(void *context, uint32_t lba, uint32_t blocks)
{
    ext2_ram_blockdev_t *const device = context;
    const size_t offset = (size_t)lba * device->device.block_size;
    const size_t length = (size_t)blocks * device->device.block_size;
    memset(device->storage + offset, 0, length);
    return true;
}

static bool ram_sync(void *context)
{
    (void)context;
    return true;
}

bool ext2_ram_blockdev_init(ext2_ram_blockdev_t *device, void *storage,
                            size_t storage_bytes, uint32_t block_size)
{
    if (device == NULL || storage == NULL || block_size == 0 ||
        storage_bytes < block_size) {
        return false;
    }
    const uint32_t block_count = (uint32_t)(storage_bytes / block_size);
    memset(storage, 0, (size_t)block_count * block_size);
    memset(device, 0, sizeof(*device));
    device->storage = storage;
    device->block_count = block_count;
    device->device.block_size = block_size;
    device->device.block_count = block_count;
    device->device.context = device;
    device->device.read = ram_read;
    device->device.write = ram_write;
    device->device.erase = ram_erase;
    device->device.sync = ram_sync;
    return true;
}
