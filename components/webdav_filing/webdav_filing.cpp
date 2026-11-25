// SPDX-License-Identifier: MIT
#include "webdav_filing.h"

#include "dav_xml.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifndef WEBDAV_FILING_HOST_TEST
#include "vfs_auto.h"
#endif
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace esphome {
namespace webdav_filing {

static const char *const TAG = "webdav_filing";

// Set to 1 temporarily to log where a GET/PUT request spent its time.
#ifndef WEBDAV_FILING_PERF_DEBUG
#define WEBDAV_FILING_PERF_DEBUG 0
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

namespace {

const char *const kAuthRealm = "webdav_filing";

struct Md5Context {
  uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
  uint64_t bits{0};
  uint8_t block[64]{};
  size_t used{0};
};

const uint8_t kMd5Shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

const uint32_t kMd5K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

inline uint32_t md5_rotate_left(uint32_t value, uint8_t bits) {
  return (value << bits) | (value >> (32 - bits));
}

void md5_transform(Md5Context *ctx, const uint8_t block[64]) {
  uint32_t words[16];
  for (size_t i = 0; i < 16; i++) {
    words[i] = static_cast<uint32_t>(block[i * 4]) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
  }

  uint32_t a = ctx->state[0];
  uint32_t b = ctx->state[1];
  uint32_t c = ctx->state[2];
  uint32_t d = ctx->state[3];
  for (size_t i = 0; i < 64; i++) {
    uint32_t f;
    size_t g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    const uint32_t next = d;
    d = c;
    c = b;
    b += md5_rotate_left(a + f + kMd5K[i] + words[g], kMd5Shift[i]);
    a = next;
  }
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
}

void md5_update(Md5Context *ctx, const uint8_t *data, size_t len) {
  ctx->bits += static_cast<uint64_t>(len) * 8;
  while (len != 0) {
    const size_t take = std::min(static_cast<size_t>(64) - ctx->used, len);
    std::memcpy(ctx->block + ctx->used, data, take);
    ctx->used += take;
    data += take;
    len -= take;
    if (ctx->used == 64) {
      md5_transform(ctx, ctx->block);
      ctx->used = 0;
    }
  }
}

std::string md5_digest_hex(const std::string &data) {
  Md5Context ctx;
  md5_update(&ctx, reinterpret_cast<const uint8_t *>(data.data()), data.size());
  const uint64_t bit_length = ctx.bits;
  const uint8_t marker = 0x80;
  md5_update(&ctx, &marker, 1);
  const uint8_t zero = 0;
  while (ctx.used != 56)
    md5_update(&ctx, &zero, 1);
  uint8_t length[8];
  for (size_t i = 0; i < 8; i++)
    length[i] = static_cast<uint8_t>(bit_length >> (i * 8));
  md5_update(&ctx, length, sizeof(length));

  uint8_t digest[16];
  for (size_t i = 0; i < 4; i++) {
    digest[i * 4] = static_cast<uint8_t>(ctx.state[i]);
    digest[i * 4 + 1] = static_cast<uint8_t>(ctx.state[i] >> 8);
    digest[i * 4 + 2] = static_cast<uint8_t>(ctx.state[i] >> 16);
    digest[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i] >> 24);
  }
  static const char *const hex = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (uint8_t byte : digest) {
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0x0f]);
  }
  return out;
}

std::string ascii_lower(std::string value) {
  for (char &c : value) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c + ('a' - 'A'));
  }
  return value;
}

bool base64_byte(char c, uint8_t *value) {
  if (c >= 'A' && c <= 'Z') {
    *value = static_cast<uint8_t>(c - 'A');
    return true;
  }
  if (c >= 'a' && c <= 'z') {
    *value = static_cast<uint8_t>(c - 'a' + 26);
    return true;
  }
  if (c >= '0' && c <= '9') {
    *value = static_cast<uint8_t>(c - '0' + 52);
    return true;
  }
  if (c == '+') {
    *value = 62;
    return true;
  }
  if (c == '/') {
    *value = 63;
    return true;
  }
  return false;
}

const char *http_method_name(int method) {
  switch (method) {
    case HTTP_DELETE:
      return "DELETE";
    case HTTP_GET:
      return "GET";
    case HTTP_HEAD:
      return "HEAD";
    case HTTP_POST:
      return "POST";
    case HTTP_PUT:
      return "PUT";
    case HTTP_OPTIONS:
      return "OPTIONS";
    case HTTP_COPY:
      return "COPY";
    case HTTP_LOCK:
      return "LOCK";
    case HTTP_MKCOL:
      return "MKCOL";
    case HTTP_MOVE:
      return "MOVE";
    case HTTP_PROPFIND:
      return "PROPFIND";
    case HTTP_PROPPATCH:
      return "PROPPATCH";
    case HTTP_UNLOCK:
      return "UNLOCK";
    default:
      return "";
  }
}

}  // namespace

WebDAVFiling::WebDAVFiling(uint16_t port, int8_t clk, int8_t miso, int8_t mosi, int8_t cs,
                   uint32_t frequency)
    : WebDAVFiling(port, spi_config(clk, miso, mosi, cs, frequency)) {}

// File transfers use a heap buffer that is a multiple of ext2's 4096-byte
// block so each VFS call covers whole blocks. PUT accumulates short TCP reads
// into this buffer; GET/COPY issue larger reads to cut per-chunk lookup and
// indirect-block overhead. XML/control buffers stay at one block to keep
// first-byte latency low.
static constexpr size_t kFileBlockSize = 4096;
// PUT accumulates short TCP reads; 16 KiB ring slots measured best with the
// zero-copy async writer.
static constexpr size_t kPutBufferSize = 16 * 1024;
// GET/COPY buffer size can be overridden at build time for A/B benchmarks.
// 256 KiB of cache-aligned PSRAM measured best with the contiguous ext2 reader.
// Boards without PSRAM keep one block so they do not depend on a large internal
// heap allocation; every value is a multiple of the ext2 block size.
#ifndef WEBDAV_FILING_FILE_BUFFER_SIZE
#ifdef USE_PSRAM
#define WEBDAV_FILING_FILE_BUFFER_SIZE (256 * 1024)
#else
#define WEBDAV_FILING_FILE_BUFFER_SIZE 4096
#endif
#endif
static constexpr size_t kFileBufferSize = WEBDAV_FILING_FILE_BUFFER_SIZE;
static constexpr size_t kXmlBufferSize = kFileBlockSize;
static constexpr size_t kMaxFileSize = 4 * 1024 * 1024;

// Generated from __init__.py so the ESPHome lwIP socket budget, HTTPD session
// limit, and async GET worker pool stay in step. The fallback values are used
// only by standalone host builds.
#ifndef WEBDAV_FILING_HTTPD_MAX_OPEN_SOCKETS
#define WEBDAV_FILING_HTTPD_MAX_OPEN_SOCKETS 12
#endif
#ifndef WEBDAV_FILING_HTTPD_BACKLOG_CONN
#define WEBDAV_FILING_HTTPD_BACKLOG_CONN 16
#endif
#ifndef WEBDAV_FILING_HTTPD_MAX_URI_LEN
#define WEBDAV_FILING_HTTPD_MAX_URI_LEN 4096
#endif
#ifndef WEBDAV_FILING_HTTPD_MAX_REQ_HDR_LEN
#define WEBDAV_FILING_HTTPD_MAX_REQ_HDR_LEN 8192
#endif

// ESPHome's loopTask runs at priority 1 and owns the task watchdog. Keep the
// HTTPD, GET, and upload tasks at or below that priority; otherwise a long
// media scan can starve the loop past ESP_TASK_WDT_TIMEOUT_S.
constexpr int kWebDavTaskPriority = 1;

static uint8_t *allocate_transfer_buffer_(size_t bytes) {
#ifdef ESP_PLATFORM
  uint8_t *const psram = static_cast<uint8_t *>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED));
  if (psram != nullptr)
    return psram;
#endif
  return static_cast<uint8_t *>(malloc(bytes));
}

#if defined(ESP_PLATFORM) && defined(USE_PSRAM) && !defined(WEBDAV_FILING_HOST_TEST)
namespace {

// The HTTPD task must keep draining TCP while ext2 writes to SD. A PSRAM ring
// lets a writer task consume whole slots independently; without it every SD
// write stops lwIP from reading the socket and the client sees a stall.
constexpr size_t kUploadRingSize = 1024 * 1024;
std::mutex s_async_upload_mutex;

struct AsyncUploadJob {
  Vfs *vfs{nullptr};
  std::string temp;
  std::string target;
  uint8_t *ring{nullptr};
  size_t *slot_len{nullptr};
  size_t capacity{0};
  size_t slot_count{0};
  size_t head{0};
  size_t tail{0};
  size_t used{0};
  size_t content_len{0};
  bool producer_done{false};
  volatile bool producer_failed{false};
  volatile bool failed{false};
  SemaphoreHandle_t mutex{nullptr};
  SemaphoreHandle_t space{nullptr};
  SemaphoreHandle_t data{nullptr};
  SemaphoreHandle_t done{nullptr};
};

void async_upload_free(AsyncUploadJob *job) {
  if (job->ring != nullptr) heap_caps_free(job->ring);
  if (job->slot_len != nullptr) heap_caps_free(job->slot_len);
  if (job->space != nullptr) vSemaphoreDelete(job->space);
  if (job->data != nullptr) vSemaphoreDelete(job->data);
  if (job->mutex != nullptr) vSemaphoreDelete(job->mutex);
  if (job->done != nullptr) vSemaphoreDelete(job->done);
  *job = AsyncUploadJob{};
}

void async_upload_writer(void *arg) {
  AsyncUploadJob *const job = static_cast<AsyncUploadJob *>(arg);
  for (;;) {
    xSemaphoreTake(job->data, portMAX_DELAY);

    xSemaphoreTake(job->mutex, portMAX_DELAY);
    const bool finished = job->used == 0 && job->producer_done;
    if (finished) {
      xSemaphoreGive(job->mutex);
      break;
    }
    const size_t slot = job->head;
    const size_t len = job->slot_len[slot];
    const bool skip = job->producer_failed || job->failed;
    xSemaphoreGive(job->mutex);

    // The producer cannot reuse this slot until write_file() returns, so the
    // ext2 writer can consume the ring directly without a staging copy.
    bool ok = true;
    if (!skip)
      ok = job->vfs->write_file(job->temp, job->ring + slot * kPutBufferSize, len);

    xSemaphoreTake(job->mutex, portMAX_DELAY);
    if (!ok) job->failed = true;
    job->head = (job->head + 1) % job->slot_count;
    --job->used;
    xSemaphoreGive(job->space);
    xSemaphoreGive(job->mutex);
  }

  if (!job->failed && !job->producer_failed) {
    if (!job->vfs->sync() || !job->vfs->rename(job->temp, job->target))
      job->failed = true;
  } else {
    (void) job->vfs->remove(job->temp);
  }
  xSemaphoreGive(job->done);
  vTaskDelete(nullptr);
}

bool async_upload_wait(SemaphoreHandle_t sem, const AsyncUploadJob *job) {
  while (xSemaphoreTake(sem, pdMS_TO_TICKS(100)) != pdTRUE) {
    if (job->failed) return false;
  }
  return true;
}

// GET streams can block for a long time inside httpd_resp_send_chunk() when a
// client stops reading. HTTPD has one event loop task, so a stalled GET used to
// prevent PROPFINDs and new connections from being served. Give every client
// session its own worker so a burst of media probes never falls back to a
// synchronous GET on the HTTPD task.
constexpr UBaseType_t kAsyncGetWorkers = static_cast<UBaseType_t>(WEBDAV_FILING_HTTPD_MAX_OPEN_SOCKETS);
constexpr uint32_t kAsyncGetTaskStackBytes = 16 * 1024;
constexpr UBaseType_t kAsyncGetTaskPriority = kWebDavTaskPriority;

struct AsyncGetJob {
  WebDAVFiling *self;
  httpd_req_t *req;
  std::string path;
};

QueueHandle_t s_get_queue{nullptr};
SemaphoreHandle_t s_get_worker_ready{nullptr};
SemaphoreHandle_t s_get_workers_stopped{nullptr};
std::mutex s_get_control_mutex;
TaskHandle_t s_get_workers[kAsyncGetWorkers]{};
bool s_get_workers_started{false};
bool s_get_stopping{false};

void cleanup_async_get_primitives_() {
  if (s_get_queue != nullptr) {
    vQueueDelete(s_get_queue);
    s_get_queue = nullptr;
  }
  if (s_get_worker_ready != nullptr) {
    vSemaphoreDelete(s_get_worker_ready);
    s_get_worker_ready = nullptr;
  }
  if (s_get_workers_stopped != nullptr) {
    vSemaphoreDelete(s_get_workers_stopped);
    s_get_workers_stopped = nullptr;
  }
}

}  // namespace

