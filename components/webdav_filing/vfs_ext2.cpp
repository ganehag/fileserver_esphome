// SPDX-License-Identifier: MIT
#include "vfs_ext2.h"

#include "ext2_file_mode.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstdlib>
#include <ctime>
#include <mutex>
#include <utility>

#ifdef ESP_PLATFORM
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sdmmc_cmd.h>
#endif

namespace esphome {
namespace webdav_filing {

static const char *const TAG = "vfs_ext2";

static void *ext2_allocate(size_t bytes) {
#ifdef ESP_PLATFORM
  /* Large host-formatted ext2 volumes need descriptor tables and fixed
   * metadata caches. Keep them out of the limited internal heap when the
   * board has PSRAM. */
  void *const psram =
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED);
  if (psram != nullptr) return psram;
#endif
  return malloc(bytes);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
}

static void ext2_release(void *memory) {
#ifdef ESP_PLATFORM
  heap_caps_free(memory);
#else
  free(memory);
#endif
}

static uint32_t ext2_now() {
  const time_t wall = ::time(nullptr);
  uint32_t seconds = wall > 0 ? static_cast<uint32_t>(wall) : 0;
  // Without SNTP the RTC reads as a tiny value; keep timestamps monotonic and
  // sane by falling back to 2026-01-01 + uptime.
  if (seconds < 1767225600u)
    seconds = 1767225600u + millis() / 1000;
  return seconds;
}

[[maybe_unused]] static const char *status_name(ext2_status_t status) {
  switch (status) {
    case EXT2_OK:
      return "OK";
    case EXT2_NOT_FORMATTED:
      return "NOT_FORMATTED";
    case EXT2_NOT_FOUND:
      return "NOT_FOUND";
    case EXT2_NO_SPACE:
      return "NO_SPACE";
    case EXT2_NOT_EMPTY:
      return "NOT_EMPTY";
    case EXT2_READ_ONLY:
      return "READ_ONLY";
    case EXT2_UNSUPPORTED:
      return "UNSUPPORTED";
    case EXT2_IO_ERROR:
      return "IO_ERROR";
    case EXT2_INVALID:
      return "INVALID";
    default:
      return "ERROR";
  }
}

std::string VfsExt2::absolute_(const std::string &path) {
  if (!path.empty() && path[0] == '/')
    return path;
  return "/" + path;
}

// ext2 puts recovered files in /lost+found. It is filesystem housekeeping, not
// share content, so it is hidden at the VFS boundary on all WebDAV methods.
static bool hidden_lost_found_(const std::string &path) {
  constexpr char kName[] = "/lost+found";
  constexpr size_t kNameLen = sizeof(kName) - 1;
  std::string full = path.empty() || path[0] != '/' ? "/" + path : path;
  while (full.size() > 1 && full.back() == '/')
    full.pop_back();
  return full == kName || full.compare(0, kNameLen + 1, "/lost+found/") == 0;
}

#ifdef ESP_PLATFORM
static bool read_card_sectors(void *context, uint32_t lba, void *buffer, uint32_t count) {
  auto *const card = static_cast<sdmmc_card_t *>(context);
  return sdmmc_read_sectors(card, buffer, lba, count) == ESP_OK;
}
#endif

static SdConfig spi_config(int8_t clk, int8_t miso, int8_t mosi, int8_t cs, uint32_t frequency) {
  SdConfig config;
  config.bus = SdBus::SPI;
  config.clk = clk;
  config.miso = miso;
  config.mosi = mosi;
  config.cs = cs;
  config.frequency = frequency;
  return config;
}

VfsExt2::VfsExt2(int8_t clk, int8_t miso, int8_t mosi, int8_t cs, uint32_t frequency,
                 uint32_t start_sector, uint32_t sector_count)
    : VfsExt2(spi_config(clk, miso, mosi, cs, frequency), start_sector, sector_count) {}

VfsExt2::VfsExt2(const SdConfig &config, uint32_t start_sector, uint32_t sector_count)
    : config_(config),
      start_sector_(start_sector),
      sector_count_(sector_count) {
  // A nonzero explicit start sector is the legacy raw-volume configuration.
  // A sector count alone is only a cap on the auto-detected region; codegen can
  // override either with set_partition_raw()/number()/name().
  if (start_sector_ != 0)
    this->partition_selector_.kind = PartitionSelectorKind::RAW;
}

#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32P4)
sd_pwr_ctrl_handle_t VfsExt2::create_power_control_() {
  if (this->config_.pwr_ctrl_ldo_channel == 0)
    return nullptr;
  sd_pwr_ctrl_ldo_config_t ldo_config = {};
  ldo_config.ldo_chan_id = this->config_.pwr_ctrl_ldo_channel;
  sd_pwr_ctrl_handle_t handle = nullptr;
  const esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sd_pwr_ctrl_new_on_chip_ldo failed: %s", esp_err_to_name(err));
    return nullptr;
  }
  ESP_LOGI(TAG, "SDMMC power from on-chip LDO %u", this->config_.pwr_ctrl_ldo_channel);
  return handle;
}

