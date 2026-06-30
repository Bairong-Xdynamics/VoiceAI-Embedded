#include "esp_mqtt_lwt.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>

namespace {
constexpr int kMqttConnectTimeoutMs = 10000;
constexpr EventBits_t kMqttConnectedEvent = BIT0;
constexpr EventBits_t kMqttDisconnectedEvent = BIT1;
constexpr EventBits_t kMqttErrorEvent = BIT2;
constexpr const char* TAG = "EspMqttLwt";
}  // namespace

EspMqttLwt::EspMqttLwt() {
    event_group_handle_ = xEventGroupCreate();
}

EspMqttLwt::~EspMqttLwt() {
    Disconnect();
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
        event_group_handle_ = nullptr;
    }
}

void EspMqttLwt::SetLastWill(const std::string& topic, const std::string& payload, int qos, bool retain) {
    last_will_enabled_ = !topic.empty();
    last_will_topic_ = topic;
    last_will_payload_ = payload;
    last_will_qos_ = qos;
    last_will_retain_ = retain;
}

void EspMqttLwt::ClearLastWill() {
    last_will_enabled_ = false;
    last_will_topic_.clear();
    last_will_payload_.clear();
}

bool EspMqttLwt::Connect(const std::string broker_address, int broker_port, const std::string client_id,
                         const std::string username, const std::string password) {
    if (mqtt_client_handle_ != nullptr) {
        Disconnect();
    }

    esp_mqtt_client_config_t mqtt_config = {};
    mqtt_config.task.stack_size = 4096;
    mqtt_config.broker.address.hostname = broker_address.c_str();
    mqtt_config.broker.address.port = broker_port;
    if (broker_port == 8883) {
        mqtt_config.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
        mqtt_config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    } else {
        mqtt_config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    }
    mqtt_config.credentials.client_id = client_id.c_str();
    mqtt_config.credentials.username = username.c_str();
    mqtt_config.credentials.authentication.password = password.c_str();
    mqtt_config.session.keepalive = keep_alive_seconds_;

    if (last_will_enabled_ && !last_will_topic_.empty()) {
        mqtt_config.session.last_will.topic = last_will_topic_.c_str();
        mqtt_config.session.last_will.msg = last_will_payload_.c_str();
        mqtt_config.session.last_will.msg_len = static_cast<int>(last_will_payload_.size());
        mqtt_config.session.last_will.qos = last_will_qos_;
        mqtt_config.session.last_will.retain = last_will_retain_ ? 1 : 0;
        ESP_LOGI(TAG, "Last will configured: topic=%s qos=%d retain=%d len=%d",
                 last_will_topic_.c_str(), last_will_qos_, last_will_retain_ ? 1 : 0,
                 static_cast<int>(last_will_payload_.size()));
    }

    mqtt_client_handle_ = esp_mqtt_client_init(&mqtt_config);
    if (mqtt_client_handle_ == nullptr) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return false;
    }
    esp_mqtt_client_register_event(
        mqtt_client_handle_, MQTT_EVENT_ANY,
        [](void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
            static_cast<EspMqttLwt*>(handler_args)->MqttEventCallback(base, event_id, event_data);
        },
        this);
    esp_mqtt_client_start(mqtt_client_handle_);

    auto bits = xEventGroupWaitBits(event_group_handle_,
                                    kMqttConnectedEvent | kMqttDisconnectedEvent | kMqttErrorEvent,
                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(kMqttConnectTimeoutMs));
    return (bits & kMqttConnectedEvent) != 0;
}

