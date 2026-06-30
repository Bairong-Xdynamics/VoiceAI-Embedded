#include "app_mqtt_client.h"
#include "config.h"
#include "esp_mqtt_lwt.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <cstdio>
#include <cstdlib>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <utility>

#define TAG "MqttClient"

namespace {

constexpr size_t kDedupCacheLimit = 256;
constexpr int kDefaultBrokerPort = 8883;
constexpr int64_t kReconnectIntervalUs = 5 * 1000000LL;

struct CJsonDelete {
    void operator()(cJSON* ptr) const {
        if (ptr != nullptr) {
            cJSON_Delete(ptr);
        }
    }
};

struct CJsonFree {
    void operator()(char* ptr) const {
        if (ptr != nullptr) {
            cJSON_free(ptr);
        }
    }
};

using CJsonPtr = std::unique_ptr<cJSON, CJsonDelete>;
using CJsonCharPtr = std::unique_ptr<char, CJsonFree>;

std::string ResolveRequestId(MqttClient* client, const std::string& request_id) {
    return request_id.empty() ? client->NextRequestId() : request_id;
}

std::string GenerateRandomUuidV4() {
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = static_cast<uint8_t>((b[6] & 0x0f) | 0x40);
    b[8] = static_cast<uint8_t>((b[8] & 0x3f) | 0x80);
    char buf[37];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(buf);
}

std::string JsonRequestIdString(const cJSON* item) {
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return std::string(item->valuestring);
    }
    if (cJSON_IsNumber(item)) {
        return std::to_string(static_cast<int64_t>(cJSON_GetNumberValue(item)));
    }
    return {};
}

bool JsonBool(const cJSON* item, bool default_value) {
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    if (cJSON_IsNumber(item)) {
        return cJSON_GetNumberValue(item) != 0.0;
    }
    return default_value;
}

std::string JsonOptString(cJSON* obj, const char* key) {
    cJSON* it = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(it) || it->valuestring == nullptr) {
        return {};
    }
    return std::string(it->valuestring);
}

std::string JsonValueToString(cJSON* item) {
    if (item == nullptr) {
        return {};
    }
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return std::string(item->valuestring);
    }
    if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
        CJsonCharPtr printed(cJSON_PrintUnformatted(item));
        if (printed) {
            return std::string(printed.get());
        }
    }
    return {};
}

int JsonOptInt(cJSON* obj, const char* key, int default_value) {
    cJSON* it = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(it)) {
        return default_value;
    }
    return static_cast<int>(cJSON_GetNumberValue(it));
}

bool ParseEndpoint(const std::string& endpoint, std::string* address, int* port) {
    if (endpoint.empty()) {
        return false;
    }
    *address = endpoint;
    *port = kDefaultBrokerPort;

    const size_t pos = endpoint.find(':');
    if (pos == std::string::npos) {
        return true;
    }

    *address = endpoint.substr(0, pos);
    const std::string port_str = endpoint.substr(pos + 1);
    const long parsed_port = strtol(port_str.c_str(), nullptr, 10);
    if (parsed_port > 0 && parsed_port <= 65535) {
        *port = static_cast<int>(parsed_port);
    } else {
        ESP_LOGW(TAG, "Invalid MQTT port: %s, fallback to %d", port_str.c_str(), kDefaultBrokerPort);
    }
    return !address->empty();
}

std::string FormatTimestampMsString() {
    struct timeval tv {};
    if (gettimeofday(&tv, nullptr) != 0) {
        return "0";
    }
    const int64_t ms =
        static_cast<int64_t>(tv.tv_sec) * 1000 + static_cast<int64_t>(tv.tv_usec) / 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(ms));
    return std::string(buf);
}

}  // namespace

MqttClient::~MqttClient() {
    auto_reconnect_ = false;
    StopReconnectTimer();
    if (reconnect_timer_) {
        esp_timer_delete(reconnect_timer_);
        reconnect_timer_ = nullptr;
    }
    Disconnect();
}