void VfsExt2::release_power_control_() {
  if (this->power_control_ == nullptr)
    return;
  const esp_err_t err = sd_pwr_ctrl_del_on_chip_ldo(this->power_control_);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "sd_pwr_ctrl_del_on_chip_ldo failed: %s", esp_err_to_name(err));
  this->power_control_ = nullptr;
}
#elif defined(ESP_PLATFORM)
sd_pwr_ctrl_handle_t VfsExt2::create_power_control_() { return nullptr; }
void VfsExt2::release_power_control_() {}
#endif

bool VfsExt2::sd_init_spi_() {
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

  // sdmmc_card_init() expects the SDSPI device to already be attached; this is
  // the same sequence esp_vfs_fat_sdspi_mount() uses for the FAT backend.
  err = host.init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "sdspi host init failed: %s", esp_err_to_name(err));
    return false;
  }
  sdspi_dev_handle_t slot_handle{};
  err = sdspi_host_init_device(&slot, &slot_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sdspi_host_init_device failed: %s", esp_err_to_name(err));
    return false;
  }
  host.slot = slot_handle;

  err = sdmmc_card_init(&host, &this->card_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sdmmc_card_init failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
#else
  ESP_LOGE(TAG, "SD backend is only available on ESP-IDF");
  return false;
#endif
}

bool VfsExt2::sd_init_sdmmc_() {
#ifdef ESP_PLATFORM
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = this->config_.slot;
  host.max_freq_khz = this->config_.frequency / 1000;
  /* Let one ext2 contiguous run (16 blocks = 128 sectors) become one SDMMC
   * request. The default chunk of 16 sectors splits it into eight commands. */
  host.unaligned_multi_block_rw_max_chunk_size = 128;
  this->power_control_ = this->create_power_control_();
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
  // Keep the power-control handle for the life of the card: SDMMC can still
  // use it when changing IO voltage after initialization.
  return true;
#else
  ESP_LOGE(TAG, "SD backend is only available on ESP-IDF");
  return false;
#endif
}

bool VfsExt2::sd_init_() {
  const bool card_ok =
      this->config_.bus == SdBus::SDMMC ? this->sd_init_sdmmc_() : this->sd_init_spi_();
  if (!card_ok)
    return false;
#ifdef ESP_PLATFORM
  if (!this->configure_card_(&this->card_)) {
    this->release_power_control_();
    return false;
  }
  return true;
#else
  return false;
#endif
}