bool WebDAVFiling::start_async_get_workers_() {
  std::lock_guard<std::mutex> lock(s_get_control_mutex);
  if (s_get_workers_started)
    return true;

  s_get_queue = xQueueCreate(kAsyncGetWorkers, sizeof(AsyncGetJob *));
  s_get_worker_ready = xSemaphoreCreateCounting(kAsyncGetWorkers, 0);
  s_get_workers_stopped = xSemaphoreCreateCounting(kAsyncGetWorkers, 0);
  if (s_get_queue == nullptr || s_get_worker_ready == nullptr || s_get_workers_stopped == nullptr) {
    cleanup_async_get_primitives_();
    return false;
  }

  UBaseType_t created = 0;
  for (; created < kAsyncGetWorkers; ++created) {
    if (xTaskCreateWithCaps(&WebDAVFiling::async_get_worker_, "dav-get", kAsyncGetTaskStackBytes,
                            nullptr, kAsyncGetTaskPriority, &s_get_workers[created],
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
      break;
    }
  }
  if (created != kAsyncGetWorkers) {
    for (UBaseType_t i = 0; i < created; ++i)
      vTaskDeleteWithCaps(s_get_workers[i]);
    cleanup_async_get_primitives_();
    return false;
  }

  for (UBaseType_t i = 0; i < kAsyncGetWorkers; ++i)
    xSemaphoreGive(s_get_worker_ready);
  s_get_stopping = false;
  s_get_workers_started = true;
  ESP_LOGI(TAG, "started %u async GET workers", static_cast<unsigned>(kAsyncGetWorkers));
  return true;
}

void WebDAVFiling::stop_async_get_workers_() {
  std::lock_guard<std::mutex> lock(s_get_control_mutex);
  if (!s_get_workers_started)
    return;
  s_get_stopping = true;

  AsyncGetJob *const stop_job = nullptr;
  const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
  for (UBaseType_t i = 0; i < kAsyncGetWorkers; ++i) {
    while (xQueueSend(s_get_queue, &stop_job, 0) != pdTRUE &&
           xTaskGetTickCount() < deadline) {
      // Do not busy-spin the loopTask: it owns the task watchdog.
      vTaskDelay(1);
    }
  }
  for (UBaseType_t i = 0; i < kAsyncGetWorkers; ++i) {
    const TickType_t now = xTaskGetTickCount();
    const TickType_t remaining = deadline > now ? deadline - now : 1;
    if (xSemaphoreTake(s_get_workers_stopped, remaining) != pdTRUE)
      ESP_LOGW(TAG, "async GET worker %u did not stop cleanly", static_cast<unsigned>(i));
  }
  for (UBaseType_t i = 0; i < kAsyncGetWorkers; ++i)
    vTaskDeleteWithCaps(s_get_workers[i]);

  cleanup_async_get_primitives_();
  s_get_workers_started = false;
  s_get_stopping = false;
}

bool WebDAVFiling::queue_get_request_(WebDAVFiling *self, httpd_req_t *req, const std::string &path) {
  std::lock_guard<std::mutex> lock(s_get_control_mutex);
  if (!s_get_workers_started || s_get_stopping)
    return false;
  if (xSemaphoreTake(s_get_worker_ready, 0) != pdTRUE)
    return false;

  httpd_req_t *async_req = nullptr;
  if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
    xSemaphoreGive(s_get_worker_ready);
    return false;
  }
  AsyncGetJob *const job = new (std::nothrow) AsyncGetJob{self, async_req, path};
  if (job == nullptr || xQueueSend(s_get_queue, &job, 0) != pdTRUE) {
    delete job;
    // The original request is no longer safe to use after async-handler-begin.
    // Consume it here instead of falling back to a synchronous GET.
    send_status_(async_req, "503 Service Unavailable", "text/plain", "GET worker unavailable");
    (void) httpd_req_async_handler_complete(async_req);
    xSemaphoreGive(s_get_worker_ready);
    return true;
  }
  return true;
}

void WebDAVFiling::async_get_worker_(void *arg) {
  (void) arg;
  for (;;) {
    AsyncGetJob *job = nullptr;
    if (xQueueReceive(s_get_queue, &job, portMAX_DELAY) != pdTRUE)
      continue;
    if (job == nullptr)
      break;

    const esp_err_t err = job->self->handle_get_(job->req, job->path, false);
    if (err == ESP_ERR_HTTPD_RESP_SEND) {
      // Media probes routinely close the socket once they have enough bytes.
      // Logging every early close at DEBUG floods the ESPHome logger.
      ESP_LOGV(TAG, "async GET client closed during response");
    } else if (err != ESP_OK) {
      ESP_LOGD(TAG, "async GET handler returned %s", esp_err_to_name(err));
    }
    if (err != ESP_OK) {
      // Never hand a half-written/desynchronized socket back to keep-alive.
      const int fd = httpd_req_to_sockfd(job->req);
      if (fd >= 0 && job->self->server_ != nullptr)
        httpd_sess_trigger_close(job->self->server_, fd);
    }
    if (httpd_req_async_handler_complete(job->req) != ESP_OK)
      ESP_LOGW(TAG, "failed to complete async GET request");
    delete job;
    xSemaphoreGive(s_get_worker_ready);
  }

  xSemaphoreGive(s_get_workers_stopped);
  // Stay suspended so stop_async_get_workers_() can free the PSRAM stack.
  vTaskSuspend(nullptr);
}
#endif

static int hex_val(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static std::string url_decode(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '%' && i + 2 < in.size()) {
      int hi = hex_val(in[i + 1]);
      int lo = hex_val(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(in[i]);
  }
  if (out.find('\0') != std::string::npos)
    return "";
  return out;
}

// URL-decodes, strips the query string, resolves . and .., and confines the
// result to the root. Returns "" for anything suspicious.
//
// ext2 allows 255-byte names; FAT allows 255 UTF-16 characters, which can be
// up to 765 bytes in UTF-8. Allow any path that can arrive in a request instead
// of silently rejecting long media filenames. Percent-encoding can triple the
// on-the-wire length, and the HTTP request line needs room for the method and
// version, so keep the HTTPD limits generated from __init__.py in step.
static constexpr size_t kMaxPathBytes = 1024;
static constexpr size_t kMaxSegmentBytes = 255 * 3;

static_assert(WEBDAV_FILING_HTTPD_MAX_URI_LEN >= kMaxPathBytes * 3 + 64,
              "HTTPD URI limit must fit a fully percent-encoded maximum path");
static_assert(WEBDAV_FILING_HTTPD_MAX_REQ_HDR_LEN > WEBDAV_FILING_HTTPD_MAX_URI_LEN,
              "HTTPD header limit must fit the request line and headers");

static std::string sanitize_path(const std::string &target) {
  std::string raw = target;
  size_t q = raw.find('?');
  if (q != std::string::npos)
    raw.erase(q);
  raw = url_decode(raw);
  if (raw.empty() || raw[0] != '/' || raw.size() > kMaxPathBytes)
    return "";

  std::vector<std::string> parts;
  size_t start = 1;
  while (start <= raw.size()) {
    size_t end = raw.find('/', start);
    if (end == std::string::npos)
      end = raw.size();
    std::string seg = raw.substr(start, end - start);
    if (seg == "..") {
      if (parts.empty())
        return "";
      parts.pop_back();
    } else if (!seg.empty() && seg != ".") {
      if (seg.size() > kMaxSegmentBytes)
        return "";
      parts.push_back(seg);
    }
    if (end == raw.size())
      break;
    start = end + 1;
  }
  if (parts.empty())
    return "/";

  std::string out;
  for (const auto &part : parts) {
    out.push_back('/');
    out += part;
  }
  return out;
}

[[maybe_unused]] static std::string html_escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

static std::string json_escape(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          const char *digits = "0123456789abcdef";
          out.push_back(digits[(c >> 4) & 0xf]);
          out.push_back(digits[c & 0xf]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

static std::string trim_copy(const std::string &s) {
  size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

static bool make_http_time(int year, int month, int day, int hour, int minute, int second,
                           time_t *out) {
  static const uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year < 1970 || year > 9999 || month < 1 || month > 12 || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 60)
    return false;
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  const int max_day = month == 2 ? (leap ? 29 : 28) : days_in_month[month - 1];
  if (day < 1 || day > max_day)
    return false;

  const int adjusted_year = year - (month <= 2 ? 1 : 0);
  const int64_t era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
  const int yoe = static_cast<int>(adjusted_year - era * 400);
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t seconds =
      (era * 146097 + doe - 719468) * 86400 + hour * 3600 + minute * 60 + second;
  *out = static_cast<time_t>(seconds);
  return true;
}

std::string WebDAVFiling::xml_escape_(const std::string &in) {
  return xml_escape(in);
}

std::string WebDAVFiling::url_encode_path_(const std::string &in) {
  static const char *digits = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : in) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(digits[c >> 4]);
      out.push_back(digits[c & 0xf]);
    }
  }
  return out;
}

std::string WebDAVFiling::http_date_(time_t mtime) {
  if (mtime <= 0)
    mtime = 0;
  struct tm tm_value {};
  if (gmtime_r(&mtime, &tm_value) == nullptr)
    return "Thu, 01 Jan 1970 00:00:00 GMT";
  char buf[40];
  if (strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_value) == 0)
    return "Thu, 01 Jan 1970 00:00:00 GMT";
  return buf;
}

bool WebDAVFiling::parse_http_date_(const std::string &in, time_t *out) {
  size_t comma = in.find(',');
  std::string value = comma == std::string::npos ? in : in.substr(comma + 1);
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  char month[8] = {}, zone[8] = {};
  if (std::sscanf(value.c_str(), " %d %7s %d %d:%d:%d %7s", &day, month, &year, &hour, &minute,
                  &second, zone) != 7)
    return false;
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int mon = 0;
  for (int i = 0; i < 12; i++) {
    if (std::strncmp(month, months[i], 3) == 0) {
      mon = i + 1;
      break;
    }
  }
  if (mon == 0)
    return false;
  if (year < 100)
    year += year < 70 ? 2000 : 1900;
  return make_http_time(year, mon, day, hour, minute, second, out);
}

std::string WebDAVFiling::etag_(const VfsStat &st) {
  return "\"" + std::to_string(st.size) + "-" + std::to_string(static_cast<long long>(st.mtime)) +
         "\"";
}

bool WebDAVFiling::iequals_(const std::string &a, const char *b) {
  size_t i = 0;
  for (; i < a.size() && b[i] != '\0'; i++) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return i == a.size() && b[i] == '\0';
}

