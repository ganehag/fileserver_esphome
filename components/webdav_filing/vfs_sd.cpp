// SPDX-License-Identifier: MIT
#include "vfs_sd.h"
#include <cstring>
#include <dirent.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <fcntl.h>
#include <driver/sdspi_host.h>
#include <driver/sdmmc_host.h>
#include <driver/spi_master.h>
#include <diskio_impl.h>
#include <diskio_sdmmc.h>
#include <ff.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sys/stat.h>
#include <unistd.h>

namespace esphome {
namespace webdav_filing {

static const char *const TAG = "vfs_sd";

// Mirror the HTTPD client-session budget so parallel media probes do not fail
// with FR_TOO_MANY_OPEN_FILES. FatFs opens are short-lived, but 12 concurrent
// GET workers can overlap.
constexpr BYTE kMaxOpenFatFiles = 12;

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

VfsSd::VfsSd(int8_t clk, int8_t miso, int8_t mosi, int8_t cs, uint32_t frequency)
    : VfsSd(spi_config(clk, miso, mosi, cs, frequency)) {}

bool VfsSd::init() {
  return this->config_.bus == SdBus::SDMMC ? this->init_sdmmc_() : this->init_spi_();
}

#ifdef ESP_PLATFORM
namespace {

// Presents one discovered FAT partition to FatFs as a complete drive. IDF's
// sdmmc diskio driver always starts at card LBA 0, which only works for a
// tableless card or a simple MBR primary. This offset view also handles GPT and
// MBR logical partitions without ever rewriting partition tables.
struct FatPartition {
  sdmmc_card_t *card{nullptr};
  uint32_t start_sector{0};
  uint32_t sector_count{0};
};

FatPartition s_fat_partitions[FF_VOLUMES];

FatPartition *partition_for_(BYTE pdrv) {
  return pdrv < FF_VOLUMES ? &s_fat_partitions[pdrv] : nullptr;
}

bool sector_range_valid_(const FatPartition *partition, DWORD sector, UINT count) {
  return partition != nullptr && partition->card != nullptr && count != 0 &&
         sector < partition->sector_count && count <= partition->sector_count - sector;
}

DSTATUS partition_init_(BYTE pdrv) {
  const FatPartition *partition = partition_for_(pdrv);
  return partition != nullptr && partition->card != nullptr ? 0 : STA_NOINIT;
}

DSTATUS partition_status_(BYTE pdrv) { return partition_init_(pdrv); }

DRESULT partition_read_(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
  const FatPartition *partition = partition_for_(pdrv);
  if (!sector_range_valid_(partition, sector, count))
    return RES_PARERR;
  if (sdmmc_read_sectors(partition->card, buff, partition->start_sector + sector, count) != ESP_OK) {
    ESP_LOGE(TAG, "FAT partition read failed");
    return RES_ERROR;
  }
  return RES_OK;
}

DRESULT partition_write_(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count) {
  const FatPartition *partition = partition_for_(pdrv);
  if (!sector_range_valid_(partition, sector, count))
    return RES_PARERR;
  if (sdmmc_write_sectors(partition->card, buff, partition->start_sector + sector, count) !=
      ESP_OK) {
    ESP_LOGE(TAG, "FAT partition write failed");
    return RES_ERROR;
  }
  return RES_OK;
}

DRESULT partition_ioctl_(BYTE pdrv, BYTE cmd, void *buff) {
  const FatPartition *partition = partition_for_(pdrv);
  if (partition == nullptr || partition->card == nullptr)
    return RES_ERROR;
  switch (cmd) {
    case CTRL_SYNC:
      return RES_OK;
    case GET_SECTOR_COUNT:
      *static_cast<DWORD *>(buff) = partition->sector_count;
      return RES_OK;
    case GET_SECTOR_SIZE:
      *static_cast<WORD *>(buff) = 512;
      return RES_OK;
    case GET_BLOCK_SIZE:
      return RES_ERROR;
#if FF_USE_TRIM
    case CTRL_TRIM: {
      if (sdmmc_can_trim(partition->card) != ESP_OK)
        return RES_PARERR;
      const DWORD *range = static_cast<const DWORD *>(buff);
      const DWORD start = range[0];
      const DWORD end = range[1];
      if (end < start || end >= partition->sector_count)
        return RES_PARERR;
      const sdmmc_erase_arg_t arg = sdmmc_can_discard(partition->card) == ESP_OK
                                        ? SDMMC_DISCARD_ARG
                                        : SDMMC_ERASE_ARG;
      if (sdmmc_erase_sectors(partition->card, partition->start_sector + start, end - start + 1,
                              arg) != ESP_OK)
        return RES_ERROR;
      return RES_OK;
    }
#endif
    default:
      return RES_PARERR;
  }
}

const ff_diskio_impl_t kPartitionDiskio = {
    .init = &partition_init_,
    .status = &partition_status_,
    .read = &partition_read_,
    .write = &partition_write_,
    .ioctl = &partition_ioctl_,
};

}  // namespace

bool VfsSd::init_with_card(sdmmc_card_t *card, uint32_t start_sector, uint32_t sector_count) {
  if (card == nullptr || sector_count == 0 ||
      start_sector >= static_cast<uint32_t>(card->csd.capacity) ||
      sector_count > static_cast<uint32_t>(card->csd.capacity) - start_sector)
    return false;

  BYTE pdrv = FF_DRV_NOT_USED;
  if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == FF_DRV_NOT_USED || pdrv >= FF_VOLUMES) {
    ESP_LOGE(TAG, "no free FATFS drive for shared card");
    return false;
  }
  s_fat_partitions[pdrv] = FatPartition{card, start_sector, sector_count};
  // Sector 0 of this view is the FAT boot sector, so FatFs auto-detects it as
  // a super-floppy volume and never consults the card's MBR/GPT.
  VolToPart[pdrv] = {pdrv, 0};
  ff_diskio_register(pdrv, &kPartitionDiskio);