void MqttClient::EnsureReconnectTimer() {
    if (reconnect_timer_) {
        return;
    }
    esp_timer_create_args_t timer_args = {
        .callback = ReconnectTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mqtt_reconnect",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reconnect_timer_));
}

void MqttClient::StopReconnectTimer() {
    if (reconnect_timer_) {
        esp_timer_stop(reconnect_timer_);
    }
}

void MqttClient::ScheduleReconnect() {
    if (!auto_reconnect_ || auth_token_.empty()) {
        return;
    }
    if (IsConnected() || connecting_) {
        return;
    }
    EnsureReconnectTimer();
    StopReconnectTimer();
    ESP_LOGI(TAG, "Schedule MQTT reconnect in %lld s", static_cast<long long>(kReconnectIntervalUs / 1000000));
    ESP_ERROR_CHECK(esp_timer_start_once(reconnect_timer_, kReconnectIntervalUs));
}

void MqttClient::ReconnectTimerCallback(void* arg) {
    auto* self = static_cast<MqttClient*>(arg);
    if (self) {
        self->TryReconnect();
    }
}

void MqttClient::TryReconnect() {
    if (!auto_reconnect_ || auth_token_.empty()) {
        return;
    }
    if (IsConnected() || connecting_) {
        return;
    }
    ESP_LOGI(TAG, "MQTT reconnect timer fired");
    if (!DoConnect()) {
        ScheduleReconnect();
    }
}

void MqttClient::SubscribeDefaultTopicIfNeeded() {
    if (default_subscribe_topic_.empty()) {
        return;
    }
    if (!mqtt_ || !mqtt_->IsConnected()) {
        ESP_LOGW(TAG, "Skip default subscribe: not connected");
        return;
    }
    constexpr int kDefaultDownlinkQos = 1;
    if (!Subscribe(default_subscribe_topic_, kDefaultDownlinkQos)) {
        ESP_LOGW(TAG, "Default subscribe failed, topic=%s", default_subscribe_topic_.c_str());
    }
}

void MqttClient::AttachMqttCallbacks() {
    if (!mqtt_) {
        return;
    }
    mqtt_->OnConnected([this]() {
        ESP_LOGI(TAG, "MQTT connected");
        connecting_ = false;
        StopReconnectTimer();
        SubscribeDefaultTopicIfNeeded();
        if (user_on_connected_) {
            user_on_connected_();
        }
    });
    mqtt_->OnDisconnected([this]() {
        if (auto_reconnect_ && last_will_mode_ != LastWillMode::kDisabled) {
            ESP_LOGW(TAG, "MQTT disconnected unexpectedly; broker is expected to publish our LWT");
        } else {
            ESP_LOGI(TAG, "MQTT disconnected");
        }
        connecting_ = false;
        if (user_on_disconnected_) {
            user_on_disconnected_();
        }
        ScheduleReconnect();
    });
    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        HandleIncomingPayload(payload);
        if (user_on_message_) {
            user_on_message_(topic, payload);
        }
    });
}

bool MqttClient::ConnectFromSettings(const std::string& broker_endpoint, const std::string& token) {
    broker_endpoint_ = broker_endpoint;
    auth_token_ = token;
    auto_reconnect_ = true;
    StopReconnectTimer();
    if (!DoConnect()) {
        ScheduleReconnect();
        return false;
    }
    return true;
}

