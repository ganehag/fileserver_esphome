// SPDX-License-Identifier: MIT
#pragma once

#include "sd_shared.h"
#include "vfs.h"
#include "vfs_router.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace esphome {
namespace webdav_filing {

// Auto mode discovers mountable volumes on the shared SD card and exposes them
// through VfsRouter: one volume is aliased at `/`, several become top-level
// virtual directories. Discovery never formats or partitions the card.
class VfsAuto : public Vfs {
 public:
  explicit VfsAuto(const SdConfig &config) : config_(config), card_(config) {}
  ~VfsAuto() override { this->shutdown(); }

  bool init() override;
  void shutdown() override;

  bool stat(const std::string &path, VfsStat *out) override;
  bool is_virtual_collection(const std::string &path) const override;
  bool is_hidden(const std::string &path) const override;
  bool read_dir(const std::string &path, std::vector<VfsDirEntry> *out) override;
  bool read_file(const std::string &path, size_t offset, uint8_t *buf, size_t len,
                 size_t *read) override;
  bool write_file(const std::string &path, const uint8_t *buf, size_t len) override;
  bool mkdir(const std::string &path) override;
  bool remove(const std::string &path) override;
  bool rename(const std::string &from, const std::string &to) override;
  bool sync() override;

  const VfsRouter &router() const { return router_; }

 protected:
#ifdef ESP_PLATFORM
  static bool read_sectors_(void *context, uint32_t lba, void *buffer, uint32_t count);
#endif
  std::string unique_name_(const char *preferred, const char *fallback) const;

  SdConfig config_;
  SharedSdCard card_;
  VfsRouter router_;
  std::vector<std::unique_ptr<Vfs>> backends_;
  bool initialized_{false};
};

}  // namespace webdav_filing
}  // namespace esphome
