// SPDX-License-Identifier: MIT
#pragma once

#include "ext2_blockdev.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  ext2_blockdev_t device;
  void *storage;
  uint32_t block_count;
} ext2_ram_blockdev_t;

bool ext2_ram_blockdev_init(ext2_ram_blockdev_t *device, void *storage, size_t storage_bytes,
                            uint32_t block_size);

#ifdef __cplusplus
}
#endif