bool MqttClient::DoConnect() {
    if (auth_token_.empty()) {
        ESP_LOGW(TAG, "MQTT token is empty");
        last_error_ = -1;
        return false;
    }
    if (connecting_) {
        ESP_LOGW(TAG, "MQTT connect already in progress");
        return false;
    }
    if (IsConnected()) {
        return true;
    }

    connecting_ = true;

    if (mqtt_) {
        mqtt_->Disconnect();
        mqtt_.reset();
    }

    const std::string& endpoint = broker_endpoint_.empty() ? kMqttBrokerEndpoint : broker_endpoint_;
    const std::string client_id = SystemInfo::GetMacAddress();
    const std::string username = SystemInfo::GetMacAddress();
    const std::string password = auth_token_;
    const int keepalive_interval = 120;
    default_publish_topic_ = "smartvoice/" + SystemInfo::GetMacAddress() + "/qos1/up";
    default_subscribe_topic_ = "smartvoice/" + SystemInfo::GetMacAddress() + "/qos1/down";

    std::string broker_address;
    int broker_port = kDefaultBrokerPort;
    if (!ParseEndpoint(endpoint, &broker_address, &broker_port)) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified or invalid");
        last_error_ = -1;
        connecting_ = false;
        return false;
    }

    auto lwt_mqtt = std::make_unique<EspMqttLwt>();
    if (!lwt_mqtt) {
        ESP_LOGE(TAG, "Create MQTT client failed");
        last_error_ = -1;
        connecting_ = false;
        return false;
    }

    switch (last_will_mode_) {
    case LastWillMode::kDefault: {
        const std::string payload = BuildDefaultLastWillPayload();
        lwt_mqtt->SetLastWill(default_publish_topic_, payload, /*qos=*/1, /*retain=*/false);
        ESP_LOGI(TAG, "Default last will applied: topic=%s len=%d", default_publish_topic_.c_str(),
                 static_cast<int>(payload.size()));
        break;
    }
    case LastWillMode::kCustom:
        if (!last_will_topic_.empty()) {
            lwt_mqtt->SetLastWill(last_will_topic_, last_will_payload_, last_will_qos_, last_will_retain_);
            ESP_LOGI(TAG, "Custom last will applied: topic=%s qos=%d retain=%d len=%d",
                     last_will_topic_.c_str(), last_will_qos_, last_will_retain_ ? 1 : 0,
                     static_cast<int>(last_will_payload_.size()));
        }
        break;
    case LastWillMode::kDisabled:
        lwt_mqtt->ClearLastWill();
        ESP_LOGI(TAG, "Last will disabled");
        break;
    }

    mqtt_ = std::move(lwt_mqtt);
    mqtt_->SetKeepAlive(keepalive_interval);
    AttachMqttCallbacks();

    ESP_LOGI(TAG, "Connecting to %s:%d", broker_address.c_str(), broker_port);
    if (!mqtt_->Connect(broker_address, broker_port, client_id, username, password)) {
        last_error_ = mqtt_->GetLastError();
        ESP_LOGE(TAG, "Failed to connect, code=%d", last_error_);
        mqtt_.reset();
        connecting_ = false;
        return false;
    }

    last_error_ = 0;
    connecting_ = false;
    StopReconnectTimer();
    ESP_LOGI(TAG, "Connected to broker");
    return true;
}

void MqttClient::Disconnect() {
    auto_reconnect_ = false;
    StopReconnectTimer();
    connecting_ = false;
    if (mqtt_) {
        ESP_LOGI(TAG, "Active disconnect requested; LWT will NOT be triggered by broker");
        mqtt_->Disconnect();
        mqtt_.reset();
    }
}

void MqttClient::SetLastWill(const std::string& topic, const std::string& payload, int qos, bool retain) {
    last_will_mode_ = LastWillMode::kCustom;
    last_will_topic_ = topic;
    last_will_payload_ = payload;
    last_will_qos_ = qos;
    last_will_retain_ = retain;
}

void MqttClient::DisableLastWill() {
    last_will_mode_ = LastWillMode::kDisabled;
    last_will_topic_.clear();
    last_will_payload_.clear();
}

std::string MqttClient::BuildDefaultLastWillPayload() const {
    CJsonPtr root(cJSON_CreateObject());
    if (!root) {
        return {};
    }
    cJSON_AddStringToObject(root.get(), "msg_type", "offline");
    cJSON_AddStringToObject(root.get(), "version", SystemInfo::GetVersion().c_str());
    cJSON_AddStringToObject(root.get(), "request_id", GenerateRandomUuidV4().c_str());
    cJSON_AddStringToObject(root.get(), "timestamp", FormatTimestampMsString().c_str());
    cJSON_AddStringToObject(root.get(), "device_id", SystemInfo::GetMacAddress().c_str());
    cJSON* data = cJSON_CreateObject();
    if (data != nullptr) {
        cJSON_AddStringToObject(data, "reason", "lwt");
        cJSON_AddItemToObject(root.get(), "data", data);
    }

    CJsonCharPtr raw(cJSON_PrintUnformatted(root.get()));
    if (!raw) {
        return {};
    }
    return std::string(raw.get());
}

