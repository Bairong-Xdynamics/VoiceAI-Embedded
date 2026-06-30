#include "tls13_test.h"

#include <cstring>
#include <string>

#include "board.h"
#include "esp_log_timestamp.h"
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <http.h>
#include <esp_http_client.h>
#include <esp_log.h>

namespace {

constexpr const char* TAG = "Tls13Test";
constexpr size_t kResponseCap = 1024;

struct Tls13TestBuffer {
    char data[kResponseCap];
    size_t len = 0;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->user_data == nullptr || evt->data_len <= 0) {
        return ESP_OK;
    }

    auto* buf = static_cast<Tls13TestBuffer*>(evt->user_data);
    const size_t space = kResponseCap - 1 - buf->len;
    const size_t copy_len = evt->data_len < space ? static_cast<size_t>(evt->data_len) : space;
    if (copy_len > 0) {
        memcpy(buf->data + buf->len, evt->data, copy_len);
        buf->len += copy_len;
        buf->data[buf->len] = '\0';
    }
    return ESP_OK;
}

}  // namespace

bool RunTls13Test() {
#if !CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
    ESP_LOGE(TAG, "TLS 1.3 is not enabled (CONFIG_MBEDTLS_SSL_PROTO_TLS1_3)");
    return false;
#endif

    Tls13TestBuffer response;

    esp_http_client_config_t config = {};
    config.url = "https://www.howsmyssl.com/a/check";
    config.method = HTTP_METHOD_GET;
    config.event_handler = HttpEventHandler;
    config.user_data = &response;
    config.timeout_ms = 20000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_3;

    ESP_LOGI(TAG, "Starting TLS 1.3 HTTPS test => %s", config.url);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return false;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status: %d", status);
        return false;
    }

    ESP_LOGI(TAG, "Response (%u bytes): %s", static_cast<unsigned>(response.len), response.data);

    if (response.len >= kResponseCap - 1) {
        ESP_LOGE(TAG, "Response truncated (%u bytes); increase kResponseCap", static_cast<unsigned>(response.len));
        return false;
    }

    if (strstr(response.data, "\"tls_version\":\"TLS 1.3\"") == nullptr) {
        ESP_LOGE(TAG, "Negotiated protocol is not TLS 1.3");
        return false;
    }

    ESP_LOGI(TAG, "TLS 1.3 test passed");
    return true;
}

bool RunHttpTest() {
    constexpr const char* kHttpTestTag = "HttpTest";
    constexpr const char* kUrl = "https://www.howsmyssl.com/a/check";

    auto& board = Board::GetInstance();
    auto http = board.GetNetwork()->CreateHttp(0);
    if (!http) {
        ESP_LOGE(kHttpTestTag, "CreateHttp failed");
        return false;
    }

    ESP_LOGI(kHttpTestTag, "Starting HTTP test => %s", kUrl);

    if (!http->Open("GET", kUrl)) {
        ESP_LOGE(kHttpTestTag, "Open GET failed");
        return false;
    }

    const int status = http->GetStatusCode();
    const std::string body = http->ReadAll();

    if (status != 200) {
        ESP_LOGE(kHttpTestTag, "Unexpected HTTP status: %d", status);
        return false;
    }

    if (body.empty()) {
        ESP_LOGE(kHttpTestTag, "Empty response body");
        return false;
    }

    const size_t log_len = body.size() > 512 ? 512 : body.size();
    ESP_LOGI(kHttpTestTag, "Response (%u bytes): %.*s%s", static_cast<unsigned>(body.size()),
             static_cast<int>(log_len), body.c_str(), body.size() > log_len ? "..." : "");

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGE(kHttpTestTag, "Invalid JSON response");
        return false;
    }

    cJSON* tls_item = cJSON_GetObjectItem(root, "tls_version");
    if (!cJSON_IsString(tls_item) || tls_item->valuestring == nullptr) {
        ESP_LOGE(kHttpTestTag, "Response missing tls_version field");
        cJSON_Delete(root);
        return false;
    }

    ESP_LOGI(kHttpTestTag, "tls_version: %s", tls_item->valuestring);
    cJSON_Delete(root);

    ESP_LOGI(kHttpTestTag, "HTTP test passed");
    return true;
}
