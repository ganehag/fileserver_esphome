#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <sys/types.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)

const char *esp_err_to_name(esp_err_t err);

enum http_method {
  HTTP_DELETE = 0,
  HTTP_GET = 1,
  HTTP_HEAD = 2,
  HTTP_POST = 3,
  HTTP_PUT = 4,
  HTTP_OPTIONS = 6,
  HTTP_COPY = 8,
  HTTP_LOCK = 9,
  HTTP_MKCOL = 10,
  HTTP_MOVE = 11,
  HTTP_PROPFIND = 12,
  HTTP_PROPPATCH = 13,
  HTTP_UNLOCK = 15,
};
typedef int httpd_method_t;
#define HTTP_ANY 2147483647

struct httpd_req_t {
  const char *uri{nullptr};
  int method{0};
  size_t content_len{0};
  void *user_ctx{nullptr};

  // Host-test-only fields.
  std::string body;
  size_t recv_offset{0};
  std::map<std::string, std::string> headers;
};

struct httpd_uri_t {
  const char *uri{nullptr};
  httpd_method_t method{0};
  esp_err_t (*handler)(httpd_req_t *){nullptr};
  void *user_ctx{nullptr};
};

struct httpd_config_t {
  uint16_t server_port{0};
  uint16_t task_priority{5};
  uint16_t max_open_sockets{7};
  uint16_t max_uri_handlers{0};
  uint16_t max_resp_headers{8};
  uint16_t backlog_conn{5};
  uint16_t recv_wait_timeout{5};
  uint16_t send_wait_timeout{5};
  bool lru_purge_enable{false};
  size_t stack_size{0};
  size_t max_req_hdr_len{1024};
  size_t max_uri_len{512};
  bool (*uri_match_fn)(const char *, const char *, size_t){nullptr};
};

#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}

typedef void *httpd_handle_t;

bool httpd_uri_match_wildcard(const char *uri_template, const char *uri_to_match, size_t match_upto);
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_stop(httpd_handle_t handle);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *value,
                                      size_t value_size);
int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len);
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *buf, ssize_t len);