bool MqttClient::IsConnected() {
    return mqtt_ && mqtt_->IsConnected();
}

bool MqttClient::Publish(const std::string& topic, const std::string& payload, int qos) {
    if (!mqtt_ || !mqtt_->IsConnected()) {
        ESP_LOGW(TAG, "Publish skipped: not connected");
        return false;
    }
    if (!mqtt_->Publish(topic, payload, qos)) {
        last_error_ = mqtt_->GetLastError();
        ESP_LOGE(TAG, "Publish failed, code=%d", last_error_);
        return false;
    }
    return true;
}

bool MqttClient::PublishDefault(const std::string& payload, int qos) {
    if (default_publish_topic_.empty()) {
        ESP_LOGW(TAG, "publish_topic is empty in settings");
        return false;
    }
    return Publish(default_publish_topic_, payload, qos);
}

bool MqttClient::Subscribe(const std::string& topic, int qos) {
    if (!mqtt_ || !mqtt_->IsConnected()) {
        ESP_LOGW(TAG, "Subscribe skipped: not connected");
        return false;
    }
    if (!mqtt_->Subscribe(topic, qos)) {
        last_error_ = mqtt_->GetLastError();
        ESP_LOGE(TAG, "Subscribe failed, code=%d", last_error_);
        return false;
    }
    ESP_LOGI(TAG, "Subscribed: %s", topic.c_str());
    return true;
}

bool MqttClient::Unsubscribe(const std::string& topic) {
    if (!mqtt_ || !mqtt_->IsConnected()) {
        ESP_LOGW(TAG, "Unsubscribe skipped: not connected");
        return false;
    }
    if (!mqtt_->Unsubscribe(topic)) {
        last_error_ = mqtt_->GetLastError();
        ESP_LOGE(TAG, "Unsubscribe failed, code=%d", last_error_);
        return false;
    }
    return true;
}

void MqttClient::OnConnected(std::function<void()> callback) {
    user_on_connected_ = std::move(callback);
    if (mqtt_) {
        AttachMqttCallbacks();
    }
}

void MqttClient::OnDisconnected(std::function<void()> callback) {
    user_on_disconnected_ = std::move(callback);
    if (mqtt_) {
        AttachMqttCallbacks();
    }
}

void MqttClient::OnMessage(std::function<void(const std::string& topic, const std::string& payload)> callback) {
    user_on_message_ = std::move(callback);
    if (mqtt_) {
        AttachMqttCallbacks();
    }
}

int MqttClient::GetLastError() const {
    return last_error_;
}

std::string MqttClient::NextRequestId() {
    return GenerateRandomUuidV4();
}

bool MqttClient::PublishProtocolMessage(const char* msg_type, const std::string& request_id,
                                        const std::function<void(cJSON* data)>& fill_data) {
    CJsonPtr root(cJSON_CreateObject());
    if (!root) {
        return false;
    }

    cJSON_AddStringToObject(root.get(), "msg_type", msg_type);
    cJSON_AddStringToObject(root.get(), "version", SystemInfo::GetVersion().c_str());
    cJSON_AddStringToObject(root.get(), "request_id", request_id.c_str());
    cJSON_AddStringToObject(root.get(), "timestamp", FormatTimestampMsString().c_str());
    cJSON_AddStringToObject(root.get(), "device_id", SystemInfo::GetMacAddress().c_str());

    if (fill_data) {
        cJSON* data = cJSON_CreateObject();
        if (data == nullptr) {
            return false;
        }
        fill_data(data);
        cJSON_AddItemToObject(root.get(), "data", data);
    }

    CJsonCharPtr raw(cJSON_PrintUnformatted(root.get()));
    if (!raw) {
        return false;
    }
    return PublishDefault(std::string(raw.get()));
}