bool WebDAVFiling::token_list_has_(const std::string &header, const std::string &etag) {
  if (header.empty())
    return false;
  std::string weak = etag;
  if (weak.rfind("W/", 0) == 0)
    weak = weak.substr(2);
  size_t start = 0;
  while (start <= header.size()) {
    size_t comma = header.find(',', start);
    std::string token = trim_copy(header.substr(start, comma == std::string::npos
                                                         ? std::string::npos
                                                         : comma - start));
    if (token.empty())
      start = comma == std::string::npos ? header.size() + 1 : comma + 1;
    if (token == "*")
      return true;
    if (token.rfind("W/", 0) == 0)
      token = token.substr(2);
    if (token == weak)
      return true;
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return false;
}

std::string WebDAVFiling::strip_angles_(const std::string &in) {
  size_t begin = in.find_first_not_of(" \t");
  if (begin == std::string::npos)
    return "";
  size_t end = in.find_last_not_of(" \t");
  std::string value = in.substr(begin, end - begin + 1);
  if (value.size() >= 2 && value.front() == '<' && value.back() == '>')
    return value.substr(1, value.size() - 2);
  return value;
}

bool WebDAVFiling::header_has_lock_token_(const std::string &header, const std::string &token) {
  if (token.empty())
    return false;
  return header.find("<" + token + ">") != std::string::npos;
}

bool WebDAVFiling::if_header_allows_(const std::string &if_header, const std::string &token) {
  if (token.empty())
    return false;
  const std::string needle = "<" + token + ">";
  size_t i = 0;
  while ((i = if_header.find('(', i)) != std::string::npos) {
    size_t depth = 1;
    size_t end = i + 1;
    for (; end < if_header.size() && depth > 0; ++end) {
      if (if_header[end] == '(')
        ++depth;
      else if (if_header[end] == ')')
        --depth;
    }
    if (depth != 0)
      return false;
    std::string group = if_header.substr(i + 1, end - i - 2);
    bool negated = false;
    size_t pos = 0;
    while (pos < group.size()) {
      while (pos < group.size() && std::isspace(static_cast<unsigned char>(group[pos])))
        ++pos;
      size_t word_end = pos;
      while (word_end < group.size() &&
             !std::isspace(static_cast<unsigned char>(group[word_end])))
        ++word_end;
      if (word_end == pos)
        break;
      const std::string word = group.substr(pos, word_end - pos);
      if (iequals_(word, "Not")) {
        negated = true;
      } else {
        if (word == needle && !negated)
          return true;
        negated = false;
      }
      pos = word_end;
    }
    i = end;
  }
  return false;
}

bool WebDAVFiling::parse_depth_(const std::string &value, WebDAVDepth *depth) {
  if (value.empty() || iequals_(value, "infinity")) {
    *depth = WebDAVDepth::INFINITE;
    return true;
  }
  if (value == "0") {
    *depth = WebDAVDepth::ZERO;
    return true;
  }
  if (value == "1") {
    *depth = WebDAVDepth::ONE;
    return true;
  }
  return false;
}

int WebDAVFiling::parse_range_(const std::string &value, size_t total, size_t *start, size_t *end) {
  if (value.empty())
    return 0;
  if (value.rfind("bytes=", 0) != 0 || value.find(',') != std::string::npos)
    return -1;
  std::string spec = value.substr(6);
  if (spec.empty() || total == 0)
    return -1;
  size_t dash = spec.find('-');
  if (dash == std::string::npos)
    return -1;
  std::string first = trim_copy(spec.substr(0, dash));
  std::string last = trim_copy(spec.substr(dash + 1));
  if (first.empty()) {
    char *endptr = nullptr;
    unsigned long suffix = std::strtoul(last.c_str(), &endptr, 10);
    if (endptr == last.c_str() || *endptr != '\0' || suffix == 0)
      return -1;
    *start = total > suffix ? total - static_cast<size_t>(suffix) : 0;
    *end = total - 1;
    return 1;
  }
  char *endptr = nullptr;
  unsigned long begin = std::strtoul(first.c_str(), &endptr, 10);
  if (endptr == first.c_str() || *endptr != '\0' || begin >= total)
    return -1;
  *start = static_cast<size_t>(begin);
  if (last.empty()) {
    *end = total - 1;
  } else {
    endptr = nullptr;
    unsigned long finish = std::strtoul(last.c_str(), &endptr, 10);
    if (endptr == last.c_str() || *endptr != '\0' || finish < begin)
      return -1;
    *end = static_cast<size_t>(std::min<unsigned long>(finish, total - 1));
  }
  return 1;
}

bool WebDAVFiling::is_internal_path_(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  return name == ".davprops" || name.rfind(".davprops.", 0) == 0 ||
         name.rfind(".davtmp", 0) == 0;
}

bool WebDAVFiling::destination_matches_request_(const std::string &destination,
                                            const std::string &host_header) {
  size_t scheme = destination.find("://");
  if (scheme == std::string::npos)
    return true;
  std::string scheme_name = destination.substr(0, scheme);
  std::transform(scheme_name.begin(), scheme_name.end(), scheme_name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (scheme_name != "http" && scheme_name != "https")
    return false;
  size_t authority_start = scheme + 3;
  size_t slash = destination.find('/', authority_start);
  std::string authority = destination.substr(authority_start, slash == std::string::npos
                                                                  ? std::string::npos
                                                                  : slash - authority_start);
  if (authority.empty())
    return false;
  if (host_header.empty())
    return true;

  auto split = [](const std::string &value, std::string *host, std::string *port) {
    size_t colon = value.rfind(':');
    if (colon != std::string::npos && colon + 1 < value.size() &&
        std::all_of(value.begin() + colon + 1, value.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
      *host = value.substr(0, colon);
      *port = value.substr(colon + 1);
    } else {
      *host = value;
      port->clear();
    }
    std::transform(host->begin(), host->end(), host->begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  };
  std::string host, port, req_host, req_port;
  split(authority, &host, &port);
  split(host_header, &req_host, &req_port);
  if (host != req_host)
    return false;
  if (!port.empty() && !req_port.empty() && port != req_port)
    return false;
  return true;
}

bool WebDAVFiling::benchmark_size_(const std::string &path, size_t *size) {
  if (path == "bench/512kb.bin") {
    *size = 512 * 1024;
    return true;
  }
  if (path == "bench/1mb.bin") {
    *size = 1024 * 1024;
    return true;
  }
  return false;
}

std::string WebDAVFiling::normalize_base_path(const std::string &path) {
  if (path.empty() || path == "/")
    return "/";
  std::vector<std::string> parts;
  size_t start = 1;
  while (start < path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos)
      end = path.size();
    const std::string part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..")
      return "/";
    parts.push_back(part);
    start = end + 1;
  }
  if (path[0] != '/' || parts.empty())
    return "/";
  std::string out;
  for (const auto &part : parts)
    out += "/" + part;
  return out;
}

std::string WebDAVFiling::strip_base_path_(const std::string &target) const {
  std::string value = target;
  const size_t query = value.find('?');
  if (query != std::string::npos)
    value.erase(query);
  if (this->base_path_ == "/")
    return value;
  if (value == this->base_path_)
    return "/";
  if (value.size() > this->base_path_.size() &&
      value.compare(0, this->base_path_.size(), this->base_path_) == 0 &&
      value[this->base_path_.size()] == '/')
    return value.substr(this->base_path_.size());
  return "";
}

std::string WebDAVFiling::href_(const std::string &path) const {
  return this->base_prefix_() + url_encode_path_("/" + path);
}

bool WebDAVFiling::file_path_(const std::string &uri, std::string *out) {
  const std::string stripped = this->strip_base_path_(uri);
  if (stripped.empty()) {
    out->clear();
    return false;
  }
  std::string clean = sanitize_path(stripped);
  if (clean.empty()) {
    out->clear();
    return false;
  }
  if (clean == "/") {
    out->clear();
    return true;
  }
  static const std::string legacy = "/fs/";
  if (clean.compare(0, legacy.size(), legacy) == 0)
    *out = clean.substr(legacy.size());
  else
    *out = clean.substr(1);
  return true;
}

std::string WebDAVFiling::destination_path_(const std::string &destination) {
  std::string value = destination;
  size_t scheme = value.find("://");
  if (scheme != std::string::npos) {
    size_t slash = value.find('/', scheme + 3);
    if (slash == std::string::npos)
      return "";
    value = value.substr(slash);
  }
  value = this->strip_base_path_(value);
  if (value.empty())
    return "";
  std::string clean = sanitize_path(value);
  if (clean.empty() || clean == "/")
    return "";
  return clean.substr(1);
}

std::string WebDAVFiling::header_(httpd_req_t *req, const char *name) {
  size_t len = httpd_req_get_hdr_value_len(req, name);
  if (len == 0)
    return "";
  std::string value(len + 1, '\0');
  if (httpd_req_get_hdr_value_str(req, name, &value[0], value.size()) != ESP_OK)
    return "";
  value.resize(len);
  return value;
}

std::string WebDAVFiling::md5_hex_(const std::string &data) {
  return md5_digest_hex(data);
}

bool WebDAVFiling::base64_decode_(const std::string &input, std::string *output) {
  output->clear();
  std::string token;
  for (char c : input) {
    if (!std::isspace(static_cast<unsigned char>(c)))
      token.push_back(c);
  }
  if (token.empty() || token.size() % 4 != 0)
    return false;

  for (size_t i = 0; i < token.size(); i += 4) {
    const char chars[4] = {token[i], token[i + 1], token[i + 2], token[i + 3]};
    uint8_t values[4] = {0, 0, 0, 0};
    bool padded = false;
    for (size_t j = 0; j < 4; j++) {
      if (chars[j] == '=') {
        // Padding is legal only in the final quantum, at positions 2 and 3.
        // Rejecting the second '=' broke credentials whose length is 1 mod 3,
        // e.g. "mikael:secret" -> "bWlrYWVsOnNlY3JldA==".
        if (j < 2 || (padded && j != 3))
          return false;
        padded = true;
      } else {
        if (padded || !base64_byte(chars[j], &values[j]))
          return false;
      }
    }
    if (chars[2] == '=' && chars[3] != '=')
      return false;

    const uint32_t bits = (static_cast<uint32_t>(values[0]) << 18) |
                          (static_cast<uint32_t>(values[1]) << 12) |
                          (static_cast<uint32_t>(values[2]) << 6) | values[3];
    output->push_back(static_cast<char>(bits >> 16));
    if (chars[2] != '=')
      output->push_back(static_cast<char>((bits >> 8) & 0xff));
    if (chars[3] != '=')
      output->push_back(static_cast<char>(bits & 0xff));
  }
  return true;
}

bool WebDAVFiling::parse_auth_params_(const std::string &input,
                                      std::map<std::string, std::string> *params) {
  size_t pos = 0;
  while (pos < input.size()) {
    while (pos < input.size() &&
           (std::isspace(static_cast<unsigned char>(input[pos])) || input[pos] == ','))
      pos++;
    if (pos == input.size())
      break;

    const size_t equals = input.find('=', pos);
    if (equals == std::string::npos || equals == pos)
      return false;
    const std::string key = ascii_lower(trim_copy(input.substr(pos, equals - pos)));
    if (key.empty())
      return false;
    pos = equals + 1;
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])))
      pos++;

    std::string value;
    if (pos < input.size() && input[pos] == '"') {
      pos++;
      bool closed = false;
      while (pos < input.size()) {
        if (input[pos] == '"') {
          pos++;
          closed = true;
          break;
        }
        if (input[pos] == '\\' && pos + 1 < input.size()) {
          value.push_back(input[pos + 1]);
          pos += 2;
        } else {
          value.push_back(input[pos++]);
        }
      }
      if (!closed)
        return false;
    } else {
      const size_t start = pos;
      while (pos < input.size() && input[pos] != ',')
        pos++;
      value = trim_copy(input.substr(start, pos - start));
    }
    (*params)[key] = value;

    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])))
      pos++;
    if (pos < input.size() && input[pos] != ',')
      return false;
  }
  return !params->empty();
}

bool WebDAVFiling::constant_time_equals_(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); i++)
    diff = static_cast<uint8_t>(diff | static_cast<uint8_t>(a[i] ^ b[i]));
  return diff == 0;
}

void WebDAVFiling::prepare_auth_() {
  if (this->auth_type_ != AuthType::DIGEST) {
    this->auth_nonce_.clear();
    this->auth_ha1_.clear();
    return;
  }
#ifdef WEBDAV_FILING_HOST_TEST
  this->auth_nonce_ = "testnonce";
#else
  char nonce[17];
  std::snprintf(nonce, sizeof(nonce), "%08x%08x",
                static_cast<unsigned>(esp_random()), static_cast<unsigned>(esp_random()));
  this->auth_nonce_ = nonce;
#endif
  this->auth_ha1_ = this->md5_hex_(this->auth_username_ + ":" + kAuthRealm + ":" +
                                  this->auth_password_);
}

esp_err_t WebDAVFiling::unauthorized_(httpd_req_t *req, bool stale) {
  char challenge[256];
  if (this->auth_type_ == AuthType::DIGEST) {
    std::snprintf(challenge, sizeof(challenge),
                  "Digest realm=\"%s\", nonce=\"%s\", algorithm=MD5, qop=\"auth\"%s", kAuthRealm,
                  this->auth_nonce_.c_str(), stale ? ", stale=true" : "");
  } else {
    std::snprintf(challenge, sizeof(challenge), "Basic realm=\"%s\"", kAuthRealm);
  }
  httpd_resp_set_hdr(req, "WWW-Authenticate", challenge);
  return this->send_empty_(req, "401 Unauthorized");
}

bool WebDAVFiling::authorize_(httpd_req_t *req) {
  if (this->auth_type_ == AuthType::NONE)
    return true;

  const std::string header = this->header_(req, "Authorization");
  if (header.empty()) {
    this->unauthorized_(req);
    return false;
  }
  const size_t space = header.find(' ');
  const std::string scheme =
      ascii_lower(space == std::string::npos ? header : header.substr(0, space));
  const std::string value = space == std::string::npos ? "" : trim_copy(header.substr(space + 1));

  if (scheme == "basic" && this->auth_type_ == AuthType::BASIC) {
    std::string decoded;
    const std::string expected = this->auth_username_ + ":" + this->auth_password_;
    if (this->base64_decode_(value, &decoded) &&
        this->constant_time_equals_(decoded, expected))
      return true;
    this->unauthorized_(req);
    return false;
  }

  if (scheme == "digest" && this->auth_type_ == AuthType::DIGEST) {
    std::map<std::string, std::string> params;
    auto field = [&params](const char *name) {
      const auto it = params.find(name);
      return it == params.end() ? std::string() : it->second;
    };
#ifdef WEBDAV_FILING_HOST_TEST
    const std::string uri = req->uri == nullptr ? "" : req->uri;
#else
    // ESP-IDF declares httpd_req_t::uri as a fixed-size array, so it is never null.
    const std::string uri = req->uri;
#endif
    const char *method = http_method_name(req->method);
    if (!this->parse_auth_params_(value, &params)) {
      this->unauthorized_(req);
      return false;
    }
    const bool nonce_ok = field("nonce") == this->auth_nonce_;
    const bool valid = nonce_ok && field("username") == this->auth_username_ &&
                       field("realm") == kAuthRealm && field("uri") == uri && method[0] != '\0' &&
                       (ascii_lower(field("algorithm")).empty() ||
                        ascii_lower(field("algorithm")) == "md5");
    if (!valid) {
      this->unauthorized_(req, !nonce_ok);
      return false;
    }

    const std::string qop = ascii_lower(field("qop"));
    const std::string ha2 = this->md5_hex_(std::string(method) + ":" + uri);
    std::string expected;
    if (qop.empty()) {
      expected = this->md5_hex_(this->auth_ha1_ + ":" + this->auth_nonce_ + ":" + ha2);
    } else if (qop == "auth") {
      const std::string nc = field("nc");
      const std::string cnonce = field("cnonce");
      if (nc.empty() || cnonce.empty()) {
        this->unauthorized_(req);
        return false;
      }
      expected = this->md5_hex_(this->auth_ha1_ + ":" + this->auth_nonce_ + ":" + nc + ":" +
                                cnonce + ":auth:" + ha2);
    } else {
      this->unauthorized_(req);
      return false;
    }
    if (this->constant_time_equals_(expected, ascii_lower(field("response"))))
      return true;
  }

  this->unauthorized_(req);
  return false;
}

bool WebDAVFiling::read_body_(httpd_req_t *req, std::string *out, size_t max_len) {
  out->clear();
  if (req->content_len > max_len) {
    send_status_(req, "413 Payload Too Large", "text/plain", "body too large");
    return false;
  }
  out->resize(req->content_len);
  size_t offset = 0;
  while (offset < req->content_len) {
    size_t want = std::min(kXmlBufferSize, req->content_len - offset);
    int got = httpd_req_recv(req, &(*out)[offset], want);
    if (got <= 0) {
      send_status_(req, "400 Bad Request", "text/plain", "body read failed");
      return false;
    }
    offset += static_cast<size_t>(got);
  }
  return true;
}

esp_err_t WebDAVFiling::send_status_(httpd_req_t *req, const char *status, const char *type,
                                 const std::string &body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, type);
  // IDF has no HEAD handling: pass the representation length but no buffer so
  // it emits exactly one correct Content-Length and no body.
  if (req->method == HTTP_HEAD)
    return httpd_resp_send(req, nullptr, static_cast<ssize_t>(body.size()));
  return httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}

esp_err_t WebDAVFiling::send_head_(httpd_req_t *req, const char *status, const char *type,
                                   size_t length) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, type);
  return httpd_resp_send(req, nullptr, static_cast<ssize_t>(length));
}

