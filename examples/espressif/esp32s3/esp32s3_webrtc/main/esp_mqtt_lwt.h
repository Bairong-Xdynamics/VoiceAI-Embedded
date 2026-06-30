#ifndef ESP_MQTT_LWT_H
#define ESP_MQTT_LWT_H

#include "mqtt.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <mqtt_client.h>
#include <string>

class EspMqttLwt : public Mqtt {
public:
    EspMqttLwt();
    ~EspMqttLwt() override;

    EspMqttLwt(const EspMqttLwt&) = delete;
    EspMqttLwt& operator=(const EspMqttLwt&) = delete;

    bool Connect(const std::string broker_address, int broker_port, const std::string client_id,
                 const std::string username, const std::string password) override;
    void Disconnect() override;
    bool Publish(const std::string topic, const std::string payload, int qos = 0) override;
    bool Subscribe(const std::string topic, int qos = 0) override;
    bool Unsubscribe(const std::string topic) override;
    bool IsConnected() override;
    int GetLastError() override;

    // 设置 MQTT 遗嘱（Last Will and Testament）消息。
    // 必须在调用 Connect() 之前设置才会生效；retain=true 时由 broker 保留为离线状态。
    void SetLastWill(const std::string& topic, const std::string& payload, int qos = 1, bool retain = false);
    void ClearLastWill();

private:
    void MqttEventCallback(esp_event_base_t base, int32_t event_id, void* event_data);

    bool connected_ = false;
    EventGroupHandle_t event_group_handle_ = nullptr;
    std::string message_payload_;
    esp_mqtt_client_handle_t mqtt_client_handle_ = nullptr;
    int last_error_ = 0;

    bool last_will_enabled_ = false;
    std::string last_will_topic_;
    std::string last_will_payload_;
    int last_will_qos_ = 1;
    bool last_will_retain_ = false;
};

#endif  // ESP_MQTT_LWT_H
