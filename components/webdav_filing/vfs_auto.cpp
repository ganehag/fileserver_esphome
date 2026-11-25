// SPDX-License-Identifier: MIT
#include "vfs_auto.h"

#include "esphome/core/log.h"
#include "partition_scan.h"
#include "vfs_ext2.h"
#include "vfs_sd.h"

#include <algorithm>
#include <cctype>
#include <utility>

#ifdef ESP_PLATFORM
#include <sdmmc_cmd.h>
#endif

namespace esphome {
namespace webdav_filing {

static const char *const TAG = "vfs_auto";

#ifdef ESP_PLATFORM
bool VfsAuto::read_sectors_(void *context, uint32_t lba, void *buffer, uint32_t count) {
  auto *const card = static_cast<sdmmc_card_t *>(context);
  return sdmmc_read_sectors(card, buffer, lba, count) == ESP_OK;
}
#endif

std::string VfsAuto::unique_name_(const char *preferred, const char *fallback) const {
  std::string base;
  const std::string raw = preferred != nullptr ? std::string(preferred) : std::string();
  for (unsigned char c : raw) {
    if (base.size() >= 32)
      break;
    if (std::isalnum(c)) {
      base.push_back(static_cast<char>(std::tolower(c)));
    } else if (c == '-' || c == '_') {
      base.push_back(static_cast<char>(c));
    }
  }
  if (!VfsRouter::valid_name(base))
    base = fallback;

  auto exists = [this](const std::string &name) {
    for (const auto &mount : this->router_.mounts()) {
      if (mount.name == name)
        return true;
    }
    return false;
  };
  if (!exists(base))
    return base;
  for (uint32_t i = 2; i < 100; ++i) {
    const std::string candidate = base + "-" + std::to_string(i);
    if (!exists(candidate))
      return candidate;
  }
  return base;  // add() will reject the duplicate; this is a defensive fallback.
}

bool VfsAuto::init() {
  if (this->initialized_)
    return true;
#ifdef ESP_PLATFORM
  if (!this->card_.init()) {
    ESP_LOGE(TAG, "shared SD card init failed");
    return false;
  }
  if (this->card_.sector_size() != 512)
    ESP_LOGW(TAG, "partition scanner assumes 512-byte sectors; card reports %u",
             static_cast<unsigned>(this->card_.sector_size()));

  std::vector<PartitionSelection> partitions;
  if (!scan_partitions(this->card_.card(), VfsAuto::read_sectors_, this->card_.total_sectors(),
                       &partitions)) {
    ESP_LOGE(TAG, "partition scan failed");
    return false;
  }

  for (const auto &partition : partitions) {
    ESP_LOGI(TAG, "found %s volume in %s partition %u at LBA %u+%u%s%s",
             partition.ext2_magic ? "ext2" : "FAT", partition_table_name(partition.table),
             static_cast<unsigned>(partition.number),
             static_cast<unsigned>(partition.start_sector),
             static_cast<unsigned>(partition.sector_count),
             partition.name[0] != '\0' ? " name=" : "",
             partition.name[0] != '\0' ? partition.name : "");

    if (partition.ext2_magic) {
      auto backend = std::unique_ptr<VfsExt2>(
          new VfsExt2(this->config_, partition.start_sector, partition.sector_count));
      backend->set_partition_raw();
      backend->set_sd_card(this->card_.card());
      if (!backend->init()) {
        ESP_LOGW(TAG, "skipping ext2 volume: mount failed");
        continue;
      }
      const std::string name = this->unique_name_(partition.name, "ext2");
      if (!this->router_.add(name, backend.get())) {
        ESP_LOGE(TAG, "could not register ext2 volume as /%s", name.c_str());
        continue;
      }
      this->backends_.push_back(std::move(backend));
      continue;
    }

    if (partition.fat_magic) {
      auto backend = std::unique_ptr<VfsSd>(new VfsSd(this->config_));
      if (!backend->init_with_card(this->card_.card(), partition.start_sector,
                                   partition.sector_count)) {
        ESP_LOGW(TAG, "skipping FAT volume: mount failed");
        continue;
      }
      const std::string name = this->unique_name_(partition.name, "fat");
      if (!this->router_.add(name, backend.get())) {
        ESP_LOGE(TAG, "could not register FAT volume as /%s", name.c_str());
        continue;
      }
      this->backends_.push_back(std::move(backend));
    }
  }

  if (this->router_.mounts().empty()) {
    ESP_LOGE(TAG, "no mountable FAT or ext2 volume found");
    return false;
  }
  this->router_.finalize();
  this->initialized_ = true;
  if (this->router_.single_volume()) {
    ESP_LOGI(TAG, "auto mode: exposing /%s at the share root",
             this->router_.mounts().front().name.c_str());
  } else {
    ESP_LOGI(TAG, "auto mode: exposing %u volumes at the share root",
             static_cast<unsigned>(this->router_.mounts().size()));
  }
  return true;
#else
  ESP_LOGE(TAG, "auto backend is only available on ESP-IDF");
  return false;
#endif
}

void VfsAuto::shutdown() {
  this->router_.shutdown();
  this->card_.shutdown();
}

bool VfsAuto::stat(const std::string &path, VfsStat *out) {
  return this->router_.stat(path, out);
}

bool VfsAuto::is_virtual_collection(const std::string &path) const {
  return this->router_.is_virtual_collection(path);
}

bool VfsAuto::is_hidden(const std::string &path) const {
  return this->router_.is_hidden(path);
}

bool VfsAuto::read_dir(const std::string &path, std::vector<VfsDirEntry> *out) {
  return this->router_.read_dir(path, out);
}

bool VfsAuto::read_file(const std::string &path, size_t offset, uint8_t *buf, size_t len,
                        size_t *read) {
  return this->router_.read_file(path, offset, buf, len, read);
}

bool VfsAuto::write_file(const std::string &path, const uint8_t *buf, size_t len) {
  return this->router_.write_file(path, buf, len);
}

bool VfsAuto::mkdir(const std::string &path) {
  return this->router_.mkdir(path);
}

bool VfsAuto::remove(const std::string &path) {
  return this->router_.remove(path);
}

bool VfsAuto::rename(const std::string &from, const std::string &to) {
  return this->router_.rename(from, to);
}

bool VfsAuto::sync() {
  return this->router_.sync();
}

}  // namespace webdav_filing
}  // namespace esphome