esp_err_t WebDAVFiling::send_empty_(httpd_req_t *req, const char *status) {
  httpd_resp_set_status(req, status);
  return httpd_resp_send(req, nullptr, 0);
}

esp_err_t WebDAVFiling::send_close_(httpd_req_t *req, const char *status, const char *type,
                                    const std::string &body) {
  send_status_(req, status, type, body);
  // Returning an error makes IDF close the socket without trying to purge a
  // potentially multi-megabyte body 32 bytes at a time.
  return ESP_FAIL;
}

esp_err_t WebDAVFiling::send_dav_error_(httpd_req_t *req, const char *status, const char *condition,
                                    const char *message) {
  std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                     "<D:error xmlns:D=\"DAV:\"><D:";
  body += condition;
  body += "/><D:message>" + xml_escape_(message) + "</D:message></D:error>\n";
  return send_status_(req, status, "application/xml; charset=utf-8", body);
}

bool WebDAVFiling::send_continue_(httpd_req_t *req) {
#if defined(ESP_PLATFORM) && !defined(WEBDAV_FILING_HOST_TEST)
  // IDF has no interim-response API and httpd_resp_send() would add entity
  // headers to a 1xx response. Write the interim line directly.
  static const char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
  const int fd = httpd_req_to_sockfd(req);
  if (fd < 0)
    return false;
  size_t sent = 0;
  while (sent < sizeof(kContinue) - 1) {
    const int wrote = ::send(fd, kContinue + sent, sizeof(kContinue) - 1 - sent, 0);
    if (wrote <= 0)
      return false;
    sent += static_cast<size_t>(wrote);
  }
  return true;
#else
  httpd_resp_set_status(req, "100 Continue");
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, nullptr, 0) == ESP_OK;
#endif
}

bool WebDAVFiling::handle_expect_(httpd_req_t *req, const std::string &expect) {
  if (expect.empty())
    return true;
  if (iequals_(expect, "100-continue"))
    return send_continue_(req);
  send_status_(req, "417 Expectation Failed", "text/plain", "unsupported Expect header");
  return false;
}

esp_err_t WebDAVFiling::flush_chunk_(httpd_req_t *req, std::string *buffer, bool force) {
  if (buffer->empty() || (!force && buffer->size() < kXmlBufferSize))
    return ESP_OK;
  esp_err_t err = httpd_resp_send_chunk(req, buffer->data(), static_cast<ssize_t>(buffer->size()));
  buffer->clear();
  return err;
}

PropSpec WebDAVFiling::prop_spec_(const std::string &uri, const std::string &local) {
  return PropSpec{uri, local, property_key(uri, local)};
}

bool WebDAVFiling::protected_prop_(const PropSpec &spec) {
  if (spec.uri != "DAV:")
    return false;
  static const char *const kProtected[] = {"displayname",       "getcontentlength",
                                           "getetag",           "getlastmodified",
                                           "creationdate",      "resourcetype",
                                           "supportedlock",     "lockdiscovery"};
  for (const char *local : kProtected) {
    if (spec.local == local)
      return true;
  }
  return false;
}

std::string WebDAVFiling::dead_prop_element_(const DeadProperty &prop) {
  size_t close = prop.key.find('}');
  if (close == std::string::npos)
    return prop.xml;
  std::string uri = prop.key.substr(1, close - 1);
  std::string local = prop.key.substr(close + 1);

  size_t name_start = prop.xml.find('<');
  if (name_start == std::string::npos)
    return "<D:" + local + "/>";
  size_t name_end = name_start + 1;
  while (name_end < prop.xml.size()) {
    char c = prop.xml[name_end];
    if (std::isspace(static_cast<unsigned char>(c)) || c == '>' || c == '/')
      break;
    name_end++;
  }
  std::string name = prop.xml.substr(name_start + 1, name_end - name_start - 1);
  size_t colon = name.find(':');
  std::string prefix = colon == std::string::npos ? "" : name.substr(0, colon);
  std::string decl_name = prefix.empty() ? "xmlns" : "xmlns:" + prefix;
  if (prop.xml.find(decl_name) != std::string::npos)
    return prop.xml;

  std::string declaration = " " + decl_name + "=\"" + xml_escape(uri) + "\" ";
  size_t insert = prop.xml.find('>');
  if (insert == std::string::npos)
    return prop.xml;
  if (insert > 0 && prop.xml[insert - 1] == '/')
    insert--;
  std::string out = prop.xml;
  out.insert(insert, declaration);
  out += local.empty() ? "" : "";  // keep local referenced for clarity
  return out;
}

bool WebDAVFiling::parse_propfind_body_(const std::string &body, PropfindRequest *out) {
  if (body.empty())
    return true;
  XmlNode root;
  if (!parse_xml(body, &root) || root.local != "propfind")
    return false;

  if (first_child(root, "DAV:", "allprop") != nullptr) {
    out->mode = PropfindMode::ALL_PROPS;
    const XmlNode *include = first_child(root, "DAV:", "include");
    if (include != nullptr) {
      for (const auto &child : include->children)
        out->props.push_back(prop_spec_(child.uri, child.local));
    }
    return true;
  }
  if (first_child(root, "DAV:", "propname") != nullptr) {
    out->mode = PropfindMode::PROP_NAMES;
    return true;
  }
  const XmlNode *prop = first_child(root, "DAV:", "prop");
  if (prop == nullptr)
    return false;
  out->mode = PropfindMode::NAMED_PROPS;
  for (const auto &child : prop->children)
    out->props.push_back(prop_spec_(child.uri, child.local));
  return true;
}

bool WebDAVFiling::parse_proppatch_body_(const std::string &body, std::vector<PropChange> *out) {
  XmlNode root;
  if (body.empty() || !parse_xml(body, &root) || root.local != "propertyupdate")
    return false;

  for (const auto &operation : root.children) {
    if (operation.uri != "DAV:" || (operation.local != "set" && operation.local != "remove"))
      continue;
    bool set = operation.local == "set";
    const XmlNode *prop = first_child(operation, "DAV:", "prop");
    if (prop == nullptr)
      continue;
    for (const auto &item : prop->children) {
      PropChange change;
      change.set = set;
      change.spec = prop_spec_(item.uri, item.local);
      change.value = item.raw;
      out->push_back(std::move(change));
    }
  }
  return true;
}

bool WebDAVFiling::live_prop_element_(const PropSpec &spec, const std::string &path,
                                  const VfsStat &st, std::string *out) {
  if (spec.uri != "DAV:")
    return false;
  std::string name = "davbox";
  if (!path.empty()) {
    size_t slash = path.find_last_of('/');
    name = slash == std::string::npos ? path : path.substr(slash + 1);
  }

  if (spec.local == "displayname") {
    *out = "<D:displayname>" + xml_escape_(name) + "</D:displayname>";
  } else if (spec.local == "getcontentlength") {
    *out = "<D:getcontentlength>" + std::to_string(st.size) + "</D:getcontentlength>";
  } else if (spec.local == "getetag") {
    *out = "<D:getetag>" + xml_escape_(this->etag_(st)) + "</D:getetag>";
  } else if (spec.local == "getlastmodified" || spec.local == "creationdate") {
    if (spec.local == "getlastmodified") {
      *out = "<D:getlastmodified>" + this->http_date_(st.mtime) + "</D:getlastmodified>";
    } else {
      time_t mtime = st.mtime > 0 ? st.mtime : 0;
      struct tm tm_value {};
      gmtime_r(&mtime, &tm_value);
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_value);
      *out = "<D:creationdate>" + std::string(buf) + "</D:creationdate>";
    }
  } else if (spec.local == "resourcetype") {
    *out = "<D:resourcetype>";
    if (st.is_dir)
      *out += "<D:collection/>";
    *out += "</D:resourcetype>";
  } else if (spec.local == "supportedlock") {
    *out = "<D:supportedlock><D:lockentry><D:lockscope><D:exclusive/></D:lockscope>"
           "<D:locktype><D:write/></D:locktype></D:lockentry>"
           "<D:lockentry><D:lockscope><D:shared/></D:lockscope>"
           "<D:locktype><D:write/></D:locktype></D:lockentry></D:supportedlock>";
  } else if (spec.local == "lockdiscovery") {
    this->append_lockdiscovery_(out, path);
  } else {
    return false;
  }
  return true;
}

void WebDAVFiling::append_lock_properties_(std::string *out, const std::string &path) {
  this->append_lockdiscovery_(out, path);
}

void WebDAVFiling::append_lockdiscovery_(std::string *out, const std::string &path) {
  std::string active;
  this->append_activelocks_(&active, path);
  if (active.empty())
    *out += "<D:lockdiscovery/>";
  else
    *out += "<D:lockdiscovery>" + active + "</D:lockdiscovery>";
}

void WebDAVFiling::append_activelocks_(std::string *out, const std::string &path) {
  auto locks = this->locks_for_(path);
  for (const WebDAVLock *lock : locks) {
    uint32_t remaining = 0;
    uint32_t now = esphome::millis();
    if (static_cast<int32_t>(lock->expires_at - now) > 0)
      remaining = (lock->expires_at - now) / 1000;
    *out += "<D:activelock>"
            "<D:locktype><D:write/></D:locktype>"
            "<D:lockscope><D:";
    *out += lock->exclusive ? "exclusive/>" : "shared/>";
    *out += "</D:lockscope>"
            "<D:depth>0</D:depth>"
            "<D:timeout>Second-" +
            std::to_string(remaining) + "</D:timeout>";
    if (!lock->owner.empty())
      *out += "<D:owner>" + xml_escape_(lock->owner) + "</D:owner>";
    *out += "<D:locktoken><D:href>" + xml_escape_(lock->token) +
            "</D:href></D:locktoken></D:activelock>";
  }
}

void WebDAVFiling::append_prop_for_spec_(std::string *out, const std::string &path,
                                     const VfsStat &st, const PropSpec &spec, bool names_only) {
  std::string element;
  bool found = this->live_prop_element_(spec, path, st, &element);
  if (!found) {
    for (const auto &dead : this->props_.list(path)) {
      if (dead.key == spec.key) {
        found = true;
        element = dead_prop_element_(dead);
        break;
      }
    }
  }
  if (!found) {
    std::string name = "<" + (spec.uri == "DAV:" ? std::string("D:") + spec.local
                                                 : std::string("P:") + spec.local);
    if (spec.uri != "DAV:")
      name += " xmlns:P=\"" + xml_escape_(spec.uri) + "\"";
    element = name + "/>";
  }
  if (names_only) {
    size_t space = element.find(' ');
    size_t close = element.find('>');
    std::string name = element.substr(1, std::min(space, close) - 1);
    element = name + "/>";
  }
  *out += "<D:propstat><D:prop>" + element + "</D:prop><D:status>HTTP/1.1 " +
          (found ? "200 OK" : "404 Not Found") + "</D:status></D:propstat>";
}

void WebDAVFiling::append_all_props_(std::string *out, const std::string &path, const VfsStat &st,
                                 bool names_only) {
  static const char *const kLive[] = {"displayname",        "getcontentlength", "getetag",
                                      "getlastmodified",    "creationdate",     "resourcetype",
                                      "supportedlock",      "lockdiscovery"};
  std::string props;
  for (const char *local : kLive) {
    std::string element;
    if (this->live_prop_element_(prop_spec_("DAV:", local), path, st, &element)) {
      if (names_only)
        props += "<D:" + std::string(local) + "/>";
      else
        props += element;
    }
  }
  for (const auto &dead : this->props_.list(path)) {
    if (!names_only) {
      props += dead_prop_element_(dead);
      continue;
    }
    size_t close = dead.key.find('}');
    if (close == std::string::npos)
      continue;
    const std::string uri = dead.key.substr(1, close - 1);
    const std::string local = dead.key.substr(close + 1);
    props += "<P:" + local + " xmlns:P=\"" + xml_escape_(uri) + "\"/>";
  }

  *out += "<D:propstat><D:prop>" + props + "</D:prop><D:status>HTTP/1.1 200 OK</D:status>"
          "</D:propstat>";
}

void WebDAVFiling::append_propstat_(std::string *out, const std::string &path, const VfsStat &st) {
  this->append_all_props_(out, path, st, false);
}

esp_err_t WebDAVFiling::send_propfind_resource_(httpd_req_t *req, std::string *buffer,
                                            const std::string &path, const VfsStat &st,
                                            const PropfindRequest &request, int depth, int level) {
  std::string body = "<D:response><D:href>" + this->href_(path) + "</D:href>";
  if (request.mode == PropfindMode::NAMED_PROPS) {
    for (const auto &spec : request.props)
      this->append_prop_for_spec_(&body, path, st, spec, false);
    if (request.props.empty())
      body += "<D:propstat><D:prop/><D:status>HTTP/1.1 200 OK</D:status></D:propstat>";
  } else if (request.mode == PropfindMode::PROP_NAMES) {
    this->append_all_props_(&body, path, st, true);
  } else {
    this->append_all_props_(&body, path, st, false);
    for (const auto &spec : request.props)
      this->append_prop_for_spec_(&body, path, st, spec, false);
  }
  body += "</D:response>";
  *buffer += body;
  esp_err_t err = flush_chunk_(req, buffer, false);
  if (err != ESP_OK)
    return err;
  if (depth == 0 || !st.is_dir || level >= kMaxRecursion)
    return ESP_OK;
  if (depth == static_cast<int>(WebDAVDepth::ZERO))
    return ESP_OK;

  std::vector<VfsDirEntry> entries;
  if (!vfs_->read_dir(path, &entries))
    return ESP_OK;
  for (const auto &entry : entries) {
    if (is_internal_path_(entry.name))
      continue;
    std::string child = path.empty() ? entry.name : path + "/" + entry.name;
    // read_dir() already carries size/mtime/type. Avoid a second filesystem
    // round-trip for every child during large PROPFIND scans.
    VfsStat child_st;
    child_st.exists = true;
    child_st.is_dir = entry.is_dir;
    child_st.size = entry.size;
    child_st.mtime = entry.mtime;
    int child_depth = depth == static_cast<int>(WebDAVDepth::INFINITE)
                          ? static_cast<int>(WebDAVDepth::INFINITE)
                          : depth - 1;
    err = this->send_propfind_resource_(req, buffer, child, child_st, request, child_depth,
                                        level + 1);
    if (err != ESP_OK)
      return err;
  }
  return ESP_OK;
}

