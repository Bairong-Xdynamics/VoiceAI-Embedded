#include "activate_api.h"

#include <http.h>

#include <cJSON.h>
#include <esp_log.h>

#include <cmath>
#include <string>

static const char* TAG = "activate_api";

namespace activate_api {
namespace {

std::string BuildCommonBody(const std::string& version, const std::string& device_id,
                            const std::string& user_id) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", version.c_str());
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "user_id", user_id.c_str());
    char* printed = cJSON_PrintUnformatted(root);
    std::string json(printed ? printed : "{}");
    if (printed) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return json;
}

void ApplyBearer(Http* http, const std::string& bearer_access_token) {
    if (!bearer_access_token.empty()) {
        http->SetHeader("Authorization", "Bearer " + bearer_access_token);
    }
}

std::string UrlEncodeQueryValue(const std::string& s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[(c >> 4) & 0xF];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

std::string AppendCommonQueryParams(const std::string& endpoint_url, const std::string& version,
                                    const std::string& device_id, const std::string& user_id) {
    const std::string q = std::string("version=") + UrlEncodeQueryValue(version) + "&device_id=" +
                          UrlEncodeQueryValue(device_id) + "&user_id=" + UrlEncodeQueryValue(user_id);
    const char* sep = (endpoint_url.find('?') == std::string::npos) ? "?" : "&";
    return endpoint_url + sep + q;
}

std::string JsonCodeString(cJSON* root) {
    cJSON* code_item = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsString(code_item) && code_item->valuestring) {
        return code_item->valuestring;
    }
    if (cJSON_IsNumber(code_item)) {
        double v = cJSON_GetNumberValue(code_item);
        if (std::floor(v) == v) {
            return std::to_string(static_cast<int64_t>(v));
        }
        return std::to_string(v);
    }
    return {};
}

bool BusinessOk(const std::string& code) {
    return code == kBusinessSuccessCode;
}

int64_t JsonInt64(cJSON* item) {
    if (!item || !cJSON_IsNumber(item)) {
        return 0;
    }
    double v = cJSON_GetNumberValue(item);
    return static_cast<int64_t>(v);
}

std::string JsonStringField(cJSON* obj, const char* key) {
    if (!obj) {
        return {};
    }
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return {};
}

