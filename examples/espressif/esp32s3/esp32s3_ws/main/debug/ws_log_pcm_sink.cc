#include "ws_log_pcm_sink.h"

#include "board.h"
#include "settings.h"
#include "sdkconfig.h"

#if CONFIG_USE_WS_DEBUG_SINK

#include <web_socket.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <cstdarg>
#include <cstring>
#include <memory>
#include <string>

// 二进制帧：魔数 + 小端 sample_rate + channels + reserved + sample_count + PCM int16 小端
static const char kPcmMagic[4] = {'X', 'Z', 'P', 'C'};

// kind: 0 = 日志文本, 1 = PCM
struct WsDbgQueueItem {
    uint8_t kind;
    char* log_line;
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t sample_count;
    int16_t* pcm;
};

namespace {

static void FreeQueueItem(WsDbgQueueItem* item) {
    if (!item) {
        return;
    }
    if (item->kind == 0) {
        free(item->log_line);
        item->log_line = nullptr;
    } else {
        free(item->pcm);
        item->pcm = nullptr;
    }
}

static std::string JsonEscape(const char* s) {
    std::string o;
    if (!s) {
        return o;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        char c = static_cast<char>(*p);
        switch (c) {
            case '"':
                o += "\\\"";
                break;
            case '\\':
                o += "\\\\";
                break;
            case '\n':
                o += "\\n";
                break;
            case '\r':
                o += "\\r";
                break;
            case '\t':
                o += "\\t";
                break;
            default:
                if (*p < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", *p);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

static bool SendLogJson(WebSocket& ws, const char* line) {
    std::string esc = JsonEscape(line);
    std::string json = "{\"type\":\"log\",\"text\":\"";
    json += esc;
    json += "\"}";
    return ws.Send(json);
}

static bool SendPcmFrame(WebSocket& ws, uint32_t sample_rate, uint16_t channels,
                         const int16_t* samples, size_t sample_count) {
    const size_t pcm_bytes = sample_count * sizeof(int16_t);
    std::string frame;
    frame.resize(16 + pcm_bytes);
    uint8_t* p = reinterpret_cast<uint8_t*>(frame.data());
    memcpy(p, kPcmMagic, 4);
    p += 4;
    memcpy(p, &sample_rate, 4);
    p += 4;
    memcpy(p, &channels, 2);
    p += 2;
    uint16_t reserved = 0;
    memcpy(p, &reserved, 2);
    p += 2;
    uint32_t sc = static_cast<uint32_t>(sample_count);
    memcpy(p, &sc, 4);
    p += 4;
    memcpy(p, samples, pcm_bytes);
    return ws.Send(frame.data(), frame.size(), true);
}

static QueueHandle_t g_queue = nullptr;
static TaskHandle_t g_task = nullptr;
static vprintf_like_t g_prev_vprintf = nullptr;
static std::string g_connect_url;
static bool g_started = false;

// 工作线程内禁止 ESP_LOG，否则会经 vprintf 钩子再次入队造成递归
static void WorkerTask(void* arg) {
    (void)arg;
    std::unique_ptr<WebSocket> ws;
    uint32_t backoff_ms = 1000;

    for (;;) {
        WsDbgQueueItem item{};
        BaseType_t got = xQueueReceive(g_queue, &item, pdMS_TO_TICKS(5000));
        if (!g_started || g_connect_url.empty()) {
            if (got == pdTRUE) {
                FreeQueueItem(&item);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (got != pdTRUE) {
            continue;
        }

        if (!ws) {
            auto network = Board::GetInstance().GetNetwork();
            ws = network->CreateWebSocket(1);
            if (!ws) {
                FreeQueueItem(&item);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms = backoff_ms < 16000 ? backoff_ms * 2 : 16000;
                continue;
            }
        }

        if (!ws->IsConnected()) {
            if (!ws->Connect(g_connect_url.c_str())) {
                FreeQueueItem(&item);
                ws.reset();
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms = backoff_ms < 16000 ? backoff_ms * 2 : 16000;
                continue;
            }
            backoff_ms = 1000;
        }

        bool ok = false;
        if (item.kind == 0) {
            ok = item.log_line && SendLogJson(*ws, item.log_line);
        } else {
            ok = item.pcm && item.sample_count > 0 &&
                 SendPcmFrame(*ws, item.sample_rate, item.channels, item.pcm, item.sample_count);
        }
        FreeQueueItem(&item);

        if (!ok) {
            ws.reset();
        }
    }
}

static int DebugLogVprintf(const char* fmt, va_list args) {
    char buf[512];
    va_list copy;
    va_copy(copy, args);
    vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);

    int ret = 0;
    if (g_prev_vprintf) {
        ret = g_prev_vprintf(fmt, args);
    }
    if (g_started && g_queue && g_connect_url.size() > 0) {
        WsLogPcmSink::Instance().EnqueueLogLine(buf);
    }
    return ret;
}

}  // namespace

#endif  // CONFIG_USE_WS_DEBUG_SINK

WsLogPcmSink& WsLogPcmSink::Instance() {
    static WsLogPcmSink inst;
    return inst;
}

void WsLogPcmSink::ConfigureFromNvs() {
#if CONFIG_USE_WS_DEBUG_SINK
    Settings settings("ws_debug", false);
    url_ = settings.GetString("url");
    if (url_.empty()) {
        url_ = CONFIG_WS_DEBUG_DEFAULT_URL;
    }
    enabled_ = !url_.empty();
    g_connect_url = url_;
#else
    (void)0;
#endif
}

void WsLogPcmSink::StartIfEnabled() {
#if CONFIG_USE_WS_DEBUG_SINK
    if (!enabled_ || started_) {
        return;
    }
    g_connect_url = url_;
    if (g_queue == nullptr) {
        g_queue = xQueueCreate(24, sizeof(WsDbgQueueItem));
        if (!g_queue) {
            return;
        }
    }
    g_started = true;
    if (g_task == nullptr) {
        if (xTaskCreate(WorkerTask, "ws_dbg", 4096, nullptr, 3, &g_task) != pdPASS) {
            g_started = false;
            return;
        }
    }
    if (g_prev_vprintf == nullptr) {
        g_prev_vprintf = esp_log_set_vprintf(DebugLogVprintf);
    }
    started_ = true;
#else
    (void)0;
#endif
}

void WsLogPcmSink::Stop() {
#if CONFIG_USE_WS_DEBUG_SINK
    g_started = false;
    if (g_prev_vprintf) {
        esp_log_set_vprintf(g_prev_vprintf);
        g_prev_vprintf = nullptr;
    }
    started_ = false;
    enabled_ = false;
    g_connect_url.clear();
    if (g_queue) {
        WsDbgQueueItem item{};
        while (xQueueReceive(g_queue, &item, 0) == pdTRUE) {
            FreeQueueItem(&item);
        }
    }
#else
    (void)0;
#endif
}

void WsLogPcmSink::EnqueueLogLine(const char* line) {
#if CONFIG_USE_WS_DEBUG_SINK
    if (!g_started || !g_queue || !line) {
        return;
    }
    WsDbgQueueItem item{};
    memset(&item, 0, sizeof(item));
    item.kind = 0;
    item.log_line = strdup(line);
    if (!item.log_line) {
        return;
    }
    if (xQueueSend(g_queue, &item, 0) != pdTRUE) {
        free(item.log_line);
    }
#else
    (void)line;
#endif
}

void WsLogPcmSink::EnqueuePcm(uint32_t sample_rate, uint16_t channels, const int16_t* samples,
                              size_t sample_count) {
#if CONFIG_USE_WS_DEBUG_SINK
    if (!g_started || !g_queue || !samples || sample_count == 0) {
        return;
    }
    const size_t bytes = sample_count * sizeof(int16_t);
    int16_t* copy = static_cast<int16_t*>(malloc(bytes));
    if (!copy) {
        return;
    }
    memcpy(copy, samples, bytes);
    WsDbgQueueItem item{};
    memset(&item, 0, sizeof(item));
    item.kind = 1;
    item.sample_rate = sample_rate;
    item.channels = channels;
    item.sample_count = static_cast<uint32_t>(sample_count);
    item.pcm = copy;
    if (xQueueSend(g_queue, &item, 0) != pdTRUE) {
        free(copy);
    }
#else
    (void)sample_rate;
    (void)channels;
    (void)samples;
    (void)sample_count;
#endif
}
