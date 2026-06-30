#ifndef _TWILIO_PROTOCOL_H_
#define _TWILIO_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class EspNetwork;

#define TWILIO_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class TwilioProtocol : public Protocol {
public:
    TwilioProtocol(const std::string& robot_key, const std::string& robot_tokens, const std::string& model_config);
    ~TwilioProtocol();

    void SetNetWorkUrl(const std::string& config_url) override;
    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    bool SendText(const std::string& text) override;
    bool SendTwilioMedia(const uint8_t* pcm, size_t len);
    bool SendTwilioStop();
    void ParseTwilioMedia(const cJSON* root);
    void NotifyAudioChannelClosed();

    EventGroupHandle_t event_group_handle_;
    bool audio_channel_active_ = false;
    std::unique_ptr<EspNetwork> esp_network_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;
    uint32_t sequence_ = 1;
    uint32_t chunk_ = 0;
    bool first_outgoing_audio_logged_ = false;
    bool first_incoming_audio_logged_ = false;
    std::chrono::time_point<std::chrono::steady_clock> first_outgoing_audio_time_;

    std::string robot_key_;
    std::string robot_token_;
    std::string model_config_;
    std::string network_url_;
};

#endif