esp_err_t WebDAVFiling::handle_benchmark_(httpd_req_t *req, size_t length, bool head) {
  // RAM-only transfer: isolates Wi-Fi/lwIP/HTTPD send cost from ext2 and SD.
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (head)
    return send_head_(req, "200 OK", "application/octet-stream", length);

#if WEBDAV_FILING_PERF_DEBUG
  const uint32_t start = millis();
#endif
  uint8_t *const buf = allocate_transfer_buffer_(kFileBufferSize);
  if (buf == nullptr)
    return send_status_(req, "500 Internal Server Error", "text/plain", "alloc failed");
  for (size_t i = 0; i < kFileBufferSize; ++i)
    buf[i] = static_cast<uint8_t>(i);
  size_t remaining = length;
  while (remaining != 0) {
    const size_t chunk = std::min(kFileBufferSize, remaining);
    const esp_err_t err =
        httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buf),
                              static_cast<ssize_t>(chunk));
    if (err != ESP_OK) {
      free(buf);
      return err;
    }
    remaining -= chunk;
  }
  free(buf);
#if WEBDAV_FILING_PERF_DEBUG
  ESP_LOGI(TAG, "BENCH perf: bytes=%u total=%ums", static_cast<unsigned>(length),
           static_cast<unsigned>(millis() - start));
#endif
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t WebDAVFiling::handle_get_(httpd_req_t *req, const std::string &path, bool head) {
  size_t benchmark_size = 0;
  if (benchmark_size_(path, &benchmark_size))
    return this->handle_benchmark_(req, benchmark_size, head);
  if (is_internal_path_(path))
    return send_status_(req, "404 Not Found", "text/plain", "no such file");
  if (path.empty()) {
    const std::string prefix = this->base_prefix_();
    static const char kIndex[] = R"HTML(<!doctype html>
<meta charset="utf-8"><title>davbox</title>
<style>body{font:14px system-ui;margin:2rem}li{margin:.3rem 0}</style>
<h1>davbox</h1>
<p>WebDAV file server on ESP-IDF. Plain HTTP UI shown here.</p>
<form id="f"><input type="file" id="file"><button>upload</button></form>
<ul id="l"></ul>
<script>
function esc(s){return s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function load(){
  const r = await fetch('@BASE@/list');
  const j = await r.json();
  document.getElementById('l').innerHTML = j.map(x =>
    '<li>' + (x.dir ? '[dir] ' : '') + esc(x.name) + ' (' + x.size + ' B)' +
    (x.dir ? '' : ' <a href="@BASE@/fs/' + encodeURIComponent(x.name) + '">get</a>') +
    ' <button onclick="del(' + JSON.stringify(x.name) + ')">del</button></li>'
  ).join('');
}
async function del(name){
  await fetch('@BASE@/fs/' + encodeURIComponent(name), {method:'DELETE'});
  load();
}
document.getElementById('f').onsubmit = async e => {
  e.preventDefault();
  const file = document.getElementById('file').files[0];
  if (!file) return;
  await fetch('@BASE@/fs/' + encodeURIComponent(file.name), {method:'PUT', body:file});
  load();
};
load();
</script>
)HTML";
    std::string html = kIndex;
    size_t pos = 0;
    while ((pos = html.find("@BASE@", pos)) != std::string::npos) {
      html.replace(pos, 6, prefix);
      pos += prefix.size();
    }
    if (head)
      return send_head_(req, "200 OK", "text/html; charset=utf-8", html.size());
    return send_status_(req, "200 OK", "text/html; charset=utf-8", html);
  }

  if (path == "list") {
    std::vector<VfsDirEntry> entries;
    if (!vfs_->read_dir("", &entries))
      return send_status_(req, "500 Internal Server Error", "text/plain", "read_dir failed");
    std::string body = "[";
    bool first = true;
    for (const auto &entry : entries) {
      if (is_internal_path_(entry.name) || entry.name == "list")
        continue;
      if (!first)
        body += ',';
      first = false;
      body += "{\"name\":\"" + json_escape(entry.name) + "\",\"size\":" +
              std::to_string(entry.size) + ",\"dir\":" + (entry.is_dir ? "true" : "false") + "}";
    }
    body += ']';
    if (head)
      return send_head_(req, "200 OK", "application/json", body.size());
    return send_status_(req, "200 OK", "application/json", body);
  }

  VfsStat st;
  if (!vfs_->stat(path, &st))
    return send_status_(req, "500 Internal Server Error", "text/plain", "stat failed");
  if (!st.exists)
    return send_status_(req, "404 Not Found", "text/plain", "no such file");
  if (st.is_dir)
    return send_status_(req, "403 Forbidden", "text/plain", "is a directory");

  const std::string etag = this->etag_(st);
  const std::string last_modified = this->http_date_(st.mtime);
  const std::string if_match = this->header_(req, "If-Match");
  const std::string if_unmodified = this->header_(req, "If-Unmodified-Since");
  const std::string if_none_match = this->header_(req, "If-None-Match");
  const std::string if_modified = this->header_(req, "If-Modified-Since");
  const std::string if_range = this->header_(req, "If-Range");
  const std::string range = this->header_(req, "Range");

  time_t timestamp = 0;
  if (!if_match.empty() && !token_list_has_(if_match, etag))
    return send_status_(req, "412 Precondition Failed", "text/plain", "If-Match failed");
  if (parse_http_date_(if_unmodified, &timestamp) && st.mtime > timestamp)
    return send_status_(req, "412 Precondition Failed", "text/plain",
                        "If-Unmodified-Since failed");
  if (if_none_match == "*" || token_list_has_(if_none_match, etag)) {
    httpd_resp_set_hdr(req, "ETag", etag.c_str());
    httpd_resp_set_hdr(req, "Last-Modified", last_modified.c_str());
    return send_empty_(req, "304 Not Modified");
  }
  if (if_none_match.empty() && parse_http_date_(if_modified, &timestamp) && st.mtime <= timestamp) {
    httpd_resp_set_hdr(req, "ETag", etag.c_str());
    httpd_resp_set_hdr(req, "Last-Modified", last_modified.c_str());
    return send_empty_(req, "304 Not Modified");
  }

  size_t start = 0, end = 0;
  int range_status = this->parse_range_(range, st.size, &start, &end);
  if (range_status == 0 && st.size > 0) {
    start = 0;
    end = st.size - 1;
  }
  if (range_status < 0) {
    const std::string content_range = "bytes */" + std::to_string(st.size);
    httpd_resp_set_hdr(req, "Content-Range", content_range.c_str());
    return send_status_(req, "416 Range Not Satisfiable", "text/plain", "bad range");
  }
  if (range_status == 1 && !if_range.empty()) {
    bool range_ok = token_list_has_(if_range, etag);
    if (!range_ok && parse_http_date_(if_range, &timestamp) && st.mtime == timestamp)
      range_ok = true;
    if (!range_ok)
      range_status = 0;
  }

  httpd_resp_set_hdr(req, "ETag", etag.c_str());
  httpd_resp_set_hdr(req, "Last-Modified", last_modified.c_str());
  httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");

  size_t length = range_status == 1 ? end - start + 1 : st.size;
  // HTTPD keeps header pointers until send; a temporary std::string would be
  // destroyed before then and produced a garbled Content-Range on the wire.
  const std::string content_range =
      range_status == 1
          ? "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                std::to_string(st.size)
          : std::string();
  if (range_status == 1)
    httpd_resp_set_hdr(req, "Content-Range", content_range.c_str());
  if (head)
    return send_head_(req, range_status == 1 ? "206 Partial Content" : "200 OK",
                      "application/octet-stream", length);

  httpd_resp_set_status(req, range_status == 1 ? "206 Partial Content" : "200 OK");
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (length == 0)
    return httpd_resp_send(req, nullptr, 0);

#if WEBDAV_FILING_PERF_DEBUG
  const uint32_t get_start = millis();
  uint32_t get_read_ms = 0;
  uint32_t get_send_ms = 0;
#endif
  uint8_t *const buf = allocate_transfer_buffer_(kFileBufferSize);
  if (buf == nullptr)
    return send_status_(req, "500 Internal Server Error", "text/plain", "alloc failed");
  size_t offset = start;
  bool complete = true;
  while (offset <= end) {
    size_t want = std::min(kFileBufferSize, end - offset + 1);
    size_t got = 0;
#if WEBDAV_FILING_PERF_DEBUG
    const uint32_t mark = millis();
#endif
    const bool read_ok = vfs_->read_file(path, offset, buf, want, &got);
#if WEBDAV_FILING_PERF_DEBUG
    get_read_ms += millis() - mark;
    const uint32_t send_mark = millis();
#endif
    if (!read_ok || got == 0) {
      // Do not terminate the chunked response as if it were complete: a SD
      // error or a concurrent delete must be visible to the client as a
      // truncated transfer, not as a valid short file.
      complete = false;
      break;
    }
    esp_err_t err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buf),
                                          static_cast<ssize_t>(got));
    if (err != ESP_OK) {
      free(buf);
      return err;
    }
#if WEBDAV_FILING_PERF_DEBUG
    get_send_ms += millis() - send_mark;
#endif
    offset += got;
  }
#if WEBDAV_FILING_PERF_DEBUG
  ESP_LOGI(TAG, "GET perf: bytes=%u read=%ums send=%ums total=%ums",
           static_cast<unsigned>(length), static_cast<unsigned>(get_read_ms),
           static_cast<unsigned>(get_send_ms), static_cast<unsigned>(millis() - get_start));
