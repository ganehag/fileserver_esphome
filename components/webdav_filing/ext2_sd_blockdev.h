// SPDX-License-Identifier: MIT
#pragma once

#include "ext2_blockdev.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  ext2_blockdev_t device;
  void *card;  /* sdmmc_card_t *, kept opaque so host tests need no IDF headers */
  uint32_t start_sector;
  uint32_t sector_count;
  uint32_t sectors_per_block;
} ext2_sd_blockdev_t;

/* Exposes a range of raw SD sectors as 4096-byte ext2 blocks. */
bool ext2_sd_blockdev_init(ext2_sd_blockdev_t *device, void *card, uint32_t start_sector,
                           uint32_t sector_count);

#ifdef __cplusplus
}
#endif
