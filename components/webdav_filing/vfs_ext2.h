// SPDX-License-Identifier: MIT
#pragma once

#include "ext2_fs.h"
#include "ext2_sd_blockdev.h"
#include "partition_scan.h"
#include "vfs.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#ifdef ESP_PLATFORM
#include <sd_pwr_ctrl.h>
#include <sdmmc_cmd.h>
#endif

namespace esphome {
namespace webdav_filing {

// WebDAV Vfs backed by the tailored ext2 implementation. The ext2 code
// uses global scratch buffers, so every entry point is serialized with a mutex.
class VfsExt2 : public Vfs {
 public:
  // Use an existing block device (used by host tests and RAM/other backends).
  VfsExt2(ext2_blockdev_t *device) : device_(device) {}

  // Own an SD card over SPI and expose a raw ext2 volume starting at
  // start_sector. sector_count == 0 means "the rest of the card".
  VfsExt2(int8_t clk, int8_t miso, int8_t mosi, int8_t cs, uint32_t frequency,
          uint32_t start_sector = 0, uint32_t sector_count = 0);

  // Own an SD card over SPI or SDMMC.
  VfsExt2(const SdConfig &config, uint32_t start_sector = 0, uint32_t sector_count = 0);
  ~VfsExt2() override { this->shutdown(); }

  bool init() override;

  // Partition selection for cards with an MBR or GPT. AUTO finds the first
  // partition carrying an ext2 superblock (or a tableless ext2 volume); RAW
  // forces the configured sector range; NUMBER/NAME select a partition by GPT
  // index or name. Every selected region must already contain ext2: this
  // component never formats or provisions storage. A nonzero start sector
  // implies RAW, while a sector count only caps an auto-detected region.
  void set_partition_auto() { this->partition_selector_ = PartitionSelector{}; }
  void set_partition_raw() { this->partition_selector_.kind = PartitionSelectorKind::RAW; }
  void set_partition_number(uint32_t number) {
    this->partition_selector_.kind = PartitionSelectorKind::NUMBER;
    this->partition_selector_.number = number;
  }
  void set_partition_name(const std::string &name) {
    this->partition_selector_.kind = PartitionSelectorKind::NAME;
    std::strncpy(this->partition_selector_.name, name.c_str(),
                 sizeof(this->partition_selector_.name) - 1);
  }

#ifdef ESP_PLATFORM
  // Use a card initialized by SharedSdCard. Auto mode calls this before init()
  // so FAT and ext2 share one SDMMC/SPI host and power handle.
  void set_sd_card(sdmmc_card_t *card) { this->external_card_ = card; }
#endif

  bool stat(const std::string &path, VfsStat *out) override;
  bool is_hidden(const std::string &path) const override;
  bool read_dir(const std::string &path, std::vector<VfsDirEntry> *out) override;
  bool read_file(const std::string &path, size_t offset, uint8_t *buf, size_t len,
                 size_t *read) override;
  bool write_file(const std::string &path, const uint8_t *buf, size_t len) override;
  bool mkdir(const std::string &path) override;
  bool remove(const std::string &path) override;
  bool rename(const std::string &from, const std::string &to) override;
  bool sync() override;
  void shutdown() override;

 protected:
  static std::string absolute_(const std::string &path);
  bool sd_init_();
  bool sd_init_spi_();
  bool sd_init_sdmmc_();
#ifdef ESP_PLATFORM
  bool configure_card_(sdmmc_card_t *card);
  sd_pwr_ctrl_handle_t create_power_control_();
  void release_power_control_();
#endif
  bool commit_locked_();

  ext2_blockdev_t *device_{nullptr};
  SdConfig config_;
  ext2_t fs_{};
  ext2_sd_blockdev_t sd_device_{};
  uint32_t start_sector_{0};
  uint32_t sector_count_{0};
  PartitionSelector partition_selector_{};
#ifdef ESP_PLATFORM
  sdmmc_card_t card_{};
  sdmmc_card_t *external_card_{nullptr};
  sd_pwr_ctrl_handle_t power_control_{nullptr};
#endif
  // The extracted ext2 implementation keeps several scratch buffers in static
  // storage. All VfsExt2 instances, not just one, must share this lock.
  static std::mutex &ext2_mutex_() {
    static std::mutex mutex;
    return mutex;
  }
  bool mounted_{false};
};

}  // namespace webdav_filing
}  // namespace esphome