  // IDF's VFS paths are global; a second FAT partition needs its own path.
  this->base_path_ = pdrv == 0 ? "/sd" : "/sd" + std::to_string(pdrv);
  char drv[3] = {static_cast<char>('0' + pdrv), ':', 0};
  esp_vfs_fat_conf_t conf = {
      .base_path = this->base_path_.c_str(),
      .fat_drive = drv,
      .max_files = kMaxOpenFatFiles,
  };
  FATFS *fs = nullptr;
  esp_err_t err = esp_vfs_fat_register_cfg(&conf, &fs);
  if (err != ESP_OK || fs == nullptr) {
    ESP_LOGE(TAG, "esp_vfs_fat_register_cfg failed: %s", esp_err_to_name(err));
    ff_diskio_unregister(pdrv);
    s_fat_partitions[pdrv] = FatPartition{};
    return false;
  }

  const FRESULT res = f_mount(fs, drv, 1);
  if (res != FR_OK) {
    ESP_LOGE(TAG, "shared card FAT mount failed (%d)", static_cast<int>(res));
    esp_vfs_fat_unregister_path(this->base_path_.c_str());
    f_mount(nullptr, drv, 0);
    ff_diskio_unregister(pdrv);
    s_fat_partitions[pdrv] = FatPartition{};
    return false;
  }

  this->card_ = card;
  this->pdrv_ = pdrv;
  std::strncpy(this->drive_, drv, sizeof(this->drive_) - 1);
  this->external_mount_ = true;
  ESP_LOGI(TAG, "SD mounted at %s (shared card, FAT LBA %u+%u)", this->base_path_.c_str(),
           static_cast<unsigned>(start_sector), static_cast<unsigned>(sector_count));
  return true;
}
#endif

bool VfsSd::init_spi_() {
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

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  mount_config.max_files = kMaxOpenFatFiles;
  mount_config.allocation_unit_size = 16 * 1024;

  err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &mount_config, &this->card_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "SD mounted at /sd (SPI)");
  return true;
#else
  ESP_LOGE(TAG, "SD backend is only available on ESP-IDF");
  return false;
#endif
}

sd_pwr_ctrl_handle_t VfsSd::create_power_control_() {
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32P4)
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
#else
  return nullptr;
#endif
}

void VfsSd::release_power_control_() {
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32P4)
  if (this->power_control_ == nullptr)
    return;
  const esp_err_t err = sd_pwr_ctrl_del_on_chip_ldo(this->power_control_);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "sd_pwr_ctrl_del_on_chip_ldo failed: %s", esp_err_to_name(err));
  this->power_control_ = nullptr;
#endif
}

bool VfsSd::init_sdmmc_() {
#ifdef ESP_PLATFORM
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = this->config_.slot;
  host.max_freq_khz = this->config_.frequency / 1000;
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

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  mount_config.max_files = kMaxOpenFatFiles;
  mount_config.allocation_unit_size = 16 * 1024;

  const esp_err_t err =
      esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mount_config, &this->card_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SDMMC mount failed: %s", esp_err_to_name(err));
    this->release_power_control_();
    return false;
  }
  // Keep the power-control handle for the life of the mounted card: SDMMC may
  // call back into it when changing IO voltage after initialization.
  ESP_LOGI(TAG, "SD mounted at /sd (SDMMC slot %u, %u-bit)", this->config_.slot,
           this->config_.width);
  return true;