bool MqttClient::SendGetConfig(const std::string& request_id) {
    return PublishProtocolMessage("get_config", ResolveRequestId(this, request_id));
}

bool MqttClient::SendHeartBeat(const std::string& request_id) {
    return PublishProtocolMessage("heart_beat", ResolveRequestId(this, request_id));
}

bool MqttClient::SendOtaUpdateState(int state, const std::string& desc, const std::string& request_id) {
    return PublishProtocolMessage("ota_update", ResolveRequestId(this, request_id),
                                  [state, &desc](cJSON* data) {
                                      cJSON_AddNumberToObject(data, "state", state);
                                      cJSON_AddStringToObject(data, "desc", desc.c_str());
                                  });
}

bool MqttClient::SendVoiceState(int state, const std::string& request_id) {
    return PublishProtocolMessage("voice_state", ResolveRequestId(this, request_id),
                                  [state](cJSON* data) { cJSON_AddNumberToObject(data, "state", state); });
}

bool MqttClient::SendDeviceLog(const std::string& time, const std::string& level, const std::string& log,
                               const std::string& event, const std::string& desc,
                               const std::string& request_id) {
    return PublishProtocolMessage("device_log", ResolveRequestId(this, request_id),
                                  [&time, &level, &log, &event, &desc](cJSON* data) {
                                      cJSON_AddStringToObject(data, "time", time.c_str());
                                      cJSON_AddStringToObject(data, "level", level.c_str());
                                      cJSON_AddStringToObject(data, "content", log.c_str());
                                      cJSON_AddStringToObject(data, "event", event.c_str());
                                      cJSON_AddStringToObject(data, "desc", desc.c_str());
                                      cJSON_AddStringToObject(data, "module", "设备端");
                                  });
}

bool MqttClient::SendSetVolume(int volume, const std::string& request_id) {
    return PublishProtocolMessage("set_volume", ResolveRequestId(this, request_id),
                                  [volume](cJSON* data) { cJSON_AddNumberToObject(data, "volume", volume); });
}

bool MqttClient::SendPushUnbind(const std::string& request_id) {
    if (request_id.empty()) {
        ESP_LOGW(TAG, "SendPushUnbind skipped: request_id is empty");
        return false;
    }
    return PublishProtocolMessage("push_unbind", request_id);
}

void MqttClient::OnOtaUpdateOffer(
    std::function<void(const std::string& request_id, const std::string& code, const MqttOtaUpdateOffer& offer)>
        callback) {
    user_on_ota_update_ = std::move(callback);
}

void MqttClient::OnGetConfigResponse(
    std::function<void(const std::string& request_id, const std::string& code, const MqttConfigData& config)>
        callback) {
    user_on_get_config_response_ = std::move(callback);
}

void MqttClient::OnSetVolume(
    std::function<void(const std::string& request_id, const std::string& code, int volume)> callback) {
    user_on_set_volume_ = std::move(callback);
}

void MqttClient::OnSetWakeWord(std::function<void(const std::string& request_id, const std::string& code,
                                                 const std::string& wake_word, const std::string& wake_name)>
                                   callback) {
    user_on_set_wake_word_ = std::move(callback);
}

void MqttClient::OnPushUnbind(
    std::function<void(const std::string& request_id, const std::string& device_id)> callback) {
    user_on_push_unbind_ = std::move(callback);
}

bool MqttClient::IsDuplicateIncomingMessage(const std::string& msg_type, const std::string& request_id) {
    if (request_id.empty()) {
        return false;
    }
    const std::string key = msg_type + "#" + request_id;

    std::lock_guard<std::mutex> lock(dedup_mutex_);
    if (dedup_cache_.find(key) != dedup_cache_.end()) {
        return true;
    }

    dedup_cache_.insert(key);
    dedup_order_.push_back(key);
    if (dedup_order_.size() > kDedupCacheLimit) {
        const std::string oldest = dedup_order_.front();
        dedup_cache_.erase(oldest);
        dedup_order_.pop_front();
    }
    return false;
}

