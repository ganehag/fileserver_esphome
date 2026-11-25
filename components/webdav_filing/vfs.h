// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace esphome {
namespace webdav_filing {

struct VfsStat {
  bool exists{false};
  bool is_dir{false};
  size_t size{0};
  time_t mtime{0};  // POSIX seconds; 0 means "unknown".
};

struct VfsDirEntry {
  std::string name;
  size_t size{0};
  bool is_dir{false};
  time_t mtime{0};
};

enum class SdBus : uint8_t { SPI, SDMMC };

// One description of the SD card, shared by the FAT and ext2 backends.
// SPI keeps the original fields; SDMMC uses clk/cmd/d0-d3 and a 4-bit bus by
// default. pwr_ctrl_ldo_channel 0 means "board handles SD power"; Waveshare
// ESP32-P4 boards with the onboard LDO use channel 4.
struct SdConfig {
  SdBus bus{SdBus::SPI};
  int8_t clk{-1};
  int8_t miso{-1};
  int8_t mosi{-1};
  int8_t cs{-1};
  int8_t cmd{-1};
  int8_t d0{-1};
  int8_t d1{-1};
  int8_t d2{-1};
  int8_t d3{-1};
  uint8_t width{4};
  uint8_t slot{0};
  uint8_t pwr_ctrl_ldo_channel{0};
  uint32_t frequency{4000000};
};

// Everything above the storage layer talks to this, never to SD or ext2
// directly. WebDAV can therefore be developed and debugged against SD, then
// swapped onto ext2 without touching the protocol code.
class Vfs {
 public:
  virtual ~Vfs() = default;

  // Optional one-time setup for backends injected before WebDAVFiling::setup().
  virtual bool init() { return true; }

  // Transport errors return false. A missing path is not an error: out->exists
  // is set to false instead.
  virtual bool stat(const std::string &path, VfsStat *out) = 0;

  // True for router-created directories that have no writable storage behind
  // them. Protocol handlers use this to reject changes with 403 instead of a
  // misleading 500. Backends with a plain filesystem keep the default false.
  virtual bool is_virtual_collection(const std::string &) const { return false; }

  // True for paths the backend deliberately does not publish. Protocol handlers
  // answer as if the path did not exist instead of surfacing a backend error.
  virtual bool is_hidden(const std::string &) const { return false; }

  virtual bool read_dir(const std::string &path, std::vector<VfsDirEntry> *out) = 0;

  // Random-access read. Offset is absolute within the file.
  virtual bool read_file(const std::string &path, size_t offset, uint8_t *buf, size_t len,
                         size_t *read) = 0;

  // Append-only. Callers must remove/truncate before the first chunk of a new
  // file so this stays cheap on backends without a truncate() primitive.
  virtual bool write_file(const std::string &path, const uint8_t *buf, size_t len) = 0;

  virtual bool mkdir(const std::string &path) = 0;
  virtual bool remove(const std::string &path) = 0;
  virtual bool rename(const std::string &from, const std::string &to) = 0;

  // Optional durability/lifecycle hooks. Backends whose writes are already
  // synchronous can keep the defaults.
  virtual bool sync() { return true; }
  virtual void shutdown() {}
};

}  // namespace webdav_filing
}  // namespace esphome
