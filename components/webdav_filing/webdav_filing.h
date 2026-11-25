// SPDX-License-Identifier: MIT
#pragma once

#include "dav_props.h"
#include "esphome/core/component.h"
#include "vfs.h"
#include <cstdint>
#include <ctime>
#include <esp_http_server.h>
#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace webdav_filing {

enum class WebDAVDepth : int8_t { ZERO = 0, ONE = 1, INFINITE = -1 };
enum class PropfindMode { ALL_PROPS, PROP_NAMES, NAMED_PROPS };
enum class CopyStatus { OK, FAIL, PARTIAL };

struct PropSpec {
  std::string uri;
  std::string local;
  std::string key;
};

struct PropfindRequest {
  PropfindMode mode{PropfindMode::ALL_PROPS};
  std::vector<PropSpec> props;
};

struct PropChange {
  bool set{true};
  PropSpec spec;
  std::string value;
};

class WebDAVFiling : public Component {
 public:
  WebDAVFiling(uint16_t port, int8_t clk, int8_t miso, int8_t mosi, int8_t cs, uint32_t frequency);
  WebDAVFiling(uint16_t port, const SdConfig &config) : port_(port), sd_(config) {}

  void setup() override;
  void on_shutdown() override;
  void loop() override {}  // esp_http_server serves requests on its own task
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Supply a different VFS before setup(), mainly for host tests.
  void set_vfs(Vfs *vfs) { vfs_ = vfs; }

  // Optional URL prefix, e.g. "/dav". The root "/" means no prefix.
  void set_base_path(const std::string &path) { base_path_ = normalize_base_path(path); }
  static std::string normalize_base_path(const std::string &path);

  // Empty username disables authentication. digest selects RFC 2617/7616 MD5
  // Digest; otherwise HTTP Basic is used. Credentials are supplied by YAML
  // secrets at codegen time.
  void set_auth(const std::string &username, const std::string &password, bool digest) {
    auth_username_ = username;
    auth_password_ = password;
    auth_type_ = username.empty() ? AuthType::NONE : (digest ? AuthType::DIGEST : AuthType::BASIC);
    this->prepare_auth_();
  }

 protected:
  enum class AuthType : uint8_t { NONE, BASIC, DIGEST };

  static std::string md5_hex_(const std::string &data);
  static bool base64_decode_(const std::string &input, std::string *output);
  static bool parse_auth_params_(const std::string &input,
                                 std::map<std::string, std::string> *params);
  static bool constant_time_equals_(const std::string &a, const std::string &b);
  void prepare_auth_();
  bool authorize_(httpd_req_t *req);
  esp_err_t unauthorized_(httpd_req_t *req, bool stale = false);
  struct WebDAVLock {
    std::string path;
    std::string token;
    std::string owner;
    bool exclusive{true};
    uint32_t expires_at{0};
  };

  static esp_err_t dispatch_(httpd_req_t *req);

#if defined(ESP_PLATFORM) && defined(USE_PSRAM) && !defined(WEBDAV_FILING_HOST_TEST)
  static bool start_async_get_workers_();
  static void stop_async_get_workers_();
  static bool queue_get_request_(WebDAVFiling *self, httpd_req_t *req, const std::string &path);
  static void async_get_worker_(void *arg);
#endif

  esp_err_t handle_(httpd_req_t *req);
  esp_err_t handle_benchmark_(httpd_req_t *req, size_t length, bool head);
  esp_err_t handle_get_(httpd_req_t *req, const std::string &path, bool head);
  esp_err_t handle_put_(httpd_req_t *req, const std::string &path);
  esp_err_t handle_delete_(httpd_req_t *req, const std::string &path);
  esp_err_t handle_mkcol_(httpd_req_t *req, const std::string &path);
  esp_err_t handle_propfind_(httpd_req_t *req, const std::string &path);
  esp_err_t handle_proppatch_(httpd_req_t *req, const std::string &path);
  esp_err_t handle_copy_move_(httpd_req_t *req, const std::string &from, bool move);
  esp_err_t handle_lock_(httpd_req_t *req, const std::string &path);
  esp_err_t handle_unlock_(httpd_req_t *req, const std::string &path);

  static esp_err_t send_status_(httpd_req_t *req, const char *status, const char *type,
                                const std::string &body);
  static esp_err_t send_head_(httpd_req_t *req, const char *status, const char *type,
                              size_t length);
  static esp_err_t send_empty_(httpd_req_t *req, const char *status);
  static esp_err_t send_close_(httpd_req_t *req, const char *status, const char *type,
                               const std::string &body);
  static esp_err_t send_dav_error_(httpd_req_t *req, const char *status, const char *condition,
                                   const char *message);
  static bool send_continue_(httpd_req_t *req);
  static bool handle_expect_(httpd_req_t *req, const std::string &expect);
  static std::string header_(httpd_req_t *req, const char *name);
  static bool read_body_(httpd_req_t *req, std::string *out, size_t max_len);

