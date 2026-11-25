// SPDX-License-Identifier: MIT
#pragma once

#include "dav_xml.h"
#include "vfs.h"
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace webdav_filing {

struct DeadProperty {
  std::string key;  // "{namespace-uri}local-name"
  std::string xml;  // complete element as supplied by the client
};

std::string property_key(const std::string &uri, const std::string &local);

class DavPropStore {
 public:
  bool load(Vfs *vfs, const std::string &path = ".davprops");
  bool save(Vfs *vfs, const std::string &path = ".davprops") const;

  void set(const std::string &resource, const std::string &key, const std::string &xml);
  bool remove(const std::string &resource, const std::string &key);
  std::vector<DeadProperty> list(const std::string &resource) const;

  // Remove or re-key a resource and all descendants after DELETE/MOVE.
  size_t remove_resource(const std::string &resource);
  size_t rename_resource(const std::string &from, const std::string &to);

  void clear();

 private:
  // path -> (property key -> XML element)
  std::map<std::string, std::map<std::string, std::string>> props_;
};

}  // namespace webdav_filing
}  // namespace esphome
