// SPDX-License-Identifier: MIT
#pragma once

#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace webdav_filing {

// Small namespace-aware XML tree. WebDAV request bodies are tiny, so a full
// libxml2/expat dependency would cost far more than this parser.
struct XmlNode {
  std::string name;                       // raw qualified name, e.g. "D:prop"
  std::string prefix;
  std::string local;
  std::string uri;                        // resolved namespace
  std::map<std::string, std::string> attrs;
  std::string text;                       // direct character data, entity-decoded
  std::string raw;                        // complete element, exactly as received
  std::vector<XmlNode> children;
  bool self_closing{false};
};

bool parse_xml(const std::string &input, XmlNode *root);

const XmlNode *first_child(const XmlNode &node, const std::string &uri, const std::string &local);
std::vector<const XmlNode *> children(const XmlNode &node, const std::string &uri,
                                      const std::string &local);

std::string xml_unescape(const std::string &in);
std::string xml_escape(const std::string &in);

}  // namespace webdav_filing
}  // namespace esphome
