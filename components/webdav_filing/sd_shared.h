// SPDX-License-Identifier: MIT
#pragma once

#include "vfs.h"

#include <cstdint>

#ifdef ESP_PLATFORM
#include <driver/sdspi_host.h>
#include <sd_pwr_ctrl.h>
#include <sdmmc_cmd.h>
#endif

namespace esphome {
namespace webdav_filing {

// Owns one initialized SD card so FAT and ext2 can share the same bus and
// power handle in auto mode. The card object stays alive for the component's
// lifetime; neither backend may reinitialize the host.
class SharedSdCard {
 public:
  explicit SharedSdCard(const SdConfig &config) : config_(config) {}

  bool init();
  void shutdown();

#ifdef ESP_PLATFORM
  sdmmc_card_t *card() { return initialized_ ? &card_ : nullptr; }
  const sdmmc_card_t *card() const { return initialized_ ? &card_ : nullptr; }
#endif
  uint32_t total_sectors() const;
  uint32_t sector_size() const;
  bool initialized() const { return initialized_; }

 protected:
  bool init_spi_();
  bool init_sdmmc_();
  void release_power_control_();

  SdConfig config_;
  bool initialized_{false};
#ifdef ESP_PLATFORM
  sdmmc_card_t card_{};
  sd_pwr_ctrl_handle_t power_control_{nullptr};
  sdspi_dev_handle_t spi_device_{};
#endif
};

}  // namespace webdav_filing
}  // namespace esphome