void MqttClient::HandleIncomingPayload(const std::string& payload) {
    ESP_LOGI(TAG, "HandleIncomingPayload: %s", payload.c_str());
    CJsonPtr root(cJSON_Parse(payload.c_str()));
    if (!root) {
        return;
    }

    cJSON* msg_type_item = cJSON_GetObjectItem(root.get(), "msg_type");
    if (!cJSON_IsString(msg_type_item) || msg_type_item->valuestring == nullptr) {
        return;
    }
    const std::string msg_type(msg_type_item->valuestring);

    cJSON* code_item = cJSON_GetObjectItem(root.get(), "code");
    const bool has_code = cJSON_IsString(code_item);
    const std::string code = has_code ? std::string(code_item->valuestring) : std::string();
    const std::string request_id = JsonRequestIdString(cJSON_GetObjectItem(root.get(), "request_id"));
    if (IsDuplicateIncomingMessage(msg_type, request_id)) {
        ESP_LOGW(TAG, "Drop duplicate message: msg_type=%s request_id=%s", msg_type.c_str(), request_id.c_str());
        return;
    }

    if (msg_type == "offline") {
        cJSON* data = cJSON_GetObjectItem(root.get(), "data");
        const std::string reason = cJSON_IsObject(data) ? JsonOptString(data, "reason") : std::string();
        const std::string peer_device = JsonOptString(root.get(), "device_id");
        ESP_LOGW(TAG, "Received LWT/offline: device_id=%s reason=%s request_id=%s",
                 peer_device.c_str(), reason.c_str(), request_id.c_str());
    }

    if (msg_type == "ota_update") {
        if (!user_on_ota_update_) {
            return;
        }
        MqttOtaUpdateOffer offer;
        cJSON* data = cJSON_GetObjectItem(root.get(), "data");
        if (cJSON_IsObject(data)) {
            offer.update = JsonBool(cJSON_GetObjectItem(data, "update"), false);
            offer.version = JsonOptString(data, "version");
            cJSON* url_fw = cJSON_GetObjectItem(data, "file_url");
            if (cJSON_IsString(url_fw) && url_fw->valuestring) {
                offer.firmware_url = url_fw->valuestring;
            }
            offer.md5 = JsonOptString(data, "file_md5");
        }
        user_on_ota_update_(request_id, code, offer);
        return;
    }

    if (msg_type == "get_config") {
        if (!user_on_get_config_response_) {
            return;
        }
        MqttConfigData cfg;
        cJSON* data = cJSON_GetObjectItem(root.get(), "data");
        if (cJSON_IsObject(data)) {
            cfg.robot_key = JsonOptString(data, "robot_key");
            cfg.robot_token = JsonOptString(data, "robot_token");
            cfg.wake_word = JsonOptString(data, "wake_word");
            cfg.wake_name = JsonOptString(data, "wake_name");
            cfg.volume = JsonOptInt(data, "volume", -1);
            cfg.model_config = JsonValueToString(cJSON_GetObjectItem(data, "model_config"));
        }
        user_on_get_config_response_(request_id, code, cfg);
        return;
    }

    if (msg_type == "set_volume") {
        if (!user_on_set_volume_) {
            return;
        }
        int volume = -1;
        cJSON* data = cJSON_GetObjectItem(root.get(), "data");
        if (cJSON_IsObject(data)) {
            volume = JsonOptInt(data, "volume", -1);
        }
        user_on_set_volume_(request_id, code, volume);
        return;
    }

    if (msg_type == "set_wake_word") {
        if (!user_on_set_wake_word_) {
            return;
        }
        std::string wake_word;
        std::string wake_name;
        cJSON* data = cJSON_GetObjectItem(root.get(), "data");
        if (cJSON_IsObject(data)) {
            wake_word = JsonOptString(data, "wake_word");
            wake_name = JsonOptString(data, "wake_name");
        }
        user_on_set_wake_word_(request_id, code, wake_word, wake_name);
        return;
    }

    if (msg_type == "push_unbind") {
        if (!user_on_push_unbind_) {
            return;
        }
        const std::string device_id = JsonOptString(root.get(), "device_id");
        user_on_push_unbind_(request_id, device_id);
        return;
    }
}