#else
  ESP_LOGE(TAG, "SD backend is only available on ESP-IDF");
  return false;
#endif
}

void VfsSd::shutdown() {
#ifdef ESP_PLATFORM
  if (this->external_mount_) {
    f_mount(nullptr, this->drive_, 0);
    esp_vfs_fat_unregister_path(this->base_path_.c_str());
    if (this->pdrv_ < FF_VOLUMES) {
      ff_diskio_unregister(this->pdrv_);
      s_fat_partitions[this->pdrv_] = FatPartition{};
    }
    this->external_mount_ = false;
  } else if (this->card_ != nullptr) {
    esp_vfs_fat_sdcard_unmount(this->base_path_.c_str(), this->card_);
    this->card_ = nullptr;
  }
#endif
  // Release power only after the filesystem has been unmounted and flushed.
  this->release_power_control_();
}

bool VfsSd::stat(const std::string &path, VfsStat *out) {
  struct stat st;
  if (::stat(this->full_(path).c_str(), &st) != 0) {
    out->exists = false;
    return true;
  }
  out->exists = true;
  out->is_dir = S_ISDIR(st.st_mode);
  out->size = static_cast<size_t>(st.st_size);
  out->mtime = st.st_mtime;
  return true;
}

bool VfsSd::read_dir(const std::string &path, std::vector<VfsDirEntry> *out) {
  DIR *dir = ::opendir(this->full_(path).c_str());
  if (dir == nullptr)
    return false;
  out->clear();
  while (dirent *entry = ::readdir(dir)) {
    std::string name = entry->d_name;
    if (name == "." || name == "..")
      continue;
    VfsDirEntry item;
    item.name = name;
    struct stat st;
    if (::stat(this->full_(path + "/" + name).c_str(), &st) == 0) {
      item.size = static_cast<size_t>(st.st_size);
      item.is_dir = S_ISDIR(st.st_mode);
      item.mtime = st.st_mtime;
    }
    out->push_back(std::move(item));
  }
  ::closedir(dir);
  return true;
}

bool VfsSd::read_file(const std::string &path, size_t offset, uint8_t *buf, size_t len,
                      size_t *read) {
  int fd = ::open(this->full_(path).c_str(), O_RDONLY);
  if (fd < 0)
    return false;
  if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
    ::close(fd);
    return false;
  }
  ssize_t n = ::read(fd, buf, len);
  ::close(fd);
  if (n < 0)
    return false;
  *read = static_cast<size_t>(n);
  return true;
}

bool VfsSd::write_file(const std::string &path, const uint8_t *buf, size_t len) {
  int fd = ::open(this->full_(path).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0)
    return false;
  size_t written = 0;
  while (written < len) {
    ssize_t n = ::write(fd, buf + written, len - written);
    if (n <= 0)
      break;
    written += static_cast<size_t>(n);
  }
  ::close(fd);
  return written == len;
}

bool VfsSd::mkdir(const std::string &path) {
  return ::mkdir(this->full_(path).c_str(), 0755) == 0;
}

bool VfsSd::remove(const std::string &path) {
  VfsStat st;
  if (stat(path, &st) && st.exists && st.is_dir)
    return ::rmdir(this->full_(path).c_str()) == 0;
  return ::unlink(this->full_(path).c_str()) == 0;
}

bool VfsSd::rename(const std::string &from, const std::string &to) {
  const std::string from_path = this->full_(from);
  const std::string to_path = this->full_(to);
  if (::rename(from_path.c_str(), to_path.c_str()) == 0)
    return true;

  // FatFs returns FR_EXIST instead of replacing an existing target. Preserve
  // the VFS replace-on-rename contract expected by PUT and the property store.
  struct stat source;
  struct stat target;
  if (::stat(from_path.c_str(), &source) != 0 || ::stat(to_path.c_str(), &target) != 0 ||
      S_ISDIR(source.st_mode) != S_ISDIR(target.st_mode))
    return false;
  const int removed = S_ISDIR(target.st_mode) ? ::rmdir(to_path.c_str())
                                              : ::unlink(to_path.c_str());
  if (removed != 0)
    return false;
  return ::rename(from_path.c_str(), to_path.c_str()) == 0;
}

}  // namespace webdav_filing
}  // namespace esphome
