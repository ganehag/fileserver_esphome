// SPDX-License-Identifier: MIT
#pragma once

#include "vfs.h"
#include <cstdint>
#include <sd_pwr_ctrl.h>
#include <sdmmc_cmd.h>
#include <string>

namespace esphome {
namespace webdav_filing {

// SD card mounted as FAT at /sd via ESP-IDF's VFS layer. The card can be
// attached either over SPI (classic ESP32) or 4-bit SDMMC (ESP32-P4 boards).
// The rest of the component only sees the Vfs interface.
class VfsSd : public Vfs {
 public:
  VfsSd(int8_t clk, int8_t miso, int8_t mosi, int8_t cs, uint32_t frequency);
  explicit VfsSd(const SdConfig &config) : config_(config) {}
  ~VfsSd() override { this->shutdown(); }

  bool init() override;
#ifdef ESP_PLATFORM
  // Mount FAT from a byte range of an already-initialized shared card. Auto
  // mode uses this so the SD host is initialized exactly once and so FAT can be
  // mounted from GPT and MBR logical partitions, which IDF's stock diskio
  // driver cannot address.
  bool init_with_card(sdmmc_card_t *card, uint32_t start_sector, uint32_t sector_count);
#endif
  void shutdown() override;

  bool stat(const std::string &path, VfsStat *out) override;
  bool read_dir(const std::string &path, std::vector<VfsDirEntry> *out) override;
  bool read_file(const std::string &path, size_t offset, uint8_t *buf, size_t len,
                 size_t *read) override;
  bool write_file(const std::string &path, const uint8_t *buf, size_t len) override;
  bool mkdir(const std::string &path) override;
  bool remove(const std::string &path) override;
  bool rename(const std::string &from, const std::string &to) override;

 protected:
  bool init_spi_();
  bool init_sdmmc_();
  sd_pwr_ctrl_handle_t create_power_control_();
  void release_power_control_();

  std::string full_(const std::string &path) const { return this->base_path_ + "/" + path; }

  std::string base_path_{"/sd"};
  SdConfig config_;
  sdmmc_card_t *card_{nullptr};
  sd_pwr_ctrl_handle_t power_control_{nullptr};
#ifdef ESP_PLATFORM
  uint8_t pdrv_{0xFF};
  char drive_[4]{};
  bool external_mount_{false};
#endif
};

}  // namespace webdav_filing
}  // namespace esphome