#endif
  free(buf);
  if (!complete) {
    ESP_LOGW(TAG, "GET ended early at offset %u of %u; closing connection",
             static_cast<unsigned>(offset), static_cast<unsigned>(length));
    return ESP_FAIL;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t WebDAVFiling::handle_put_(httpd_req_t *req, const std::string &path) {
  if (path.empty() || is_internal_path_(path) || vfs_->is_virtual_collection(path))
    return send_status_(req, "403 Forbidden", "text/plain", "reserved path");

  const std::string expect = this->header_(req, "Expect");
  const std::string if_header = this->header_(req, "If");
  const std::string lock_token_header = this->header_(req, "Lock-Token");
  const std::string if_match = this->header_(req, "If-Match");
  const std::string if_none_match = this->header_(req, "If-None-Match");
  const std::string if_unmodified = this->header_(req, "If-Unmodified-Since");

  if (req->content_len > kMaxFileSize)
    return send_close_(req, "413 Payload Too Large", "text/plain", "too large");

  VfsStat st;
  if (!vfs_->stat(path, &st))
    return send_status_(req, "500 Internal Server Error", "text/plain", "stat failed");
  if (st.exists && st.is_dir)
    return send_status_(req, "409 Conflict", "text/plain", "target is a directory");

  const std::string etag = this->etag_(st);
  if (!if_match.empty()) {
    const bool matches =
        st.exists && (if_match == "*" || token_list_has_(if_match, etag));
    if (!matches)
      return send_dav_error_(req, "412 Precondition Failed", "condition-failed",
                             "If-Match failed");
  }
  if (!if_none_match.empty()) {
    const bool matches =
        st.exists && (if_none_match == "*" || token_list_has_(if_none_match, etag));
    if (matches)
      return send_dav_error_(req, "412 Precondition Failed", "condition-failed",
                             "If-None-Match failed");
  }
  time_t timestamp = 0;
  if (parse_http_date_(if_unmodified, &timestamp) && st.exists && st.mtime > timestamp)
    return send_dav_error_(req, "412 Precondition Failed", "condition-failed",
                           "If-Unmodified-Since failed");
  if (this->path_locked_(path, if_header, lock_token_header))
    return send_dav_error_(req, "423 Locked", "lock-token-submitted", "resource is locked");
  if (!this->handle_expect_(req, expect))
    return ESP_FAIL;

#if WEBDAV_FILING_PERF_DEBUG
  const uint32_t put_start = millis();
  uint32_t put_recv_ms = 0;
  uint32_t put_write_ms = 0;
  uint32_t put_sync_ms = 0;
  uint32_t put_rename_ms = 0;
#endif
  const std::string tmp = this->temp_path_(path);
  if (!this->prepare_temp_(tmp))
    return send_status_(req, "500 Internal Server Error", "text/plain",
                        "staging path is not writable");

#if defined(ESP_PLATFORM) && defined(USE_PSRAM) && !defined(WEBDAV_FILING_HOST_TEST)
  // On PSRAM builds such as the P4, writing ext2 stops the HTTPD task from
  // draining TCP. Run the writes on a dedicated task and keep receiving into a
  // PSRAM ring so the two costs overlap. One upload owns the deferred ext2
  // state at a time.
  if (req->content_len != 0) {
    std::lock_guard<std::mutex> upload_guard(s_async_upload_mutex);
    AsyncUploadJob job;
    job.vfs = vfs_;
    job.temp = tmp;
    job.target = path;
    job.content_len = static_cast<size_t>(req->content_len);
    job.capacity = kUploadRingSize;
    job.slot_count = job.capacity / kPutBufferSize;
    job.ring = static_cast<uint8_t *>(heap_caps_malloc(
        job.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED));
    job.slot_len = static_cast<size_t *>(
        heap_caps_malloc(job.slot_count * sizeof(size_t), MALLOC_CAP_8BIT));
    const auto slots = static_cast<UBaseType_t>(job.slot_count);
    job.space = xSemaphoreCreateCounting(slots, slots);
    job.data = xSemaphoreCreateCounting(slots, 0);
    job.mutex = xSemaphoreCreateMutex();
    job.done = xSemaphoreCreateBinary();
    if (job.ring == nullptr || job.slot_len == nullptr || job.space == nullptr ||
        job.data == nullptr || job.mutex == nullptr || job.done == nullptr) {
      ESP_LOGE(TAG, "unable to allocate async upload buffers");
      async_upload_free(&job);
      return send_status_(req, "507 Insufficient Storage", "text/plain", "out of memory");
    }
    if (xTaskCreate(&async_upload_writer, "dav-upload", 16 * 1024, &job, kWebDavTaskPriority,
                    nullptr) != pdPASS) {
      async_upload_free(&job);
      return send_status_(req, "500 Internal Server Error", "text/plain", "writer task failed");
    }

    // Receive straight into a reserved ring slot. The writer owns that slot
    // until it has written it to ext2, so no intermediate copy is needed.
    bool slot_reserved = false;
    size_t slot_filled = 0;
    size_t remaining = job.content_len;
    while (remaining > 0) {
      if (!slot_reserved) {
        if (!async_upload_wait(job.space, &job)) break;
        slot_reserved = true;
      }
      const size_t want = std::min(kPutBufferSize - slot_filled, remaining);
      uint8_t *const dst = job.ring + job.tail * kPutBufferSize + slot_filled;
#if WEBDAV_FILING_PERF_DEBUG
      const uint32_t recv_mark = millis();
#endif
      const int got = httpd_req_recv(req, reinterpret_cast<char *>(dst), want);
#if WEBDAV_FILING_PERF_DEBUG
      put_recv_ms += millis() - recv_mark;
#endif
      if (got <= 0) {
        xSemaphoreGive(job.space);
        xSemaphoreTake(job.mutex, portMAX_DELAY);
        job.producer_failed = true;
        xSemaphoreGive(job.mutex);
        break;
      }
      slot_filled += static_cast<size_t>(got);
      remaining -= static_cast<size_t>(got);
      if (slot_filled != kPutBufferSize && remaining != 0) continue;
      xSemaphoreTake(job.mutex, portMAX_DELAY);
      job.slot_len[job.tail] = slot_filled;
      job.tail = (job.tail + 1) % job.slot_count;
      ++job.used;
      xSemaphoreGive(job.mutex);
      xSemaphoreGive(job.data);
      slot_filled = 0;
      slot_reserved = false;
    }
    xSemaphoreTake(job.mutex, portMAX_DELAY);
    job.producer_done = true;
    xSemaphoreGive(job.mutex);
    xSemaphoreGive(job.data);
#if WEBDAV_FILING_PERF_DEBUG
    const uint32_t writer_mark = millis();
#endif
    xSemaphoreTake(job.done, portMAX_DELAY);
#if WEBDAV_FILING_PERF_DEBUG
    put_write_ms += millis() - writer_mark;
#endif
    const bool recv_failed = job.producer_failed;
    const bool write_failed = job.failed;
    async_upload_free(&job);
#if WEBDAV_FILING_PERF_DEBUG
    ESP_LOGI(TAG, "PUT perf: bytes=%u recv=%ums writer=%ums total=%ums",
             static_cast<unsigned>(req->content_len), static_cast<unsigned>(put_recv_ms),
             static_cast<unsigned>(put_write_ms), static_cast<unsigned>(millis() - put_start));
#endif
    if (recv_failed)
      return send_status_(req, "400 Bad Request", "text/plain", "body read failed");
    if (write_failed)
      return send_status_(req, "500 Internal Server Error", "text/plain", "write failed");
    return st.exists ? send_empty_(req, "204 No Content")
                     : send_status_(req, "201 Created", "text/plain", "ok");
  }
#endif

  if (req->content_len == 0) {
    static const uint8_t kEmpty = 0;
    if (!vfs_->write_file(tmp, &kEmpty, 0)) {
      vfs_->remove(tmp);
      return send_status_(req, "500 Internal Server Error", "text/plain", "write failed");
    }
  } else {
    // Host tests and non-ESP builds use the simple path: accumulate short TCP
    // reads into whole ext2 blocks before calling write_file().
    std::vector<uint8_t> buf(kPutBufferSize);
    size_t filled = 0;
    size_t remaining = req->content_len;
    while (remaining > 0) {
      size_t want = std::min(buf.size() - filled, remaining);
#if WEBDAV_FILING_PERF_DEBUG
      const uint32_t recv_mark = millis();
#endif
      int got = httpd_req_recv(req, reinterpret_cast<char *>(buf.data() + filled), want);
#if WEBDAV_FILING_PERF_DEBUG
      put_recv_ms += millis() - recv_mark;
#endif
      if (got <= 0) {
        vfs_->remove(tmp);
        return send_status_(req, "400 Bad Request", "text/plain", "body read failed");
      }
      filled += static_cast<size_t>(got);
      remaining -= static_cast<size_t>(got);
      if (filled == buf.size()) {
#if WEBDAV_FILING_PERF_DEBUG
        const uint32_t write_mark = millis();
#endif
        const bool written = vfs_->write_file(tmp, buf.data(), filled);
#if WEBDAV_FILING_PERF_DEBUG
        put_write_ms += millis() - write_mark;
#endif
        if (!written) {
          vfs_->remove(tmp);
          return send_status_(req, "500 Internal Server Error", "text/plain", "write failed");
        }
        filled = 0;
      }
    }
    if (filled != 0) {
#if WEBDAV_FILING_PERF_DEBUG
      const uint32_t write_mark = millis();
#endif
      const bool written = vfs_->write_file(tmp, buf.data(), filled);
#if WEBDAV_FILING_PERF_DEBUG
      put_write_ms += millis() - write_mark;
#endif
      if (!written) {
        vfs_->remove(tmp);
        return send_status_(req, "500 Internal Server Error", "text/plain", "write failed");
      }
    }
  }

#if WEBDAV_FILING_PERF_DEBUG
  const uint32_t sync_mark = millis();
#endif
  const bool synced = vfs_->sync();
#if WEBDAV_FILING_PERF_DEBUG
  put_sync_ms += millis() - sync_mark;
#endif
  if (!synced) {
    vfs_->remove(tmp);
    return send_status_(req, "500 Internal Server Error", "text/plain", "sync failed");
  }
  // rename() replaces an existing file on every backend. Removing first would
  // cost an extra unlink + commit and briefly leave the destination missing.
#if WEBDAV_FILING_PERF_DEBUG
  const uint32_t rename_mark = millis();
#endif
  const bool renamed = vfs_->rename(tmp, path);
#if WEBDAV_FILING_PERF_DEBUG
  put_rename_ms += millis() - rename_mark;
#endif
  if (!renamed) {
    vfs_->remove(tmp);
    return send_status_(req, "500 Internal Server Error", "text/plain", "rename failed");
  }
#if WEBDAV_FILING_PERF_DEBUG
  ESP_LOGI(TAG, "PUT perf: bytes=%u recv=%ums write=%ums sync=%ums rename=%ums total=%ums",
           static_cast<unsigned>(req->content_len), static_cast<unsigned>(put_recv_ms),
           static_cast<unsigned>(put_write_ms), static_cast<unsigned>(put_sync_ms),
           static_cast<unsigned>(put_rename_ms), static_cast<unsigned>(millis() - put_start));
#endif
  return st.exists ? send_empty_(req, "204 No Content")
                   : send_status_(req, "201 Created", "text/plain", "ok");
}

esp_err_t WebDAVFiling::handle_delete_(httpd_req_t *req, const std::string &path) {
  if (path.empty() || is_internal_path_(path) || vfs_->is_virtual_collection(path))
    return send_status_(req, "403 Forbidden", "text/plain", "cannot delete root");
  const std::string if_header = this->header_(req, "If");
  const std::string lock_token_header = this->header_(req, "Lock-Token");
  const std::string if_match = this->header_(req, "If-Match");
  const std::string if_unmodified = this->header_(req, "If-Unmodified-Since");
  if (this->subtree_locked_(path, if_header, lock_token_header))
    return send_dav_error_(req, "423 Locked", "lock-token-submitted", "resource is locked");

  VfsStat st;
  if (!vfs_->stat(path, &st))
    return send_status_(req, "500 Internal Server Error", "text/plain", "stat failed");
  if (!st.exists)
    return send_status_(req, "404 Not Found", "text/plain", "no such file");
  if (!if_match.empty() && if_match != "*" &&
      !token_list_has_(if_match, this->etag_(st)))
    return send_dav_error_(req, "412 Precondition Failed", "condition-failed", "If-Match failed");
  time_t timestamp = 0;
  if (parse_http_date_(if_unmodified, &timestamp) && st.mtime > timestamp)
    return send_dav_error_(req, "412 Precondition Failed", "condition-failed",
                           "If-Unmodified-Since failed");
  if (!this->remove_recursive_(path))
    return send_status_(req, "500 Internal Server Error", "text/plain", "remove failed");
  if (this->props_.remove_resource(path) != 0 && !this->props_.save(vfs_))
    ESP_LOGW(TAG, "could not persist dead-property removal for %s", path.c_str());
  return send_empty_(req, "204 No Content");
}

esp_err_t WebDAVFiling::handle_mkcol_(httpd_req_t *req, const std::string &path) {
  if (path.empty() || is_internal_path_(path) || vfs_->is_virtual_collection(path))
    return send_status_(req, "405 Method Not Allowed", "text/plain", "cannot create root");
  if (req->content_len != 0)
    return send_close_(req, "415 Unsupported Media Type", "text/plain",
                       "MKCOL body not supported");
  const std::string if_header = this->header_(req, "If");
  const std::string lock_token_header = this->header_(req, "Lock-Token");
  if (this->path_locked_(path, if_header, lock_token_header))
    return send_dav_error_(req, "423 Locked", "lock-token-submitted", "resource is locked");

  size_t slash = path.find_last_of('/');
  std::string parent = slash == std::string::npos ? "" : path.substr(0, slash);
  VfsStat parent_st;
  if (!vfs_->stat(parent, &parent_st))
    return send_status_(req, "500 Internal Server Error", "text/plain", "stat failed");
  if (!parent_st.exists || !parent_st.is_dir)
    return send_status_(req, "409 Conflict", "text/plain", "parent is not a collection");

  VfsStat st;
  if (!vfs_->stat(path, &st))
    return send_status_(req, "500 Internal Server Error", "text/plain", "stat failed");
  if (st.exists)
    return send_status_(req, "405 Method Not Allowed", "text/plain", "already exists");
  if (!vfs_->mkdir(path))
    return send_status_(req, "500 Internal Server Error", "text/plain", "mkdir failed");
  return send_status_(req, "201 Created", "text/plain", "ok");
}

esp_err_t WebDAVFiling::handle_propfind_(httpd_req_t *req, const std::string &path) {
  if (is_internal_path_(path))
    return send_status_(req, "404 Not Found", "text/plain", "no such file");

  WebDAVDepth depth;
  std::string depth_header = this->header_(req, "Depth");
  if (!this->parse_depth_(depth_header, &depth))
    return send_status_(req, "400 Bad Request", "text/plain", "invalid Depth");
  const std::string expect = this->header_(req, "Expect");
  if (!this->handle_expect_(req, expect))
    return ESP_FAIL;

  std::string body;
  if (!this->read_body_(req, &body, kMaxXmlBody))
    return ESP_FAIL;
  PropfindRequest request;
  if (!this->parse_propfind_body_(body, &request))
    return send_status_(req, "400 Bad Request", "text/plain", "invalid PROPFIND body");

  VfsStat st;
  if (!vfs_->stat(path, &st))
    return send_status_(req, "500 Internal Server Error", "text/plain", "stat failed");
  if (!st.exists)
    return send_status_(req, "404 Not Found", "text/plain", "no such file");

  httpd_resp_set_status(req, "207 Multi-Status");
  httpd_resp_set_type(req, "application/xml; charset=utf-8");
  httpd_resp_set_hdr(req, "DAV", "1,2");

  std::string buffer = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                       "<D:multistatus xmlns:D=\"DAV:\">";
  esp_err_t err = this->send_propfind_resource_(req, &buffer, path, st, request,
                                                static_cast<int>(depth), 0);
  if (err != ESP_OK)
    return err;
  buffer += "</D:multistatus>\n";
  err = flush_chunk_(req, &buffer, true);
  if (err != ESP_OK)
    return err;
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t WebDAVFiling::handle_proppatch_(httpd_req_t *req, const std::string &path) {
  if (path.empty() || is_internal_path_(path))
    return send_status_(req, "403 Forbidden", "text/plain", "reserved path");
  const std::string expect = this->header_(req, "Expect");
  const std::string if_header = this->header_(req, "If");
  const std::string lock_token_header = this->header_(req, "Lock-Token");
  if (this->path_locked_(path, if_header, lock_token_header))
    return send_dav_error_(req, "423 Locked", "lock-token-submitted", "resource is locked");
  if (!this->handle_expect_(req, expect))
    return ESP_FAIL;

  std::string body;
  if (!this->read_body_(req, &body, kMaxXmlBody))
    return ESP_FAIL;
  std::vector<PropChange> changes;
  if (!this->parse_proppatch_body_(body, &changes))
    return send_status_(req, "400 Bad Request", "text/plain", "invalid PROPPATCH body");

  VfsStat st;
  if (!vfs_->stat(path, &st))
    return send_status_(req, "500 Internal Server Error", "text/plain", "stat failed");
  if (!st.exists)
    return send_status_(req, "404 Not Found", "text/plain", "no such file");

  std::string ok_props, missing_props, forbidden_props;
  for (const auto &change : changes) {
    const bool protected_prop = this->protected_prop_(change.spec);
    bool ok = !protected_prop;
    if (protected_prop) {
      // Live DAV properties are computed by the server; they cannot be patched.
    } else if (change.set) {
      this->props_.set(path, change.spec.key, change.value);
    } else {
      ok = this->props_.remove(path, change.spec.key);
    }
    std::string element;
    if (!this->live_prop_element_(change.spec, path, st, &element)) {
      for (const auto &dead : this->props_.list(path)) {
        if (dead.key == change.spec.key) {
          element = dead_prop_element_(dead);
          break;
        }
      }
    }
    if (element.empty())
      element = "<D:" + change.spec.local + "/>";
    if (protected_prop)
      forbidden_props += element;
    else if (ok)
      ok_props += element;
    else
      missing_props += element;
  }

  if (!this->props_.save(vfs_)) {
    this->props_.load(vfs_);
    return send_status_(req, "500 Internal Server Error", "text/plain", "property save failed");
  }

  std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>" +
                    url_encode_path_("/" + path) + "</D:href>";
  if (!ok_props.empty())
    out += "<D:propstat><D:prop>" + ok_props +
           "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>";
  if (!missing_props.empty())
    out += "<D:propstat><D:prop>" + missing_props +
           "</D:prop><D:status>HTTP/1.1 404 Not Found</D:status></D:propstat>";
  if (!forbidden_props.empty())
    out += "<D:propstat><D:prop>" + forbidden_props +
           "</D:prop><D:status>HTTP/1.1 403 Forbidden</D:status></D:propstat>";
  out += "</D:response></D:multistatus>\n";
  return send_status_(req, "207 Multi-Status", "application/xml; charset=utf-8", out);
}

esp_err_t WebDAVFiling::handle_copy_move_(httpd_req_t *req, const std::string &from, bool move) {
  if (from.empty() || is_internal_path_(from))
    return send_dav_error_(req, "403 Forbidden", "property-restriction", "reserved path");
  const std::string destination = this->header_(req, "Destination");
  const std::string overwrite = this->header_(req, "Overwrite");
  const std::string depth_header = this->header_(req, "Depth");
  const std::string if_header = this->header_(req, "If");
  const std::string lock_token_header = this->header_(req, "Lock-Token");
  const std::string if_match = this->header_(req, "If-Match");
  const std::string if_unmodified = this->header_(req, "If-Unmodified-Since");
  const std::string host = this->header_(req, "Host");

  std::string to = this->destination_path_(destination);
  if (to.empty() || is_internal_path_(to) || vfs_->is_virtual_collection(to))
    return send_dav_error_(req, "400 Bad Request", "destination-required", "bad destination");
  if (to == from || (to.size() > from.size() && to.compare(0, from.size(), from) == 0 &&
                     to[from.size()] == '/'))
    return send_dav_error_(req, "403 Forbidden", "cannot-modify-protected-property",
                           "destination is the source or inside it");
  if (vfs_->is_hidden(to))
    return send_status_(req, "404 Not Found", "text/plain", "no such file");
  if (!this->destination_matches_request_(destination, host))
    return send_dav_error_(req, "400 Bad Request", "destination-required",
                           "destination is on another host");

  WebDAVDepth depth = WebDAVDepth::INFINITE;
  if (!this->parse_depth_(depth_header, &depth))
    return send_status_(req, "400 Bad Request", "text/plain", "invalid Depth");
  if (move && depth != WebDAVDepth::INFINITE)
    return send_status_(req, "400 Bad Request", "text/plain", "MOVE requires Depth: infinity");

  if (this->subtree_locked_(from, if_header, lock_token_header))
    return send_dav_error_(req, "423 Locked", "lock-token-submitted", "source is locked");

  VfsStat src;
  if (!vfs_->stat(from, &src))
    return send_status_(req, "500 Internal Server Error", "text/plain", "stat failed");
  if (!src.exists)
    return send_status_(req, "404 Not Found", "text/plain", "no such file");
  if (!if_match.empty() && if_match != "*" &&
      !token_list_has_(if_match, this->etag_(src)))
    return send_dav_error_(req, "412 Precondition Failed", "condition-failed", "If-Match failed");
  time_t timestamp = 0;
  if (parse_http_date_(if_unmodified, &timestamp) && src.mtime > timestamp)
    return send_dav_error_(req, "412 Precondition Failed", "condition-failed",
                           "If-Unmodified-Since failed");

  size_t slash = to.find_last_of('/');
  const std::string parent = slash == std::string::npos ? "" : to.substr(0, slash);
  VfsStat parent_st;
  if (!vfs_->stat(parent, &parent_st))
    return send_status_(req, "500 Internal Server Error", "text/plain", "stat failed");
  if (!parent_st.exists || !parent_st.is_dir)
    return send_dav_error_(req, "409 Conflict", "resource-parent-exists",
                           "destination parent is not a collection");

  VfsStat dst;
  if (!vfs_->stat(to, &dst))
    return send_status_(req, "500 Internal Server Error", "text/plain", "stat failed");
  if (dst.exists && dst.is_dir != src.is_dir)
    return send_dav_error_(req, "409 Conflict", "cannot-complete-on-destination",
                           "destination has the wrong resource type");
  if (dst.exists) {
    if (this->subtree_locked_(to, if_header, lock_token_header))
      return send_dav_error_(req, "423 Locked", "lock-token-submitted", "destination is locked");
    if (iequals_(overwrite, "F"))
      return send_dav_error_(req, "412 Precondition Failed", "overwrite-forbidden",
                             "destination exists");
  }

  bool ok = false;
  bool partial = false;
  if (move) {
    ok = vfs_->rename(from, to);
  } else {
    bool created = false;
    CopyStatus status = this->copy_recursive_(from, to, depth, &created);
    ok = status != CopyStatus::FAIL;
    partial = status == CopyStatus::PARTIAL;
  }
  if (!ok)
    return send_status_(req, "500 Internal Server Error", "text/plain",
                        move ? "rename failed" : "copy failed");
  if (move && this->props_.rename_resource(from, to) != 0 && !this->props_.save(vfs_))
    ESP_LOGW(TAG, "could not persist dead-property move from %s to %s", from.c_str(),
             to.c_str());
  if (partial)
    return send_dav_error_(req, "424 Failed Dependency", "cannot-complete-on-destination",
                           "some children could not be copied");
  return dst.exists ? send_empty_(req, "204 No Content")
                    : send_status_(req, "201 Created", "text/plain", "ok");
}

esp_err_t WebDAVFiling::handle_lock_(httpd_req_t *req, const std::string &path) {
  const std::string expect = this->header_(req, "Expect");
  const std::string if_header = this->header_(req, "If");
  const std::string lock_token_header = this->header_(req, "Lock-Token");
  const std::string timeout = this->header_(req, "Timeout");

  VfsStat st;
  if (!vfs_->stat(path, &st))
    return send_status_(req, "500 Internal Server Error", "text/plain", "stat failed");
  if (!st.exists)
    return send_status_(req, "404 Not Found", "text/plain", "no such file");
  if (!this->handle_expect_(req, expect))
    return ESP_FAIL;

  std::string body;
  if (!this->read_body_(req, &body, kMaxXmlBody))
    return ESP_FAIL;

  bool exclusive = true;
  std::string owner;
  if (!body.empty()) {
    XmlNode root;
    if (parse_xml(body, &root)) {
      const XmlNode *scope = first_child(root, "DAV:", "lockscope");
      if (scope != nullptr && first_child(*scope, "DAV:", "shared") != nullptr)
        exclusive = false;
      const XmlNode *owner_node = first_child(root, "DAV:", "owner");
      if (owner_node != nullptr)
        owner = owner_node->text.empty() ? owner_node->raw : owner_node->text;
    }
  }

  const uint32_t seconds = this->parse_timeout_(timeout);
  const uint32_t expires_at = esphome::millis() + seconds * 1000;

  this->prune_locks_();
  auto existing = this->locks_for_(path);
  std::vector<WebDAVLock *> owned;
  bool unowned_exclusive = false;
  for (WebDAVLock *lock : existing) {
    if (this->request_has_lock_(if_header, lock_token_header, *lock))
      owned.push_back(lock);
    else if (lock->exclusive)
      unowned_exclusive = true;
  }

  std::string response_token;
  if (!owned.empty()) {
    // A LOCK carrying a matching token refreshes the locks it owns.
    for (WebDAVLock *lock : owned)
      lock->expires_at = expires_at;
    response_token = owned.front()->token;
  } else {
    if (unowned_exclusive || (exclusive && !existing.empty()))
      return send_dav_error_(req, "423 Locked", "lock-token-submitted", "resource is locked");
    if (this->locks_.size() >= kMaxLocks)
      return send_status_(req, "507 Insufficient Storage", "text/plain", "too many locks");
    this->locks_.push_back(WebDAVLock{path, this->next_lock_token_(), owner, exclusive, expires_at});
    response_token = this->locks_.back().token;
  }

  const std::string lock_header = "<" + response_token + ">";
  httpd_resp_set_hdr(req, "Lock-Token", lock_header.c_str());
  std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<D:prop xmlns:D=\"DAV:\">";
  this->append_lock_properties_(&out, path);
  out += "</D:prop>\n";
  return send_status_(req, "200 OK", "application/xml; charset=utf-8", out);
}

esp_err_t WebDAVFiling::handle_unlock_(httpd_req_t *req, const std::string &path) {
  const std::string token = this->strip_angles_(this->header_(req, "Lock-Token"));
  if (token.empty())
    return send_status_(req, "400 Bad Request", "text/plain", "missing Lock-Token header");

  this->prune_locks_();
  auto it = std::find_if(this->locks_.begin(), this->locks_.end(),
                         [&path, &token](const WebDAVLock &lock) {
                           return lock.path == path && lock.token == token;
                         });
  if (it == this->locks_.end())
    return send_dav_error_(req, "409 Conflict", "lock-token-matches-request-uri",
                           "unknown lock token");
  this->locks_.erase(it);
  return send_empty_(req, "204 No Content");
}

bool WebDAVFiling::remove_recursive_(const std::string &path) {
  // Iterative post-order deletion: path depth is client-controlled, so a
  // recursive walk can overflow the HTTPD task's small stack.
  std::vector<std::string> pending{path};
  std::vector<std::string> directories;
  while (!pending.empty()) {
    std::string current = std::move(pending.back());
    pending.pop_back();

    VfsStat st;
    if (!vfs_->stat(current, &st))
      return false;
    if (!st.exists)
      continue;
    if (!st.is_dir) {
      if (!vfs_->remove(current))
        return false;
      continue;
    }

    std::vector<VfsDirEntry> entries;
    if (!vfs_->read_dir(current, &entries))
      return false;
    directories.push_back(std::move(current));
    for (const auto &entry : entries) {
      if (is_internal_path_(entry.name))
        continue;
      pending.push_back(directories.back() + "/" + entry.name);
    }
  }
  for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
    if (!vfs_->remove(*it))
      return false;
  }
  return true;
}

CopyStatus WebDAVFiling::copy_recursive_(const std::string &from, const std::string &to,
                                     WebDAVDepth depth, bool *created, int level) {
  VfsStat st;
  if (!vfs_->stat(from, &st) || !st.exists)
    return CopyStatus::FAIL;
  if (level >= kMaxRecursion)
    return CopyStatus::FAIL;

  if (st.is_dir) {
    VfsStat dst;
    if (!vfs_->stat(to, &dst))
      return CopyStatus::FAIL;
    if (dst.exists) {
      if (!dst.is_dir)
        return CopyStatus::FAIL;
    } else if (!vfs_->mkdir(to)) {
      return CopyStatus::FAIL;
    } else {
      *created = true;
    }
    if (depth == WebDAVDepth::ZERO)
      return CopyStatus::OK;

    std::vector<VfsDirEntry> entries;
    if (!vfs_->read_dir(from, &entries))
      return CopyStatus::PARTIAL;
    CopyStatus worst = CopyStatus::OK;
    for (const auto &entry : entries) {
      if (is_internal_path_(entry.name))
        continue;
      std::string child_from = from.empty() ? entry.name : from + "/" + entry.name;
      std::string child_to = to.empty() ? entry.name : to + "/" + entry.name;
      CopyStatus status = this->copy_recursive_(child_from, child_to, depth, created, level + 1);
      if (status == CopyStatus::FAIL)
        worst = CopyStatus::PARTIAL;
    }
    return worst;
  }

  vfs_->remove(to);
  uint8_t *const buf = allocate_transfer_buffer_(kFileBufferSize);
  if (buf == nullptr)
    return CopyStatus::FAIL;
  size_t offset = 0;
  while (offset < st.size) {
    size_t want = std::min(kFileBufferSize, st.size - offset);
    size_t got = 0;
    if (!vfs_->read_file(from, offset, buf, want, &got) || got == 0) {
      free(buf);
      return CopyStatus::FAIL;
    }
    if (!vfs_->write_file(to, buf, got)) {
      free(buf);
      return CopyStatus::FAIL;
    }
    offset += got;
  }
  free(buf);
  if (st.size == 0 && !vfs_->write_file(to, reinterpret_cast<const uint8_t *>(""), 0))
    return CopyStatus::FAIL;
  return vfs_->sync() ? CopyStatus::OK : CopyStatus::FAIL;
}

void WebDAVFiling::prune_locks_() {
  uint32_t now = esphome::millis();
  auto expired = [now](const WebDAVLock &lock) {
    return static_cast<int32_t>(lock.expires_at - now) <= 0;
  };
  this->locks_.erase(std::remove_if(this->locks_.begin(), this->locks_.end(), expired),
                     this->locks_.end());
}

WebDAVFiling::WebDAVLock *WebDAVFiling::find_lock_(const std::string &path, const std::string &token) {
  for (auto &lock : this->locks_) {
    if (lock.path == path && lock.token == token)
      return &lock;
  }
  return nullptr;
}

const WebDAVFiling::WebDAVLock *WebDAVFiling::find_lock_(const std::string &path,
                                                 const std::string &token) const {
  for (const auto &lock : this->locks_) {
    if (lock.path == path && lock.token == token)
      return &lock;
  }
  return nullptr;
}

std::vector<WebDAVFiling::WebDAVLock *> WebDAVFiling::locks_for_(const std::string &path) {
  this->prune_locks_();
  std::vector<WebDAVLock *> out;
  for (auto &lock : this->locks_) {
    if (lock.path == path)
      out.push_back(&lock);
  }
  return out;
}

bool WebDAVFiling::request_has_lock_(const std::string &if_header,
                                 const std::string &lock_token_header,
                                 const WebDAVLock &lock) const {
  return if_header_allows_(if_header, lock.token) ||
         header_has_lock_token_(lock_token_header, lock.token);
}

bool WebDAVFiling::path_locked_(const std::string &path, const std::string &if_header,
                            const std::string &lock_token_header) {
  if (this->locks_.empty())
    return false;
  bool has_shared = false;
  bool authorized_shared = false;
  for (const WebDAVLock *lock : this->locks_for_(path)) {
    const bool authorized = this->request_has_lock_(if_header, lock_token_header, *lock);
    if (lock->exclusive)
      return !authorized;
    has_shared = true;
    authorized_shared = authorized_shared || authorized;
  }
  // Any holder of a shared lock may write; other clients are blocked.
  return has_shared && !authorized_shared;
}

bool WebDAVFiling::subtree_locked_(const std::string &path, const std::string &if_header,
                               const std::string &lock_token_header, int depth) {
  if (this->locks_.empty())
    return false;
  if (this->path_locked_(path, if_header, lock_token_header))
    return true;
  if (depth >= kMaxRecursion)
    return false;
  VfsStat st;
  if (!vfs_->stat(path, &st) || !st.is_dir)
    return false;
  std::vector<VfsDirEntry> entries;
  if (!vfs_->read_dir(path, &entries))
    return false;
  for (const auto &entry : entries) {
    if (is_internal_path_(entry.name))
      continue;
    std::string child = path.empty() ? entry.name : path + "/" + entry.name;
    if (this->subtree_locked_(child, if_header, lock_token_header, depth + 1))
      return true;
  }
  return false;
}

uint32_t WebDAVFiling::parse_timeout_(const std::string &value) const {
  if (iequals_(value, "Infinite"))
    return kMaxLockSeconds;
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower.rfind("second-", 0) == 0) {
    unsigned long seconds = std::strtoul(value.c_str() + 7, nullptr, 10);
    if (seconds == 0)
      return kDefaultLockSeconds;
    return static_cast<uint32_t>(std::min<unsigned long>(seconds, kMaxLockSeconds));
  }
  return kDefaultLockSeconds;
}

