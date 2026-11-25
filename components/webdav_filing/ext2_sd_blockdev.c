// SPDX-License-Identifier: MIT
/* Raw SD adapter for the ext2 implementation.
 *
 * ext2 works in 4096-byte logical blocks; SD cards expose 512-byte sectors.
 * This adapter groups sectors into ext2 blocks and borrows an already
 * initialised sdmmc_card_t from the caller. */

#include "ext2_sd_blockdev.h"

#include "ext2_fs.h"

#include <esp_err.h>
#include <sdmmc_cmd.h>
#include <stddef.h>
#include <string.h>

static bool sd_read(void *context, uint32_t lba, void *buffer, uint32_t blocks) {
  ext2_sd_blockdev_t *const dev = context;
  sdmmc_card_t *const card = (sdmmc_card_t *) dev->card;
  const uint32_t start = dev->start_sector + lba * dev->sectors_per_block;
  const uint32_t count = blocks * dev->sectors_per_block;
  return sdmmc_read_sectors(card, buffer, start, count) == ESP_OK;
}

static bool sd_write(void *context, uint32_t lba, const void *buffer, uint32_t blocks) {
  ext2_sd_blockdev_t *const dev = context;
  sdmmc_card_t *const card = (sdmmc_card_t *) dev->card;
  const uint32_t start = dev->start_sector + lba * dev->sectors_per_block;
  const uint32_t count = blocks * dev->sectors_per_block;
  return sdmmc_write_sectors(card, buffer, start, count) == ESP_OK;
}

static bool sd_erase(void *context, uint32_t lba, uint32_t blocks) {
  (void) context;
  (void) lba;
  (void) blocks;
  /* Host-side ext2 image builders call erase() to make an old superblock
   * harmless; the firmware never formats. That path immediately writes a fresh
   * superblock and descriptor table over block 0, so an explicit erase command
   * is unnecessary. */
  return true;
}

static bool sd_sync(void *context) {
  (void) context;
  return true;
}

static bool sd_flush(void *context) {
  (void) context;
  /* sdmmc_write_sectors() completes synchronously for SPI cards. */
  return true;
}

bool ext2_sd_blockdev_init(ext2_sd_blockdev_t *device, void *card, uint32_t start_sector,
                           uint32_t sector_count) {
  if (device == NULL || card == NULL)
    return false;
  sdmmc_card_t *const sd_card = (sdmmc_card_t *) card;
  uint32_t sector_size = (uint32_t) sd_card->csd.sector_size;
  if (sector_size == 0)
    sector_size = 512;
  if (EXT2_BLOCK_BYTES % sector_size != 0)
    return false;

  memset(device, 0, sizeof(*device));
  device->card = card;
  device->start_sector = start_sector;
  device->sectors_per_block = EXT2_BLOCK_BYTES / sector_size;
  device->sector_count = sector_count;

  ext2_blockdev_t *const bd = &device->device;
  bd->block_size = EXT2_BLOCK_BYTES;
  bd->block_count = sector_count / device->sectors_per_block;
  bd->context = device;
  bd->read = sd_read;
  bd->write = sd_write;
  bd->erase = sd_erase;
  bd->sync = sd_sync;
  bd->flush = sd_flush;
  return bd->block_count >= 16;
}
