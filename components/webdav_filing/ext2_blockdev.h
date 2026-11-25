// SPDX-License-Identifier: MIT
#pragma once

/* Minimal block-device contract for the tailored ext2 implementation. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t block_size;
  uint32_t block_count;
  void *context;

  bool (*read)(void *context, uint32_t lba, void *buffer, uint32_t blocks);
  bool (*write)(void *context, uint32_t lba, const void *buffer, uint32_t blocks);
  bool (*erase)(void *context, uint32_t lba, uint32_t blocks);
  bool (*sync)(void *context);
  bool (*flush)(void *context);
  bool (*flush_some)(void *context, unsigned max_blocks, bool *out_more);
  bool (*flush_range)(void *context, uint32_t lba, uint32_t blocks);
} ext2_blockdev_t;

bool ext2_blockdev_valid(const ext2_blockdev_t *device);
bool ext2_blockdev_read(ext2_blockdev_t *device, uint32_t lba, void *buffer, uint32_t blocks);
bool ext2_blockdev_write(ext2_blockdev_t *device, uint32_t lba, const void *buffer,
                         uint32_t blocks);
bool ext2_blockdev_erase(ext2_blockdev_t *device, uint32_t lba, uint32_t blocks);
bool ext2_blockdev_sync(ext2_blockdev_t *device);
bool ext2_blockdev_flush(ext2_blockdev_t *device);
bool ext2_blockdev_flush_some(ext2_blockdev_t *device, unsigned max_blocks, bool *out_more);
bool ext2_blockdev_flush_range(ext2_blockdev_t *device, uint32_t lba, uint32_t blocks);

#ifdef __cplusplus
}
#endif