std::string WebDAVFiling::next_lock_token_() {
  char buf[80];
  std::snprintf(buf, sizeof(buf), "opaquelocktoken:davbox-%08lx-%lu",
                static_cast<unsigned long>(esphome::millis()),
                static_cast<unsigned long>(++this->lock_counter_));
  return buf;
}

std::string WebDAVFiling::temp_path_(const std::string &target) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), ".davtmp-%08lx-%lu",
                static_cast<unsigned long>(esphome::millis()),
                static_cast<unsigned long>(++this->temp_counter_));
  // Keep the staging file beside its target: the final rename is atomic on a
  // single filesystem, and a virtual-root router only allows writes inside a
  // named volume.
  const size_t slash = target.rfind('/');
  if (slash == std::string::npos)
    return buf;
  return target.substr(0, slash + 1) + buf;
}

bool WebDAVFiling::prepare_temp_(const std::string &tmp) {
  // write_file() is append-only. A stale temp path must be gone before the
  // first chunk, or a failed remove would publish old+new concatenated bytes.
  vfs_->remove(tmp);
  VfsStat st;
  return vfs_->stat(tmp, &st) && !st.exists;
}

#if defined(ESP_PLATFORM) && !defined(WEBDAV_FILING_HOST_TEST)
// Status, headers, and body are separate sends. Nagle can hold the last
// segment until a delayed ACK, which adds tens of milliseconds to small
// keep-alive requests.
static esp_err_t webdav_open_(httpd_handle_t server, int sockfd) {
  (void) server;
  const int nodelay = 1;
  if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0)
    ESP_LOGW(TAG, "setsockopt(TCP_NODELAY) failed on fd %d", sockfd);
  return ESP_OK;
}
#endif

