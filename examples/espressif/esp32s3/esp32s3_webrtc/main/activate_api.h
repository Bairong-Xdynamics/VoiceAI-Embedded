#ifndef ACTIVATE_API_H
#define ACTIVATE_API_H

#include <esp_err.h>

#include <cstdint>
#include <string>

class Http;

namespace activate_api {

constexpr const char* kBusinessSuccessCode = "000000";

/** 服务端绑定激活状态：0 未激活，1 已激活（对应 get_activate 的 data.state） */
enum class ActivationBindState : int {
    kNotActivated = 0,
    kActivated = 1,
};

struct TokenPayload {
    bool ok = false;
    std::string code;
    std::string token;
    int64_t timestamp_ms = 0;
    std::string raw_body;
};

struct ActivateStatePayload {
    bool ok = false;
    std::string code;
    ActivationBindState state = ActivationBindState::kNotActivated;
    std::string raw_body;
};

struct ServerTimePayload {
    bool ok = false;
    std::string code;
    int64_t timestamp_ms = 0;
    /** 相对 UTC 的偏移（分钟），例如东八区为 480 */
    int timezone_offset_minutes = 0;
    std::string raw_body;
};

struct DeviceConfigPayload {
    bool ok = false;
    std::string code;
    std::string mqtt_url;
    std::string ws_voice_url;
    std::string rtc_voice_url;
    std::string raw_body;
};

esp_err_t SetActivate(Http* http, const std::string& endpoint_url, const std::string& version,
                      const std::string& device_id, const std::string& user_id,
                      const std::string& bearer_access_token, TokenPayload* out);

esp_err_t GetActivate(Http* http, const std::string& endpoint_url, const std::string& version,
                      const std::string& device_id, const std::string& user_id,
                      const std::string& bearer_access_token, ActivateStatePayload* out);

esp_err_t ServerTime(Http* http, const std::string& endpoint_url, const std::string& version,
                     const std::string& device_id, const std::string& user_id,
                     const std::string& bearer_access_token, ServerTimePayload* out);

esp_err_t GetToken(Http* http, const std::string& endpoint_url, const std::string& version,
                   const std::string& device_id, const std::string& user_id,
                   const std::string& bearer_access_token, TokenPayload* out);

esp_err_t GetLinkConfig(Http* http, const std::string& endpoint_url, const std::string& version,
                    const std::string& device_id, const std::string& user_id,
                    const std::string& bearer_access_token, DeviceConfigPayload* out);

/** 将 base（如 https://xxx-xxx）与 path（如 /set_activate）拼接 */
std::string JoinEndpoint(const std::string& base_url, const std::string& path);

}  // namespace activate_api

#endif  // ACTIVATE_API_H
