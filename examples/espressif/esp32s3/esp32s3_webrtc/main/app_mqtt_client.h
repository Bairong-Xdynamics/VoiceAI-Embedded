#ifndef APP_MQTT_CLIENT_H
#define APP_MQTT_CLIENT_H

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include <esp_timer.h>
#include <mqtt.h>

struct cJSON;
struct MqttOtaUpdateOffer {
    bool update = false;
    std::string version;
    std::string firmware_url;
    std::string md5;
};

struct MqttConfigData {
    std::string robot_key;
    std::string robot_token;
    std::string wake_word;
    std::string wake_name;
    int volume = -1;
    std::string model_config;
};

class MqttClient {
public:
    MqttClient() = default;
    ~MqttClient();

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    bool ConnectFromSettings(const std::string& broker_endpoint, const std::string& token);
    void Disconnect();

    // 设置自定义 MQTT 遗嘱（Last Will and Testament）消息；必须在调用
    // ConnectFromSettings 之前生效。若未调用，将在连接时自动构造一份默认遗嘱
    // （msg_type=offline）并发布到默认上行主题。
    void SetLastWill(const std::string& topic, const std::string& payload, int qos = 1, bool retain = false);
    // 关闭遗嘱消息（默认与自定义均不发送）
    void DisableLastWill();

    bool IsConnected();

    bool Publish(const std::string& topic, const std::string& payload, int qos = 1);
    bool PublishDefault(const std::string& payload, int qos = 1);
    bool Subscribe(const std::string& topic, int qos = 1);
    bool Unsubscribe(const std::string& topic);

    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);
    void OnMessage(std::function<void(const std::string& topic, const std::string& payload)> callback);


    void OnOtaUpdateOffer(
        std::function<void(const std::string& request_id, const std::string& code, const MqttOtaUpdateOffer& offer)>
            callback);
    void OnGetConfigResponse(
        std::function<void(const std::string& request_id, const std::string& code, const MqttConfigData& config)>
            callback);
    void OnSetVolume(
        std::function<void(const std::string& request_id, const std::string& code, int volume)> callback);
    void OnSetWakeWord(std::function<void(const std::string& request_id, const std::string& code,
                                          const std::string& wake_word, const std::string& wake_name)> callback);
    void OnPushUnbind(
        std::function<void(const std::string& request_id, const std::string& device_id)> callback);

    std::string NextRequestId();

    bool SendGetConfig(const std::string& request_id = {});
    bool SendHeartBeat(const std::string& request_id = {});
    bool SendOtaUpdateState(int state, const std::string& desc, const std::string& request_id = {});
    bool SendVoiceState(int state, const std::string& request_id = {});
    bool SendDeviceLog(const std::string& time, const std::string& level, const std::string& log,
                       const std::string& event, const std::string& desc,
                       const std::string& request_id = {});
    bool SendSetVolume(int volume, const std::string& request_id = {});
    bool SendPushUnbind(const std::string& request_id);

    int GetLastError() const;

    const std::string& DefaultPublishTopic() const { return default_publish_topic_; }

private:
    void AttachMqttCallbacks();
    void SubscribeDefaultTopicIfNeeded();
    void HandleIncomingPayload(const std::string& payload);
    bool IsDuplicateIncomingMessage(const std::string& msg_type, const std::string& request_id);
    bool PublishProtocolMessage(const char* msg_type, const std::string& request_id,
                                const std::function<void(cJSON* data)>& fill_data = {});
    bool DoConnect();
    void EnsureReconnectTimer();
    void StopReconnectTimer();
    void ScheduleReconnect();
    void TryReconnect();
    static void ReconnectTimerCallback(void* arg);
    std::string BuildDefaultLastWillPayload() const;

    std::unique_ptr<Mqtt> mqtt_;
    std::string default_publish_topic_;
    std::string default_subscribe_topic_;
    std::string auth_token_;
    std::string broker_endpoint_;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    bool auto_reconnect_ = false;
    bool connecting_ = false;
    int last_error_ = 0;

    enum class LastWillMode { kDefault, kCustom, kDisabled };
    LastWillMode last_will_mode_ = LastWillMode::kDefault;
    std::string last_will_topic_;
    std::string last_will_payload_;
    int last_will_qos_ = 1;
    bool last_will_retain_ = false;

    std::function<void()> user_on_connected_;
    std::function<void()> user_on_disconnected_;
    std::function<void(const std::string& topic, const std::string& payload)> user_on_message_;
    std::function<void(const std::string& msg_type, const std::string& request_id, const std::string& code,
                       bool activated)>
        user_on_activate_response_;
    std::function<void(const std::string& request_id, const std::string& code, int64_t timestamp_ms,
                       int timezone_offset_min)>
        user_on_server_time_response_;
    std::function<void(const std::string& request_id, const std::string& code, const MqttOtaUpdateOffer& offer)>
        user_on_ota_update_;
    std::function<void(const std::string& request_id, const std::string& code, const MqttConfigData& config)>
        user_on_get_config_response_;
    std::function<void(const std::string& request_id, const std::string& code, int volume)> user_on_set_volume_;
    std::function<void(const std::string& request_id, const std::string& code, const std::string& wake_word,
                       const std::string& wake_name)>
        user_on_set_wake_word_;
    std::function<void(const std::string& request_id, const std::string& device_id)> user_on_push_unbind_;

    std::mutex dedup_mutex_;
    std::deque<std::string> dedup_order_;
    std::unordered_set<std::string> dedup_cache_;
};

#endif