#ifdef ESP_PLATFORM
bool VfsExt2::configure_card_(sdmmc_card_t *card) {
  const uint32_t total_sectors = static_cast<uint32_t>(card->csd.capacity);
  PartitionSelector selector = this->partition_selector_;
  PartitionSelection selection{};

  if (selector.kind == PartitionSelectorKind::RAW) {
    const uint32_t start = this->start_sector_;
    uint32_t count = this->sector_count_;
    if (count == 0)
      count = total_sectors - start;
    if (start >= total_sectors || count == 0 || count > total_sectors - start) {
      ESP_LOGE(TAG, "invalid ext2 sector range %u+%u (card has %u sectors)",
               static_cast<unsigned>(start), static_cast<unsigned>(count),
               static_cast<unsigned>(total_sectors));
      return false;
    }
    selection.table = PartitionTable::RAW;
    selection.start_sector = start;
    selection.sector_count = count;
  } else {
    if (!scan_for_ext2(card, read_card_sectors, total_sectors, selector, &selection)) {
      ESP_LOGE(TAG, "no ext2 partition found (set partition to raw, a number, or name:...)");
      return false;
    }
    if (selection.start_sector >= total_sectors || selection.sector_count == 0 ||
        selection.sector_count > total_sectors - selection.start_sector) {
      ESP_LOGE(TAG, "ext2 partition %u+%u is outside the card (%u sectors)",
               static_cast<unsigned>(selection.start_sector),
               static_cast<unsigned>(selection.sector_count), static_cast<unsigned>(total_sectors));
      return false;
    }
    if (this->sector_count_ != 0 && selection.sector_count > this->sector_count_) {
      ESP_LOGI(TAG, "limiting ext2 partition to configured %u sectors",
               static_cast<unsigned>(this->sector_count_));
      selection.sector_count = this->sector_count_;
    }
  }

  uint32_t sector_size = card->csd.sector_size;
  if (sector_size == 0)
    sector_size = 512;
  if (EXT2_BLOCK_BYTES % sector_size != 0) {
    ESP_LOGE(TAG, "unsupported SD sector size %u", static_cast<unsigned>(sector_size));
    return false;
  }
  const uint32_t sectors_per_block = EXT2_BLOCK_BYTES / sector_size;
  const uint32_t trailing = selection.sector_count % sectors_per_block;
  if (trailing != 0)
    ESP_LOGW(TAG, "ignoring %u trailing sectors before ext2 block alignment",
             static_cast<unsigned>(trailing));
  const uint32_t count = selection.sector_count - trailing;
  if (!ext2_sd_blockdev_init(&this->sd_device_, card, selection.start_sector, count)) {
    ESP_LOGE(TAG, "ext2_sd_blockdev_init failed");
    return false;
  }
  this->device_ = &this->sd_device_.device;
  ESP_LOGI(TAG, "ext2 %s partition %u at LBA %u+%u%s%s: %u blocks of %u bytes (%s)",
           partition_table_name(selection.table), static_cast<unsigned>(selection.number),
           static_cast<unsigned>(selection.start_sector), static_cast<unsigned>(count),
           selection.name[0] != '\0' ? " name=" : "",
           selection.name[0] != '\0' ? selection.name : "",
           static_cast<unsigned>(this->sd_device_.device.block_count),
           static_cast<unsigned>(this->sd_device_.device.block_size),
           this->config_.bus == SdBus::SDMMC ? "SDMMC" : "SPI");
  return true;
}
#endif

bool VfsExt2::init() {
  std::lock_guard<std::mutex> guard(ext2_mutex_());
  ext2_set_allocator(ext2_allocate, ext2_release);
  ext2_set_clock(ext2_now);
  if (this->device_ == nullptr) {
#ifdef ESP_PLATFORM
    if (this->external_card_ != nullptr) {
      if (!this->configure_card_(this->external_card_))
        return false;
    } else if (!this->sd_init_()) {
      return false;
    }
#else
    if (!this->sd_init_())
      return false;
#endif
  }

  const ext2_status_t status = ext2_mount(&this->fs_, this->device_);
  if (status != EXT2_OK) {
    ESP_LOGE(TAG, "ext2_mount failed: %s", status_name(status));
#ifdef ESP_PLATFORM
    if (this->external_card_ == nullptr)
      this->release_power_control_();
#endif
    return false;
  }

  this->fs_.create_uid = 0;
  this->fs_.create_gid = 0;
  this->fs_.create_umask = 0;
  this->mounted_ = true;
  ESP_LOGI(TAG, "ext2 mounted: %u blocks, %u inodes", static_cast<unsigned>(this->fs_.block_count),
           static_cast<unsigned>(this->fs_.inode_count));
  return true;
}

bool VfsExt2::commit_locked_() {
  if (!this->mounted_)
    return false;
  const ext2_status_t status = ext2_commit(&this->fs_);
  if (status != EXT2_OK) {
    ESP_LOGE(TAG, "ext2_commit failed: %s", status_name(status));
    return false;
  }
  return true;
}

bool VfsExt2::stat(const std::string &path, VfsStat *out) {
  std::lock_guard<std::mutex> guard(ext2_mutex_());
  if (!this->mounted_ || out == nullptr)
    return false;
  if (hidden_lost_found_(path)) {
    *out = VfsStat{};
    return true;
  }
  ext2_stat_t st{};
  const ext2_status_t status = ext2_stat(&this->fs_, absolute_(path).c_str(), &st);
  if (status == EXT2_NOT_FOUND) {
    *out = VfsStat{};
    return true;
  }
  if (status != EXT2_OK)
    return false;
  out->exists = true;
  out->is_dir = EXT2_S_ISDIR(st.mode);
  out->size = st.size;
  out->mtime = static_cast<time_t>(st.mtime);
  return true;
}

bool VfsExt2::is_hidden(const std::string &path) const {
  return hidden_lost_found_(path);
}

