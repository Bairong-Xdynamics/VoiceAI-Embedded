#include <esp_network.h>
#include <esp_mac.h>
#include "twilio_protocol.h"

#include <cstring>
#include <cinttypes>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <mbedtls/base64.h>
#include <memory>

#define TAG "TW"

static std::string GetMacAddress() {
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_STA, mac);
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

static std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (char c : value) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::setw(2)
                    << int((unsigned char)c);
        }
    }
    return escaped.str();
}

static std::string Base64Encode(const uint8_t* data, size_t len) {
    if (!data || len == 0) return "";

    // 计算输出缓冲区大小
    size_t out_len = 0;
    size_t buf_size = ((len + 2) / 3) * 4 + 1; // +1 防止末尾 '\0'
    std::vector<uint8_t> buf(buf_size);

    int ret = mbedtls_base64_encode(buf.data(), buf.size(), &out_len, data, len);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_base64_encode failed: %d", ret);
        return "";
    }

    return std::string(reinterpret_cast<char*>(buf.data()), out_len);
}

static std::vector<uint8_t> Base64Decode(const std::string& input)
{
    if (input.empty()) return {};
    
    size_t out_len = 0;
    std::vector<uint8_t> out(input.size());

    int ret = mbedtls_base64_decode(
        out.data(),
        out.size(),
        &out_len,
        (const unsigned char*)input.data(),
        input.size()
    );
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_base64_decode failed: %d", ret);
        return {};
    }

    out.resize(out_len);
    return out;
}

TwilioProtocol::TwilioProtocol(const std::string& robot_key, const std::string& robot_token, const std::string& model_config) {
    event_group_handle_ = xEventGroupCreate();  
    robot_key_ = robot_key;
    robot_token_ = robot_token;
    model_config_ = model_config;
}

TwilioProtocol::~TwilioProtocol() {
    //CloseAudioChannel();
    vEventGroupDelete(event_group_handle_);
}

void TwilioProtocol::UpdateConfig(const std::string& robot_key, const std::string& robot_token, const std::string& model_config) {
    robot_key_ = robot_key;
    robot_token_ = robot_token;
    model_config_ = model_config;
}
   
void TwilioProtocol::SetNetWorkUrl(const std::string& config_url) {
    network_url_ = config_url;
}

bool TwilioProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool TwilioProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!IsAudioChannelOpened()) {
        CloseAudioChannel();
        return false;
    }

    if (packet->payload.size() < 1024/2) {
        return true;
    }
    if (!SendTwilioMedia(packet->payload.data(), packet->payload.size())) {
        ESP_LOGE(TAG, "Failed to send Twilio media");
        return false;
    }
    return true;
}

bool TwilioProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    cJSON* root = cJSON_Parse(text.c_str());
    if (root == nullptr) {
        return false;
    }

    cJSON* event = cJSON_GetObjectItem(root, "event");
    if (!cJSON_IsString(event)) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_Delete(root);

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        return false;
    }

    return true;
}

bool TwilioProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ /*&& !IsTimeout()*/;
}