  // Removes the configured base path from a URL target. Returns "" when the
  // target is outside the share.
  std::string strip_base_path_(const std::string &target) const;
  std::string base_prefix_() const { return base_path_ == "/" ? "" : base_path_; }
  std::string href_(const std::string &path) const;

  // Sanitizes req->uri into a VFS-relative path (no leading slash, "" = root).
  // Also understands the legacy /fs/<name> route from the HTTP-only scaffold.
  // Returns false for traversal/invalid paths.
  bool file_path_(const std::string &uri, std::string *out);
  static bool benchmark_size_(const std::string &path, size_t *size);
  std::string destination_path_(const std::string &destination);
  static bool destination_matches_request_(const std::string &destination,
                                           const std::string &host_header);
  static std::string xml_escape_(const std::string &in);
  static std::string url_encode_path_(const std::string &in);
  static std::string http_date_(time_t mtime);
  static bool parse_http_date_(const std::string &in, time_t *out);
  static std::string etag_(const VfsStat &st);
  static bool iequals_(const std::string &a, const char *b);
  static bool token_list_has_(const std::string &header, const std::string &etag);
  static std::string strip_angles_(const std::string &in);
  static bool header_has_lock_token_(const std::string &header, const std::string &token);
  static bool if_header_allows_(const std::string &if_header, const std::string &token);
  static bool parse_depth_(const std::string &value, WebDAVDepth *depth);
  // 0 = no Range header, 1 = valid, -1 = unsatisfiable.
  static int parse_range_(const std::string &value, size_t total, size_t *start, size_t *end);
  static bool is_internal_path_(const std::string &path);

  bool remove_recursive_(const std::string &path);
  CopyStatus copy_recursive_(const std::string &from, const std::string &to, WebDAVDepth depth,
                             bool *created, int level = 0);

  void append_propstat_(std::string *out, const std::string &path, const VfsStat &st);
  void append_lock_properties_(std::string *out, const std::string &path);
  void append_lockdiscovery_(std::string *out, const std::string &path);
  void append_activelocks_(std::string *out, const std::string &path);
  void append_prop_for_spec_(std::string *out, const std::string &path, const VfsStat &st,
                             const PropSpec &spec, bool names_only);
  void append_all_props_(std::string *out, const std::string &path, const VfsStat &st,
                         bool names_only);
  bool live_prop_element_(const PropSpec &spec, const std::string &path, const VfsStat &st,
                          std::string *out);
  static std::string dead_prop_element_(const DeadProperty &prop);
  static esp_err_t flush_chunk_(httpd_req_t *req, std::string *buffer, bool force);
  esp_err_t send_propfind_resource_(httpd_req_t *req, std::string *buffer, const std::string &path,
                                    const VfsStat &st, const PropfindRequest &request, int depth,
                                    int level);
  static PropSpec prop_spec_(const std::string &uri, const std::string &local);
  static bool protected_prop_(const PropSpec &spec);
  static bool parse_propfind_body_(const std::string &body, PropfindRequest *out);
  static bool parse_proppatch_body_(const std::string &body, std::vector<PropChange> *out);

  void prune_locks_();
  WebDAVLock *find_lock_(const std::string &path, const std::string &token);
  const WebDAVLock *find_lock_(const std::string &path, const std::string &token) const;
  std::vector<WebDAVLock *> locks_for_(const std::string &path);
  bool request_has_lock_(const std::string &if_header, const std::string &lock_token_header,
                         const WebDAVLock &lock) const;
  bool path_locked_(const std::string &path, const std::string &if_header,
                    const std::string &lock_token_header);
  bool subtree_locked_(const std::string &path, const std::string &if_header,
                       const std::string &lock_token_header, int depth = 0);
  uint32_t parse_timeout_(const std::string &value) const;
  std::string next_lock_token_();
  std::string temp_path_(const std::string &target);
  bool prepare_temp_(const std::string &tmp);

  static constexpr uint32_t kDefaultLockSeconds = 3600;
  static constexpr uint32_t kMaxLockSeconds = 3600;
  static constexpr size_t kMaxLocks = 16;
  static constexpr int kMaxRecursion = 16;
  static constexpr size_t kMaxXmlBody = 32 * 1024;

  uint16_t port_;
  SdConfig sd_;
  std::string base_path_{"/"};
  AuthType auth_type_{AuthType::NONE};
  std::string auth_username_;
  std::string auth_password_;
  std::string auth_nonce_;
  std::string auth_ha1_;
  httpd_handle_t server_{nullptr};
  Vfs *vfs_{nullptr};
  Vfs *vfs_owned_{nullptr};
  httpd_uri_t uri_{};
  std::vector<WebDAVLock> locks_;
  uint32_t lock_counter_{0};
  uint32_t temp_counter_{0};
  DavPropStore props_;
};

}  // namespace webdav_filing
}  // namespace esphome
