#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <string>

#include <esp_err.h>
#include "board.h"

enum VoiceState : int {
    VOICE_STOP = 0,
    VOICE_START = 1,
};

enum OtaUpgradeState : int {
    OTA_UPGRADE_START = 0,
    OTA_UPGRADE_SUCCESS = 1,
    OTA_UPGRADE_FAILED = 2,
    OTA_DOWNLOAD_START = 10,
    OTA_DOWNLOAD_SUCCESS = 11,
    OTA_DOWNLOAD_FAILED = 12,
};

class Ota {
public:
    Ota();
    ~Ota();

    esp_err_t CheckVersion();
    esp_err_t Activate();
    bool HasActivationChallenge() { return has_activation_challenge_; }
    bool HasNewVersion() { return has_new_version_; }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    bool HasActivationCode() { return has_activation_code_; }
    bool HasServerTime() { return has_server_time_; }
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    bool StartUpgradeFromUrl(const std::string& url, const std::string& expected_md5,
        std::function<void(int progress, size_t speed)> callback,
        std::function<void(OtaUpgradeState state, esp_err_t err, const std::string& desc)> state_callback = {});
    void MarkCurrentVersionValid();

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    void SetFirmwareUrl(std::string url) { firmware_url_ = std::move(url); }
    const std::string& GetFirmwareMd5() const { return firmware_md5_; }
    void SetFirmwareMd5(std::string md5) { firmware_md5_ = std::move(md5); }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    std::string GetCheckVersionUrl();

private:
    std::string activation_message_;
    std::string activation_code_;
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string firmware_md5_;
    std::string activation_challenge_;
    std::string serial_number_;
    int activation_timeout_ms_ = 30000;

    bool Upgrade(const std::string& firmware_url, const std::string& expected_md5 = {});
    bool UpgradeNew(const std::string& firmware_url, const std::string& expected_md5 = {});
    std::function<void(int progress, size_t speed)> upgrade_callback_;
    std::function<void(OtaUpgradeState state, esp_err_t err, const std::string& desc)> upgrade_state_callback_;
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
    std::string GetActivationPayload();
    std::unique_ptr<Http> SetupHttp();
};

#endif // _OTA_H