void TwilioProtocol::NotifyAudioChannelClosed() {
    if (!audio_channel_active_) {
        return;
    }
    audio_channel_active_ = false;
    first_outgoing_audio_logged_ = false;
    first_incoming_audio_logged_ = false;
    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

void TwilioProtocol::CloseAudioChannel() {
    websocket_.reset();
    esp_network_.reset();
    NotifyAudioChannelClosed();
}

bool TwilioProtocol::OpenAudioChannel() {
    if (network_url_.empty()) {
        ESP_LOGE(TAG, "Network URL is not set");
        SetError("网络地址未设置");
        return false;
    }

    audio_channel_active_ = false;

    std::string url = network_url_ + "?a=a&agentName=webcall_agent&robotKey=" + UrlEncode(robot_key_) + "&robotToken=" + UrlEncode(robot_token_) + "&userName=" + GetMacAddress().c_str();
    //std::string url = "ws://172.16.184.46:8765";
    std::string token = "";
    version_ = 0;
    error_occurred_ = false;
    first_outgoing_audio_logged_ = false;
    first_incoming_audio_logged_ = false;
    last_incoming_time_ = std::chrono::steady_clock::now();
    if (!first_outgoing_audio_logged_) {
        first_outgoing_audio_time_ = std::chrono::steady_clock::now();
        if (on_log_message_ != nullptr) {
            on_log_message_("info", "首次上行音频已发送", "语音协议", "设备首次向服务端发送音频数据");
        }
        first_outgoing_audio_logged_ = true;
    }

    esp_network_ = std::make_unique<EspNetwork>();
    websocket_ = esp_network_->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        if (on_log_message_ != nullptr) {
            on_log_message_("error", "创建WebSocket失败", "语音协议", "无法创建WebSocket连接实例");
        }
        esp_network_.reset();
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", GetMacAddress().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            ESP_LOGE(TAG, "Unsupported binary data");
            return;
        } else {
            // Parse JSON data
            auto root = cJSON_Parse(data);
            auto event = cJSON_GetObjectItem(root, "event");
            if (event != NULL) {
                ParseTwilioMedia(root);
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        NotifyAudioChannelClosed();
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        if (on_log_message_ != nullptr) {
            on_log_message_("error", "连接WebSocket服务器失败", "语音协议", "无法连接到语音协议服务端");
        }
        SetError("连接WebSocket服务器失败");
        return false;
    }

    // ==========================
    // Send start message (Twilio style)
    // ==========================
    std::string start_msg = "{";
    start_msg += "\"event\":\"start\",";
    start_msg += "\"sequenceNumber\":\"" + std::to_string(sequence_++) + "\",";
    start_msg += "\"start\":{";
    start_msg += "\"accountSid\":\"" + GetMacAddress() + "\",";
    start_msg += "\"streamSid\":\"" + GetMacAddress() + "\",";
    start_msg += "\"callSid\":\"" + GetMacAddress() + "\",";
    start_msg += "\"tracks\":[\"inbound\"],";
    start_msg += "\"mediaFormat\":{";
    start_msg += "\"encoding\":\"audio/pcm16\",";
    start_msg += "\"sampleRate\":" + std::to_string(16000) + ",";
    start_msg += "\"channels\":1";
    start_msg += "}";
    start_msg += ",";
    start_msg += "\"config\": " + model_config_;
    start_msg += "}";
    start_msg += "}";

    ESP_LOGI(TAG, "🔊 sent event start msg : %s", start_msg.c_str());
    if (!SendText(start_msg)) {
        ESP_LOGE(TAG, "Failed to send start message");
        return false;
    }

    audio_channel_active_ = true;
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    if (on_log_message_ != nullptr) {
        on_log_message_("info", "WebSocket连接成功", "语音协议", "语音通道建立完成");
    }

    return true;
}

bool TwilioProtocol::SendTwilioMedia(const uint8_t* pcm, size_t len)
{
        /*
    // 打印前 16 字节，避免日志过长
    size_t print_len = len < 16 ? len : 16;
    char buf[3 * 16 + 1] = {0}; // 每个字节 2 位 + 空格
    for (size_t i = 0; i < print_len; ++i) {
        snprintf(buf + i * 3, 4, "%02X ", pcm[i]);
    }
    ESP_LOGI(TAG, "🔊 sent %d bytes audio, payload (first %d bytes): %s", (int)len, print_len, buf);
    */
    
    std::string payload = Base64Encode(pcm, len);

    std::string msg = "{";
    msg += "\"event\":\"media\",";
    msg += "\"sequenceNumber\":\"" + std::to_string(sequence_) + "\",";
    msg += "\"media\":{";
    msg += "\"track\":\"inbound\",";
    msg += "\"chunk\":\"" + std::to_string(chunk_) + "\",";
    msg += "\"timestamp\":\"" + std::to_string(chunk_ * kPcmSendFrameMs) + "\",";
    msg += "\"payload\":\"" + payload + "\"";
    msg += "},";
    msg += "\"streamSid\":\"" + GetMacAddress() + "\"";
    msg += "}";

    sequence_++;
    chunk_++;

    return SendText(msg);
    //const bool ok = SendText(msg);
    //if (ok && !first_outgoing_audio_logged_) {
    //    first_outgoing_audio_time_ = std::chrono::steady_clock::now();
    //    if (on_log_message_ != nullptr) {
    //        on_log_message_("info", "首次上行音频已发送", "语音协议", "设备首次向服务端发送音频数据");
    //    }
    //    first_outgoing_audio_logged_ = true;
    //}
    //return ok;
}

bool TwilioProtocol::SendTwilioStop()
{
    if (websocket_ == nullptr || !websocket_->IsConnected())
        return false;

    std::string msg = "{";
    msg += "\"event\":\"stop\",";
    msg += "\"sequenceNumber\":\"" + std::to_string(sequence_) + "\",";
    msg += "\"stop\":{";
    msg += "\"accountSid\":\"" + GetMacAddress() + "\",";
    msg += "\"callSid\":\"" + GetMacAddress() + "\"";
    msg += "},";
    msg += "\"streamSid\":\"" + GetMacAddress() + "\"";
    msg += "}";

    sequence_++;

    ESP_LOGI(TAG, "🔊 sent stop message");
    return SendText(msg);
}

void TwilioProtocol::ParseTwilioMedia(const cJSON* root)
{
    /*
    char* printed = cJSON_PrintUnformatted(root);
    if (printed) {
        ESP_LOGI(TAG, "🔊 received json: %s", printed);
        cJSON_free(printed);
    }
    */

    auto media = cJSON_GetObjectItem(root, "media");
    if (media == nullptr) {
        media = cJSON_GetObjectItem(root, "message");
        if(media && on_incoming_json_ != nullptr) {
            //ESP_LOGI(TAG, "🔊 received json: %s", cJSON_PrintUnformatted(media));
            on_incoming_json_(media);
        }
        return;
    }

    auto payload = cJSON_GetObjectItem(media, "payload");
    if (payload == nullptr || payload->valuestring == nullptr)
        return;

    std::string encoded(payload->valuestring);
    //ESP_LOGI(TAG, "🔊 received payload: %s, encoded: %s", payload->valuestring, encoded.c_str());
    auto pcm = Base64Decode(encoded);
    
    /*
    // 打印前 16 字节，避免日志过长
    size_t print_len = pcm.size() < 16 ? pcm.size() : 16;
    char buf[3 * 16 + 1] = {0}; // 每个字节 2 位 + 空格
    for (size_t i = 0; i < print_len; ++i) {
        snprintf(buf + i * 3, 4, "%02X ", pcm[i]);
    }
    ESP_LOGI(TAG, "🔊 received %d bytes audio, payload (first %d bytes): %s", (int)pcm.size(), print_len, buf);
    */
    
    if (!pcm.empty())
    {
        if (on_incoming_audio_ != nullptr)
        {
            if (!first_incoming_audio_logged_) {
                if (on_log_message_ != nullptr) {
                    auto now = std::chrono::steady_clock::now();
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - first_outgoing_audio_time_).count();
                    on_log_message_("info", "首次下行音频已接收", "语音协议",
                        "上行到下行耗时: " + std::to_string(ms) + "ms");
                }
                first_incoming_audio_logged_ = true;
            }
            //ESP_LOGI(TAG, "🔊 received %d bytes audio, payload size: %d, encoded: %s", encoded.size(), pcm.size(), encoded.c_str());
            on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                .sample_rate = 16000,
                .frame_duration = kPcmSendFrameMs,
                .timestamp = static_cast<uint32_t>(chunk_ * kPcmSendFrameMs),
                .payload = std::move(pcm)
            }));
        }

    }
}
