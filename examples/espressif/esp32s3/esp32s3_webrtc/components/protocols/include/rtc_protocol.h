#ifndef RTC_PROTOCOL_H
#define RTC_PROTOCOL_H

#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define RTC_PROTOCOL_JOIN_SUCCESS_EVENT (1 << 0)

#define RTC_BANDWIDTH_ESTIMATE_MIN_BITRATE   (500000)
#define RTC_BANDWIDTH_ESTIMATE_MAX_BITRATE   (2000000)
#define RTC_BANDWIDTH_ESTIMATE_START_BITRATE (750000)

class RtcProtocol : public Protocol {
public:
    RtcProtocol(const std::string& robot_key, const std::string& robot_token);
    ~RtcProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

    bool SendVideoFrame(const uint8_t* data, size_t len);
    void DeliverIncomingAudio(std::unique_ptr<AudioStreamPacket> packet);
    void HandleStreamMessage(std::string payload);

    void OnJoinSuccess();
    void OnConnectionLost();
    void OnUserOffline(uint32_t uid, int reason);
    void OnError(int code, const std::string& message);

private:
    void VoiceStopIfNeeded();
    bool FetchVoiceSession(std::string& app_id, std::string& token, std::string& channel_name, uint32_t& uid, std::string& license);
    bool SendText(const std::string& text) override;

    EventGroupHandle_t event_group_handle_;
    mutable std::mutex channel_mutex_;
    uint32_t conn_id_;
    int data_stream_id_;
    bool join_success_;

    std::string robot_key_;
    std::string robot_token_;
    std::string room_name_;
    std::string voice_user_name_;
};

#endif // RTC_PROTOCOL_H