void EspMqttLwt::MqttEventCallback(esp_event_base_t base, int32_t event_id, void* event_data) {
    auto event = static_cast<esp_mqtt_event_t*>(event_data);
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        if (!connected_) {
            connected_ = true;
            if (on_connected_callback_) {
                on_connected_callback_();
            }
        }
        xEventGroupSetBits(event_group_handle_, kMqttConnectedEvent);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (last_will_enabled_ && !last_will_topic_.empty()) {
            ESP_LOGW(TAG, "MQTT disconnected (non-graceful path); broker will publish LWT to topic=%s qos=%d retain=%d",
                     last_will_topic_.c_str(), last_will_qos_, last_will_retain_ ? 1 : 0);
        } else {
            ESP_LOGW(TAG, "MQTT disconnected; no LWT configured");
        }
        if (connected_) {
            connected_ = false;
            if (on_disconnected_callback_) {
                on_disconnected_callback_();
            }
        }
        xEventGroupSetBits(event_group_handle_, kMqttDisconnectedEvent);
        break;
    case MQTT_EVENT_DATA: {
        auto topic = std::string(event->topic, event->topic_len);
        auto payload = std::string(event->data, event->data_len);
        if (event->data_len == event->total_data_len) {
            if (on_message_callback_) {
                on_message_callback_(topic, payload);
            }
        } else {
            message_payload_.append(payload);
            if (message_payload_.size() >= static_cast<size_t>(event->total_data_len) && on_message_callback_) {
                on_message_callback_(topic, message_payload_);
                message_payload_.clear();
            }
        }
        break;
    }
    case MQTT_EVENT_BEFORE_CONNECT:
    case MQTT_EVENT_SUBSCRIBED:
    case MQTT_EVENT_UNSUBSCRIBED:
    case MQTT_EVENT_PUBLISHED:
        break;
    case MQTT_EVENT_ERROR: {
        last_error_ = event->error_handle->esp_tls_last_esp_err;
        xEventGroupSetBits(event_group_handle_, kMqttErrorEvent);
        const char* error_name = esp_err_to_name(event->error_handle->esp_tls_last_esp_err);
        ESP_LOGW(TAG, "MQTT error: type=%d connect_rc=%d tls_err=0x%x esp_err=%s%s",
                 static_cast<int>(event->error_handle->error_type),
                 static_cast<int>(event->error_handle->connect_return_code),
                 event->error_handle->esp_tls_last_esp_err,
                 error_name ? error_name : "",
                 (last_will_enabled_ && !last_will_topic_.empty())
                     ? "; broker may publish LWT after keepalive timeout"
                     : "");
        if (on_error_callback_) {
            on_error_callback_(error_name ? error_name : "MQTT error");
        }
        break;
    }
    default:
        ESP_LOGI(TAG, "Unhandled event id %ld", event_id);
        break;
    }
}

void EspMqttLwt::Disconnect() {
    if (mqtt_client_handle_ != nullptr) {
        if (connected_) {
            ESP_LOGI(TAG, "MQTT graceful disconnect; LWT will NOT be triggered%s",
                     (last_will_enabled_ && !last_will_topic_.empty()) ? " (LWT was configured)" : "");
        }
        esp_mqtt_client_stop(mqtt_client_handle_);
        esp_mqtt_client_destroy(mqtt_client_handle_);
        mqtt_client_handle_ = nullptr;
    }
    connected_ = false;
    if (event_group_handle_ != nullptr) {
        xEventGroupClearBits(event_group_handle_,
                             kMqttConnectedEvent | kMqttDisconnectedEvent | kMqttErrorEvent);
    }
}

bool EspMqttLwt::Publish(const std::string topic, const std::string payload, int qos) {
    if (!connected_) {
        return false;
    }
    int msg_id = esp_mqtt_client_publish(mqtt_client_handle_, topic.c_str(), payload.data(),
                                         payload.size(), qos, 0);
    return (qos == 0) ? (msg_id == 0) : (msg_id > 0);
}

bool EspMqttLwt::Subscribe(const std::string topic, int qos) {
    if (!connected_) {
        return false;
    }
    return esp_mqtt_client_subscribe_single(mqtt_client_handle_, topic.c_str(), qos) > 0;
}

bool EspMqttLwt::Unsubscribe(const std::string topic) {
    if (!connected_) {
        return false;
    }
    return esp_mqtt_client_unsubscribe(mqtt_client_handle_, topic.c_str()) > 0;
}

bool EspMqttLwt::IsConnected() {
    return connected_;
}

int EspMqttLwt::GetLastError() {
    return last_error_;
}