esp_err_t WebDAVFiling::handle_(httpd_req_t *req) {
  if (!this->authorize_(req))
    return ESP_OK;

  const std::string transfer_encoding = ascii_lower(this->header_(req, "Transfer-Encoding"));
  if (transfer_encoding.find("chunked") != std::string::npos)
    return send_close_(req, "501 Not Implemented", "text/plain",
                       "chunked request bodies unsupported");

  if (req->method == HTTP_OPTIONS && std::strcmp(req->uri, "*") == 0) {
    httpd_resp_set_hdr(req, "DAV", "1,2");
    httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
    httpd_resp_set_hdr(req, "Allow",
                       "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, PROPPATCH, "
                       "COPY, MOVE, LOCK, UNLOCK");
    return send_empty_(req, "204 No Content");
  }

  std::string path;
  if (!this->file_path_(req->uri, &path))
    return send_status_(req, "400 Bad Request", "text/plain", "bad path");
  // A Music Assistant sync issues thousands of these. Keep them at VERBOSE so
  // the ESPHome logger does not stall the HTTPD task during a media scan.
  ESP_LOGV(TAG, "method=%d uri=%s path=%s", req->method, req->uri, path.c_str());
  if (vfs_->is_hidden(path))
    return send_status_(req, "404 Not Found", "text/plain", "no such file");

  switch (req->method) {
    case HTTP_GET:
#if defined(ESP_PLATFORM) && defined(USE_PSRAM) && !defined(WEBDAV_FILING_HOST_TEST)
      if (WebDAVFiling::queue_get_request_(this, req, path))
        return ESP_OK;
#endif
      return this->handle_get_(req, path, false);
    case HTTP_HEAD:
      return this->handle_get_(req, path, true);
    case HTTP_PUT:
      return this->handle_put_(req, path);
    case HTTP_DELETE:
      return this->handle_delete_(req, path);
    case HTTP_MKCOL:
      return this->handle_mkcol_(req, path);
    case HTTP_PROPFIND:
      return this->handle_propfind_(req, path);
    case HTTP_PROPPATCH:
      return this->handle_proppatch_(req, path);
    case HTTP_COPY:
      return this->handle_copy_move_(req, path, false);
    case HTTP_MOVE:
      return this->handle_copy_move_(req, path, true);
    case HTTP_LOCK:
      return this->handle_lock_(req, path);
    case HTTP_UNLOCK:
      return this->handle_unlock_(req, path);
    case HTTP_OPTIONS:
      httpd_resp_set_hdr(req, "DAV", "1,2");
      httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
      httpd_resp_set_hdr(req, "Allow",
                         "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, PROPPATCH, "
                         "COPY, MOVE, LOCK, UNLOCK");
      return send_empty_(req, "204 No Content");
    default:
      return send_status_(req, "405 Method Not Allowed", "text/plain", "method not allowed");
  }
}

esp_err_t WebDAVFiling::dispatch_(httpd_req_t *req) {
  auto *self = static_cast<WebDAVFiling *>(req->user_ctx);
  if (self == nullptr || self->vfs_ == nullptr)
    return ESP_FAIL;
  return self->handle_(req);
}

void WebDAVFiling::setup() {
#ifndef WEBDAV_FILING_HOST_TEST
  if (vfs_ == nullptr) {
    Vfs *created = new VfsAuto(this->sd_);
    if (!created->init()) {
      ESP_LOGE(TAG, "auto volume discovery failed");
      delete created;
      this->mark_failed();
      return;
    }
    vfs_owned_ = created;
    vfs_ = created;
  } else if (!vfs_->init()) {
    ESP_LOGE(TAG, "injected VFS init failed");
    this->mark_failed();
    return;
  }
#endif
  if (vfs_ != nullptr && !this->props_.load(vfs_))
    ESP_LOGW(TAG, "could not load dead properties");

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = this->port_;
  config.max_uri_handlers = 2;
  // Long-path ext2 resolution and nested PROPFIND/COPY need more headroom than
  // IDF's 4 KiB default; 16 KiB still leaves the P4 heap comfortable.
  config.stack_size = 16 * 1024;
  config.task_priority = kWebDavTaskPriority;
  config.max_open_sockets = WEBDAV_FILING_HTTPD_MAX_OPEN_SOCKETS;
  config.backlog_conn = WEBDAV_FILING_HTTPD_BACKLOG_CONN;
  config.max_req_hdr_len = WEBDAV_FILING_HTTPD_MAX_REQ_HDR_LEN;
  config.max_uri_len = WEBDAV_FILING_HTTPD_MAX_URI_LEN;
  config.max_resp_headers = 12;
  // Media players can briefly stop draining a FLAC stream while buffering or
  // seeking. The IDF default of 5 s is short enough to kill a healthy stream.
  config.send_wait_timeout = 15;
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;
#if defined(ESP_PLATFORM) && !defined(WEBDAV_FILING_HOST_TEST)
  config.open_fn = &webdav_open_;
#endif

  esp_err_t err = httpd_start(&this->server_, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

#if defined(ESP_PLATFORM) && defined(USE_PSRAM) && !defined(WEBDAV_FILING_HOST_TEST)
  if (!WebDAVFiling::start_async_get_workers_())
    ESP_LOGW(TAG, "async GET workers unavailable; GET falls back to the HTTPD task");
#endif

  // IDF's wildcard matcher needs a trailing '*'; "/" would only match the root.
  this->uri_.uri = "/*";
  this->uri_.method = static_cast<httpd_method_t>(HTTP_ANY);
  this->uri_.handler = &WebDAVFiling::dispatch_;
  this->uri_.user_ctx = this;
  err = httpd_register_uri_handler(this->server_, &this->uri_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_register_uri_handler failed: %s", esp_err_to_name(err));
    httpd_stop(this->server_);
    this->server_ = nullptr;
#if defined(ESP_PLATFORM) && defined(USE_PSRAM) && !defined(WEBDAV_FILING_HOST_TEST)
    WebDAVFiling::stop_async_get_workers_();
#endif
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG,
           "WebDAV listening on port %u; GET buffer %u bytes; sockets %u, backlog %u, URI %u, "
           "headers %u",
           static_cast<unsigned>(this->port_), static_cast<unsigned>(kFileBufferSize),
           static_cast<unsigned>(WEBDAV_FILING_HTTPD_MAX_OPEN_SOCKETS),
           static_cast<unsigned>(WEBDAV_FILING_HTTPD_BACKLOG_CONN),
           static_cast<unsigned>(WEBDAV_FILING_HTTPD_MAX_URI_LEN),
           static_cast<unsigned>(WEBDAV_FILING_HTTPD_MAX_REQ_HDR_LEN));
}

void WebDAVFiling::on_shutdown() {
#if defined(ESP_PLATFORM) && defined(USE_PSRAM) && !defined(WEBDAV_FILING_HOST_TEST)
  WebDAVFiling::stop_async_get_workers_();
#endif
  // Stop the server before tearing down the VFS: a synchronous PUT/PROPFIND
  // may still be running on the HTTPD task and must not see an unmounted card.
  if (this->server_ != nullptr) {
    httpd_stop(this->server_);
    this->server_ = nullptr;
  }
  if (this->vfs_ != nullptr)
    this->vfs_->shutdown();
}

void WebDAVFiling::dump_config() {
  ESP_LOGCONFIG(TAG, "WebDAVFiling (native ESP-IDF HTTP server):");
  ESP_LOGCONFIG(TAG, "  port: %u", static_cast<unsigned>(this->port_));
  ESP_LOGCONFIG(TAG, "  base path: %s", this->base_path_.c_str());
  switch (this->auth_type_) {
    case AuthType::BASIC:
      ESP_LOGCONFIG(TAG, "  auth: Basic user=%s", this->auth_username_.c_str());
      break;
    case AuthType::DIGEST:
      ESP_LOGCONFIG(TAG, "  auth: Digest user=%s", this->auth_username_.c_str());
      break;
    case AuthType::NONE:
      ESP_LOGCONFIG(TAG, "  auth: disabled");
      break;
  }
  if (this->sd_.bus == SdBus::SDMMC) {
    ESP_LOGCONFIG(TAG,
                  "  SDMMC: slot=%u width=%u clk=%d cmd=%d d0=%d d1=%d d2=%d d3=%d freq=%lu "
                  "ldo=%u",
                  this->sd_.slot, this->sd_.width, this->sd_.clk, this->sd_.cmd, this->sd_.d0,
                  this->sd_.d1, this->sd_.d2, this->sd_.d3,
                  static_cast<unsigned long>(this->sd_.frequency),
                  this->sd_.pwr_ctrl_ldo_channel);
  } else {
    ESP_LOGCONFIG(TAG, "  SD SPI: clk=%d miso=%d mosi=%d cs=%d freq=%lu", this->sd_.clk,
                  this->sd_.miso, this->sd_.mosi, this->sd_.cs,
                  static_cast<unsigned long>(this->sd_.frequency));
  }
}

}  // namespace webdav_filing
}  // namespace esphome