esp_err_t PostJson(Http* http, const std::string& endpoint_url, std::string&& json_body,
                   const std::string& bearer_access_token, cJSON** out_root, std::string* raw_body,
                   std::string* out_code) {
    if (!http || !out_root || !raw_body || !out_code) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_root = nullptr;
    raw_body->clear();
    out_code->clear();

    http->SetHeader("Content-Type", "application/json");
    ApplyBearer(http, bearer_access_token);
    http->SetContent(std::move(json_body));

    if (!http->Open("POST", endpoint_url)) {
        ESP_LOGE(TAG, "Open POST failed: %s", endpoint_url.c_str());
        return ESP_FAIL;
    }

    const int status = http->GetStatusCode();
    *raw_body = http->ReadAll();

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d %s body=%s", status, endpoint_url.c_str(),
                 raw_body->length() > 256 ? "(truncated)" : raw_body->c_str());
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_root = cJSON_Parse(raw_body->c_str());
    if (!*out_root) {
        ESP_LOGE(TAG, "Invalid JSON: %s", raw_body->c_str());
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_code = JsonCodeString(*out_root);
    if (!BusinessOk(*out_code)) {
        cJSON* msg = cJSON_GetObjectItem(*out_root, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            ESP_LOGW(TAG, "business code=%s message=%s", out_code->c_str(), msg->valuestring);
        } else {
            ESP_LOGW(TAG, "business code=%s", out_code->c_str());
        }
        cJSON_Delete(*out_root);
        *out_root = nullptr;
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t GetJson(Http* http, const std::string& url_with_query, const std::string& bearer_access_token,
                  cJSON** out_root, std::string* raw_body, std::string* out_code) {
    if (!http || !out_root || !raw_body || !out_code) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_root = nullptr;
    raw_body->clear();
    out_code->clear();

    http->SetContent(std::string());
    ApplyBearer(http, bearer_access_token);

    if (!http->Open("GET", url_with_query)) {
        ESP_LOGE(TAG, "Open GET failed: %s", url_with_query.c_str());
        return ESP_FAIL;
    }

    const int status = http->GetStatusCode();
    *raw_body = http->ReadAll();

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d %s body=%s", status, url_with_query.c_str(),
                 raw_body->length() > 256 ? "(truncated)" : raw_body->c_str());
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_root = cJSON_Parse(raw_body->c_str());
    if (!*out_root) {
        ESP_LOGE(TAG, "Invalid JSON: %s", raw_body->c_str());
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_code = JsonCodeString(*out_root);
    if (!BusinessOk(*out_code)) {
        cJSON* msg = cJSON_GetObjectItem(*out_root, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            ESP_LOGW(TAG, "business code=%s message=%s", out_code->c_str(), msg->valuestring);
        } else {
            ESP_LOGW(TAG, "business code=%s", out_code->c_str());
        }
        cJSON_Delete(*out_root);
        *out_root = nullptr;
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

}  // namespace

std::string JoinEndpoint(const std::string& base_url, const std::string& path) {
    std::string base = base_url;
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
        base.pop_back();
    }
    std::string p = path;
    if (!p.empty() && p.front() != '/') {
        p.insert(p.begin(), '/');
    }
    return base + p;
}

esp_err_t SetActivate(Http* http, const std::string& endpoint_url, const std::string& version,
                      const std::string& device_id, const std::string& user_id,
                      const std::string& bearer_access_token, TokenPayload* out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = TokenPayload{};

    cJSON* root = nullptr;
    std::string raw;
    std::string code;
    esp_err_t err =
        PostJson(http, endpoint_url, BuildCommonBody(version, device_id, user_id),
                 bearer_access_token, &root, &raw, &code);
    out->raw_body = std::move(raw);
    out->code = std::move(code);

    if (err != ESP_OK || !root) {
        if (root) {
            cJSON_Delete(root);
        }
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* token_item = cJSON_GetObjectItem(data, "token");
    cJSON* ts_item = cJSON_GetObjectItem(data, "expire_timestamp");

    if (!cJSON_IsString(token_item) || !token_item->valuestring) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->token = token_item->valuestring;
    out->timestamp_ms = JsonInt64(ts_item);
    out->ok = true;

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t GetActivate(Http* http, const std::string& endpoint_url, const std::string& version,
                      const std::string& device_id, const std::string& user_id,
                      const std::string& bearer_access_token, ActivateStatePayload* out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = ActivateStatePayload{};

    const std::string url = AppendCommonQueryParams(endpoint_url, version, device_id, user_id);
    cJSON* root = nullptr;
    std::string raw;
    std::string code;
    esp_err_t err = GetJson(http, url, bearer_access_token, &root, &raw, &code);
    out->raw_body = std::move(raw);
    out->code = std::move(code);

    if (err != ESP_OK || !root) {
        if (root) {
            cJSON_Delete(root);
        }
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* state_item = cJSON_GetObjectItem(data, "state");
    const int state_val = static_cast<int>(JsonInt64(state_item));
    if (state_val != static_cast<int>(ActivationBindState::kNotActivated) &&
        state_val != static_cast<int>(ActivationBindState::kActivated)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->state = static_cast<ActivationBindState>(state_val);
    out->ok = true;

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ServerTime(Http* http, const std::string& endpoint_url, const std::string& version,
                     const std::string& device_id, const std::string& user_id,
                     const std::string& bearer_access_token, ServerTimePayload* out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = ServerTimePayload{};

    const std::string url = AppendCommonQueryParams(endpoint_url, version, device_id, user_id);
    cJSON* root = nullptr;
    std::string raw;
    std::string code;
    esp_err_t err = GetJson(http, url, bearer_access_token, &root, &raw, &code);
    out->raw_body = std::move(raw);
    out->code = std::move(code);

    if (err != ESP_OK || !root) {
        if (root) {
            cJSON_Delete(root);
        }
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* ts_item = cJSON_GetObjectItem(data, "timestamp");
    cJSON* tz_item = cJSON_GetObjectItem(data, "timezone_offset");

    out->timestamp_ms = JsonInt64(ts_item);
    out->timezone_offset_minutes = static_cast<int>(JsonInt64(tz_item));
    out->ok = true;

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t GetToken(Http* http, const std::string& endpoint_url, const std::string& version,
                   const std::string& device_id, const std::string& user_id,
                   const std::string& bearer_access_token, TokenPayload* out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = TokenPayload{};

    const std::string url = AppendCommonQueryParams(endpoint_url, version, device_id, user_id);
    cJSON* root = nullptr;
    std::string raw;
    std::string code;
    esp_err_t err = GetJson(http, url, bearer_access_token, &root, &raw, &code);
    out->raw_body = std::move(raw);
    out->code = std::move(code);

    if (err != ESP_OK || !root) {
        if (root) {
            cJSON_Delete(root);
        }
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* token_item = cJSON_GetObjectItem(data, "token");
    cJSON* ts_item = cJSON_GetObjectItem(data, "expire_timestamp");

    if (!cJSON_IsString(token_item) || !token_item->valuestring) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->token = token_item->valuestring;
    out->timestamp_ms = JsonInt64(ts_item);
    out->ok = true;

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t GetLinkConfig(Http* http, const std::string& endpoint_url, const std::string& version,
                    const std::string& device_id, const std::string& user_id,
                    const std::string& bearer_access_token, DeviceConfigPayload* out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = DeviceConfigPayload{};

    const std::string url = AppendCommonQueryParams(endpoint_url, version, device_id, user_id);
    cJSON* root = nullptr;
    std::string raw;
    std::string code;
    esp_err_t err = GetJson(http, url, bearer_access_token, &root, &raw, &code);
    out->raw_body = std::move(raw);
    out->code = std::move(code);

    if (err != ESP_OK || !root) {
        if (root) {
            cJSON_Delete(root);
        }
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->mqtt_url = JsonStringField(data, "mqtt_url");
    out->ws_voice_url = JsonStringField(data, "ws_voice_url");
    out->rtc_voice_url = JsonStringField(data, "rtc_voice_url");

    if (out->mqtt_url.empty() || out->ws_voice_url.empty()) {
        ESP_LOGE(TAG, "GetConfig: mqtt_url or ws_voice_url missing");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->ok = true;
    cJSON_Delete(root);
    return ESP_OK;
}

}  // namespace activate_api
