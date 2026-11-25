#pragma once

// ESPHome logging is irrelevant in host tests; swallow all log calls.
#define ESP_LOGV(...) ((void) 0)
#define ESP_LOGVV(...) ((void) 0)
#define ESP_LOGD(...) ((void) 0)
#define ESP_LOGI(...) ((void) 0)
#define ESP_LOGW(...) ((void) 0)
#define ESP_LOGE(...) ((void) 0)
#define ESP_LOGCONFIG(...) ((void) 0)