bool VfsExt2::read_dir(const std::string &path, std::vector<VfsDirEntry> *out) {
  std::lock_guard<std::mutex> guard(ext2_mutex_());
  if (!this->mounted_ || out == nullptr)
    return false;
  std::string full = absolute_(path);
  while (full.size() > 1 && full.back() == '/')
    full.pop_back();
  out->clear();
  uint32_t dir_ino = 0;
  const ext2_status_t lookup = ext2_lookup(&this->fs_, full.c_str(), &dir_ino);
  if (lookup == EXT2_NOT_FOUND)
    return true;
  if (lookup != EXT2_OK)
    return false;
  char name[EXT2_NAME_BYTES];
  bool is_dir = false;
  uint32_t ino = 0;
  uint8_t type = 0;
  for (unsigned index = 0;; ++index) {
    const ext2_status_t status = ext2_list_inode_full(&this->fs_, dir_ino, index, name,
                                                      sizeof(name), &is_dir, &ino, &type);
    if (status == EXT2_NOT_FOUND)
      break;
    if (status != EXT2_OK)
      return false;
    if (full == "/" && std::string(name) == "lost+found")
      continue;
    VfsDirEntry entry;
    entry.name = name;
    entry.is_dir = is_dir;
    ext2_stat_t st{};
    if (ext2_stat_inode(&this->fs_, ino, &st) == EXT2_OK) {
      entry.size = st.size;
      entry.mtime = static_cast<time_t>(st.mtime);
    }
    out->push_back(std::move(entry));
  }
  return true;
}

bool VfsExt2::read_file(const std::string &path, size_t offset, uint8_t *buf, size_t len,
                        size_t *read) {
  std::lock_guard<std::mutex> guard(ext2_mutex_());
  if (!this->mounted_ || read == nullptr || hidden_lost_found_(path))
    return false;
  *read = 0;
  const ext2_status_t status =
      ext2_read_at(&this->fs_, absolute_(path).c_str(), offset, buf, len, read);
  return status == EXT2_OK;
}

bool VfsExt2::write_file(const std::string &path, const uint8_t *buf, size_t len) {
  std::lock_guard<std::mutex> guard(ext2_mutex_());
  if (!this->mounted_ || hidden_lost_found_(path))
    return false;
  const ext2_status_t status =
      ext2_append_file_deferred(&this->fs_, absolute_(path).c_str(), buf, len);
  if (status != EXT2_OK) {
    ESP_LOGE(TAG, "ext2_append_file_deferred(%s) failed: %s", path.c_str(), status_name(status));
    return false;
  }
  // PUT/COPY call Vfs::sync() once at the end of a file. Keeping metadata dirty
  // between chunks avoids writing the inode/bitmap on every 4 KiB buffer; other
  // mutations commit immediately below.
  return true;
}

bool VfsExt2::mkdir(const std::string &path) {
  std::lock_guard<std::mutex> guard(ext2_mutex_());
  if (!this->mounted_ || hidden_lost_found_(path))
    return false;
  const ext2_status_t status = ext2_mkdir(&this->fs_, absolute_(path).c_str());
  if (status != EXT2_OK)
    return false;
  return this->commit_locked_();
}

bool VfsExt2::remove(const std::string &path) {
  std::lock_guard<std::mutex> guard(ext2_mutex_());
  if (!this->mounted_ || hidden_lost_found_(path))
    return false;
  const ext2_status_t status = ext2_unlink(&this->fs_, absolute_(path).c_str());
  if (status != EXT2_OK)
    return false;
  return this->commit_locked_();
}

bool VfsExt2::rename(const std::string &from, const std::string &to) {
  std::lock_guard<std::mutex> guard(ext2_mutex_());
  if (!this->mounted_ || hidden_lost_found_(from) || hidden_lost_found_(to))
    return false;
  const ext2_status_t status =
      ext2_rename(&this->fs_, absolute_(from).c_str(), absolute_(to).c_str());
  if (status != EXT2_OK) {
    ESP_LOGE(TAG, "ext2_rename failed: %s", status_name(status));
    return false;
  }
  return this->commit_locked_();
}

bool VfsExt2::sync() {
  std::lock_guard<std::mutex> guard(ext2_mutex_());
  return this->commit_locked_();
}

void VfsExt2::shutdown() {
  std::lock_guard<std::mutex> guard(ext2_mutex_());
  if (this->mounted_) {
    const ext2_status_t status = ext2_unmount(&this->fs_);
    if (status == EXT2_OK) {
      this->mounted_ = false;
    } else {
      // ext2_unmount deliberately keeps the mount alive on a failed commit so
      // the caller can retry. Do not drop the only owner of its metadata.
      ESP_LOGW(TAG, "ext2_unmount failed: %s; keeping mount for retry",
               status_name(status));
    }
  }
#ifdef ESP_PLATFORM
  this->release_power_control_();
#endif
}

}  // namespace webdav_filing
}  // namespace esphome
