// SPDX-License-Identifier: MIT
#include "sd_shared.h"

#include "esphome/core/log.h"

#ifdef ESP_PLATFORM
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sdmmc_cmd.h>
#endif

namespace esphome {
namespace webdav_filing {

static const char *const TAG = "sd_shared";

bool SharedSdCard::init() {
  return this->config_.bus == SdBus::SDMMC ? this->init_sdmmc_() : this->init_spi_();
}

#ifdef ESP_PLATFORM
static sd_pwr_ctrl_handle_t create_power_control(const SdConfig &config) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  if (config.pwr_ctrl_ldo_channel == 0)
    return nullptr;
  sd_pwr_ctrl_ldo_config_t ldo_config = {};
  ldo_config.ldo_chan_id = config.pwr_ctrl_ldo_channel;
  sd_pwr_ctrl_handle_t handle = nullptr;
  const esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sd_pwr_ctrl_new_on_chip_ldo failed: %s", esp_err_to_name(err));
    return nullptr;
  }
  ESP_LOGI(TAG, "SDMMC power from on-chip LDO %u", config.pwr_ctrl_ldo_channel);
  return handle;
#else
  (void) config;
  return nullptr;
#endif
}
#endif

bool SharedSdCard::init_spi_() {
#ifdef ESP_PLATFORM
  spi_bus_config_t bus_cfg = {};
  bus_cfg.mosi_io_num = this->config_.mosi;
  bus_cfg.miso_io_num = this->config_.miso;
  bus_cfg.sclk_io_num = this->config_.clk;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  bus_cfg.max_transfer_sz = 4096;

  esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return false;
  }

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.max_freq_khz = this->config_.frequency / 1000;
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = static_cast<gpio_num_t>(this->config_.cs);
  slot.host_id = SPI2_HOST;

  err = host.init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "sdspi host init failed: %s", esp_err_to_name(err));
    return false;
  }
  err = sdspi_host_init_device(&slot, &this->spi_device_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sdspi_host_init_device failed: %s", esp_err_to_name(err));
    return false;
  }
  host.slot = this->spi_device_;

  err = sdmmc_card_init(&host, &this->card_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sdmmc_card_init failed: %s", esp_err_to_name(err));
    return false;
  }
  this->initialized_ = true;
  return true;
#else
  ESP_LOGE(TAG, "SD backend is only available on ESP-IDF");
  return false;
#endif
}

bool SharedSdCard::init_sdmmc_() {
#ifdef ESP_PLATFORM
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = this->config_.slot;
  host.max_freq_khz = this->config_.frequency / 1000;
  // Let one ext2 contiguous run become one SDMMC request.
  host.unaligned_multi_block_rw_max_chunk_size = 128;
  this->power_control_ = create_power_control(this->config_);
  host.pwr_ctrl_handle = this->power_control_;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = this->config_.width;
  slot.clk = static_cast<gpio_num_t>(this->config_.clk);
  slot.cmd = static_cast<gpio_num_t>(this->config_.cmd);
  slot.d0 = static_cast<gpio_num_t>(this->config_.d0);
  slot.d1 = static_cast<gpio_num_t>(this->config_.d1);
  slot.d2 = static_cast<gpio_num_t>(this->config_.d2);
  slot.d3 = static_cast<gpio_num_t>(this->config_.d3);
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_err_t err = host.init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "sdmmc host init failed: %s", esp_err_to_name(err));
    this->release_power_control_();
    return false;
  }
  err = sdmmc_host_init_slot(host.slot, &slot);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sdmmc_host_init_slot failed: %s", esp_err_to_name(err));
    this->release_power_control_();
    return false;
  }
  err = sdmmc_card_init(&host, &this->card_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sdmmc_card_init failed: %s", esp_err_to_name(err));
    this->release_power_control_();
    return false;
  }
  int real_freq_khz = 0;
  if (sdmmc_host_get_real_freq(host.slot, &real_freq_khz) == ESP_OK)
    ESP_LOGI(TAG, "SDMMC clock: requested %u kHz, actual %d kHz",
             static_cast<unsigned>(this->config_.frequency / 1000), real_freq_khz);
  this->initialized_ = true;
  return true;
#else
  ESP_LOGE(TAG, "SD backend is only available on ESP-IDF");
  return false;
#endif
}

void SharedSdCard::release_power_control_() {
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32P4)
  if (this->power_control_ == nullptr)
    return;
  const esp_err_t err = sd_pwr_ctrl_del_on_chip_ldo(this->power_control_);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "sd_pwr_ctrl_del_on_chip_ldo failed: %s", esp_err_to_name(err));
  this->power_control_ = nullptr;
#endif
}

void SharedSdCard::shutdown() {
  // The mounted backends still hold the card until WebDAV stops. Releasing the
  // optional LDO driver is the only teardown IDF needs here.
  this->release_power_control_();
}

uint32_t SharedSdCard::total_sectors() const {
#ifdef ESP_PLATFORM
  return this->initialized_ ? static_cast<uint32_t>(this->card_.csd.capacity) : 0;
#else
  return 0;
#endif
}

uint32_t SharedSdCard::sector_size() const {
#ifdef ESP_PLATFORM
  uint32_t size = this->initialized_ ? this->card_.csd.sector_size : 0;
  return size != 0 ? size : 512;
#else
  return 512;
#endif
}

}  // namespace webdav_filing
}  // namespace esphome
