#include "application.h"
#include "config.h"
#include "esp_log_level.h"
#include "tls13_test.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "twilio_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include <cmath>
#if CONFIG_USE_WS_DEBUG_SINK
#include "debug/ws_log_pcm_sink.h"
#endif

#include <cstring>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <sys/time.h>

#define TAG "Application"

namespace {
    constexpr int kVadSilenceCloseChannelSeconds = 30;
}

static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

namespace {
    constexpr const char* kAppConfigNamespace = "app_cfg";
    constexpr const char* kAppConfigKeyUserId = "user_id";
    constexpr const char* kAppConfigKeyToken = "token";
    constexpr const char* kAppConfigKeyUserToken = "user_token";
    constexpr const char* kAppConfigKeyTimestamp = "timestamp";
    constexpr const char* kAppConfigKeyActivateState = "activate_state";

    esp_err_t LoadAppConfig(AppNvsConfig& config) {
        config = {};

        Settings settings(kAppConfigNamespace, false);
        config.user_id = settings.GetString(kAppConfigKeyUserId);
        config.token = settings.GetString(kAppConfigKeyToken);
        config.user_token = settings.GetString(kAppConfigKeyUserToken);
        config.timestamp = settings.GetInt64(kAppConfigKeyTimestamp, 0);
        config.activate_state = settings.GetBool(kAppConfigKeyActivateState, false);
        return ESP_OK;
    }

    esp_err_t SaveAppConfig(const AppNvsConfig& config) {
        Settings settings(kAppConfigNamespace, true);
        settings.SetString(kAppConfigKeyUserId, config.user_id);
        settings.SetString(kAppConfigKeyToken, config.token);
        settings.SetString(kAppConfigKeyUserToken, config.user_token);
        settings.SetInt64(kAppConfigKeyTimestamp, config.timestamp);
        settings.SetBool(kAppConfigKeyActivateState, config.activate_state);
        return ESP_OK;
    }
}  // namespace

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveMode(false);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        board.SetPowerSaveMode(true);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota.CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota.GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            if (UpgradeFirmware(ota)) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation (don't break, just fall through)
        }

        // No new version, mark current version as valid and skip activation flow.
        ota.MarkCurrentVersionValid();
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        break;
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {
        Schedule([this]() {
            protocol_->CloseAudioChannel();
        });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
/*
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display 
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service 
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Start the main event loop task with priority 3
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);

    // Start the clock timer to update the status bar 
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Wait for the network to be ready 
    board.StartNetwork();

#if CONFIG_USE_WS_DEBUG_SINK
    WsLogPcmSink::Instance().ConfigureFromNvs();
    WsLogPcmSink::Instance().StartIfEnabled();
#endif

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    // = McpServer::GetInstance();
    //mcp_server.AddCommonTools();
    //mcp_server.AddUserOnlyTools();

    protocol_ = std::make_unique<TwilioProtocol>(CONFIG_CONNECTION_CONFIG_KEY, CONFIG_CONNECTION_CONFIG_TOKEN);
    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        audio_service_.PushPacketToPcmQueue(std::move(packet));
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
    });
    bool protocol_started = protocol_->Start();

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    }
*/

    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display 
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service 
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        //ESP_LOGI(TAG, "VAD change: %d", speaking);
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Start the main event loop task with priority 3
    xTaskCreate([](void* arg) {
        ((Application*)arg)->MainEventLoop();
        vTaskDelete(NULL);
    }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);

    // Start the clock timer to update the status bar 
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Wait for the network to be ready 
    board.StartNetwork();

#if CONFIG_RUN_TLS13_TEST_AT_BOOT
    if (!RunTls13Test()) {
        ESP_LOGW(TAG, "TLS 1.3 self-test failed");
    }
    if (!RunHttpTest()) {
        ESP_LOGW(TAG, "HTTP self-test failed");
    }
#endif

#if CONFIG_USE_WS_DEBUG_SINK
    WsLogPcmSink::Instance().ConfigureFromNvs();
    WsLogPcmSink::Instance().StartIfEnabled();
#endif

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new assets version
    CheckAssetsVersion();

    //ota_.SetFirmwareUrl("http://172.16.184.46:8080/firmware/app.bin");
    //ota_.SetFirmwareUrl("https://baiyu-pre.100credit.cn/api/hardware/firmware/app.bin");
    //ota_.SetFirmwareMd5("d41d8cd98f00b204e9800998ecf8427e");
    //UpgradeFirmware(ota_);
    //return;
        

    //std::string user_id = "3";
    //std::string user_token = "mp_v1.eyJ1c2VyX2lkIjozLCJvcGVuaWQiOiJvVWJNMzNlQ0F4WVBKWmI5MW1fcVJSelhTUjFBIiwiaWF0IjoxNzc5MDcxMTUxLCJleHAiOjE3NzkxNTc1NTEsInR5cCI6ImFjY2VzcyJ9.486fdd10146a34c23179237ea020e1527a325c5b30a9382d68366aa8b6f80a25";
    //ActivateParameter("{\"user_id\":\"" + user_id + "\",\"user_token\":\"" + user_token + "\"}");

    // Activate the main
    AppNvsConfig cfg;
    esp_err_t load_cfg_err = LoadAppConfig(cfg);
    if (load_cfg_err != ESP_OK) {
        ESP_LOGE(TAG, "Load app config failed: %s", esp_err_to_name(load_cfg_err));
    }
    ESP_LOGI(TAG,
             "Load app config: user_id=%s user_token=%s token=%s timestamp=%s activate_state=%d",
             cfg.user_id.c_str(), cfg.user_token.c_str(), cfg.token.c_str(),
             std::to_string(cfg.timestamp).c_str(), static_cast<int>(cfg.activate_state));
   
    while (!ActivateMain(cfg)) {
        ESP_LOGE(TAG, "ActivateMain failed");
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        vTaskDelay(pdMS_TO_TICKS(1000 * 10));
    }

    ota_.MarkCurrentVersionValid();
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);

    //mqtt client
    mqtt_client_.OnConnected([this]() {
        ESP_LOGI(TAG, "MQTT connected");
        SendLogMessage("info", "MQTT连接成功", "连接", "MQTT服务连接建立,开始获取设备配置");
        mqtt_client_.SendGetConfig();
    });
    mqtt_client_.OnDisconnected([this]() {
        ESP_LOGI(TAG, "MQTT disconnected");
    });
    mqtt_client_.OnOtaUpdateOffer([this](const std::string& request_id, const std::string& code, const MqttOtaUpdateOffer& offer) {
        ESP_LOGI(TAG, "MQTT ota update offer: %s", code.c_str());
        // OTA/HTTP must not run on mqtt_task (default ~6KB stack); defer to main event loop.
        Schedule([this, code, offer]() {
            SendLogMessage("info", "收到OTA升级请求", "升级", "固件地址: " + offer.firmware_url + " MD5: " + offer.md5 + " 状态码: " + code);
            //ota_.SetFirmwareUrl("http://172.16.184.46:8080/firmware/firmware.bin");
            //ota_.SetFirmwareMd5("d41d8cd98f00b204e9800998ecf8427e");
            ota_.SetFirmwareUrl(offer.firmware_url);
            ota_.SetFirmwareMd5(offer.md5);
            UpgradeFirmware(ota_);
        });
    });
    mqtt_client_.OnSetVolume([this, codec](const std::string& request_id, const std::string& code, int volume) {
        ESP_LOGI(TAG, "MQTT set volume: %d", volume);
        if (code != "000000") {
            ESP_LOGE(TAG, "Failed to set volume: %s", code.c_str());
            SendLogMessage("error", "设置音量失败", "音量", "服务端返回错误码: " + code);
            return;
        }
        if (volume >= 0 && volume <= 100) {
            codec->SetOutputVolume(volume);
            ESP_LOGI(TAG, "Set output volume to %d", volume);
            if (mqtt_client_.IsConnected()) {
                mqtt_client_.SendSetVolume(volume, request_id);
            }
            SendLogMessage("info", "音量设置成功", "音量", "当前音量: " + std::to_string(volume));
        }
    });
    mqtt_client_.OnPushUnbind([this, &board](const std::string& request_id, const std::string& device_id) {
        ESP_LOGI(TAG, "MQTT push_unbind: request_id=%s device_id=%s", request_id.c_str(), device_id.c_str());
        Schedule([this, &board, request_id, device_id]() {
            const std::string local_device_id = SystemInfo::GetMacAddress();
            if (!device_id.empty() && device_id != local_device_id) {
                ESP_LOGW(TAG, "push_unbind ignored: device_id mismatch (%s vs %s)", device_id.c_str(),
                         local_device_id.c_str());
                return;
            }
            SendLogMessage("info", "收到解绑指令", "解绑", "清除设备绑定信息并重启");
            AppNvsConfig cfg;
            LoadAppConfig(cfg);
            cfg.user_id.clear();
            cfg.user_token.clear();
            cfg.token.clear();
            cfg.timestamp = 0;
            cfg.activate_state = false;
            if (SaveAppConfig(cfg) != ESP_OK) {
                ESP_LOGE(TAG, "push_unbind: failed to save cleared app config");
            }
            if (protocol_) {
                if (protocol_->IsAudioChannelOpened()) {
                    protocol_->CloseAudioChannel();
                }
                protocol_.reset();
            }
            if (mqtt_client_.IsConnected()) {
                mqtt_client_.SendPushUnbind(request_id);
            }
            mqtt_client_.Disconnect();
            Reboot();
        });
    });
    mqtt_client_.OnSetWakeWord([this](const std::string& request_id, const std::string& code,
                                    const std::string& wake_word, const std::string& wake_name) {
        ESP_LOGI(TAG, "MQTT set wake word: request_id=%s code=%s wake_word=%s wake_name=%s", request_id.c_str(),
                code.c_str(), wake_word.c_str(), wake_name.c_str());
        if (code != "000000") {
            ESP_LOGE(TAG, "Failed to set wake word: %s", code.c_str());
            SendLogMessage("error", "设置唤醒词失败", "唤醒词", "服务端返回错误码: " + code);
            return;
        }
        SendLogMessage("info", "唤醒词设置成功", "唤醒词", "唤醒词: " + wake_word + " 唤醒名: " + wake_name);
        if (audio_service_.ConfigureCustomWakeWord(wake_word, wake_name)) {
            ESP_LOGI(TAG, "Wake word updated by MQTT set_wake_word: wake_word=%s wake_name=%s", wake_word.c_str(),
                    wake_name.c_str());
        } else {
            ESP_LOGW(TAG, "Failed to update wake word by MQTT set_wake_word (custom wake word not available?)");
        }
    });
    mqtt_client_.OnGetConfigResponse([this, codec, &board, display](const std::string& request_id, const std::string& code, const MqttConfigData& config) {
        ESP_LOGI(TAG, "MQTT get config response: %s", code.c_str());
        if (code != "000000") {
            ESP_LOGW(TAG, "Failed to get config: %s", code.c_str());
            SendLogMessage("error", "获取设备配置失败", "配置", "服务端返回错误码: " + code);
            return;
        }
        if (config.robot_key.empty() || config.robot_token.empty()) {
            ESP_LOGW(TAG, "Failed to get config: robot_key or robot_token is empty");
            SendLogMessage("error", "获取设备配置失败", "配置", "robot_key 或 robot_token 为空");
            return;
        }

        //MqttConfigData config;
        //config.robot_key = "dZSJQ08Y7AYrL7lfPe2%2BBxelWH8%3D";
        //config.robot_token = "MTc3NjI0MTczNjc5OApKeHJ1cHFpTC9kWlR3aHF2WE9uOXRIUnEzdGM9";
        //config.wake_word = "xiao rong";
        //config.wake_name = "小荣";
        //config.volume = 80;
        //config.model_config = "{\"tts\":{\"voice\":\"zh-CN-XiaoxiaoNeural\"}}";

        ESP_LOGI(TAG, "Set config: robot_key=%s robot_token=%s wake_word=%s wake_name=%s volume=%d model_config=%s", 
            config.robot_key.c_str(), 
            config.robot_token.c_str(), 
            config.wake_word.c_str(), 
            config.wake_name.c_str(), 
            config.volume,
            config.model_config.c_str());
        SendLogMessage("info", "设备配置更新成功", "配置", "唤醒词: " + config.wake_word + " 唤醒名: " + config.wake_name + " 音量: " + std::to_string(config.volume));

        if (audio_service_.ConfigureCustomWakeWord(config.wake_word, config.wake_name)) {
            ESP_LOGI(TAG, "Wake word updated by MQTT config: wake_word=%s wake_name=%s", config.wake_word.c_str(), config.wake_name.c_str());
        } else {
            ESP_LOGW(TAG, "Failed to update wake word by MQTT config");
        }

        // Set the output volume
        codec->SetOutputVolume(config.volume);
       
        //const std::string model_config = "{\"tts\":{\"voice\":\"zh-CN-XiaoxiaoNeural\"}}";
        //protocol_ = std::make_unique<TwilioProtocol>("dZSJQ08Y7AYrL7lfPe2%2BBxelWH8%3D", "MTc3NjI0MTczNjc5OApKeHJ1cHFpTC9kWlR3aHF2WE9uOXRIUnEzdGM9", config.model_config);
        protocol_ = std::make_unique<TwilioProtocol>(config.robot_key, config.robot_token, config.model_config);
        const std::string& ws_voice_url = server_device_config_.ok ? server_device_config_.ws_voice_url
                                                             : kTwilioWebSocketUrl;
        protocol_->SetNetWorkUrl(ws_voice_url);
        protocol_->OnConnected([this]() {
            DismissAlert();
        });

        protocol_->OnNetworkError([this](const std::string& message) {
            last_error_message_ = message;
            xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        });
        protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
            audio_service_.PushPacketToPcmQueue(std::move(packet));
        });
        protocol_->OnAudioChannelOpened([this, codec, &board]() {
            ESP_LOGI(TAG, "Audio channel opened");
            SendLogMessage("info", "语音通道已开启", "语音协议", "音频通道建立,开始语音交互");
            if (mqtt_client_.IsConnected()) {
                mqtt_client_.SendVoiceState(VoiceState::VOICE_START);
            }
            board.SetPowerSaveMode(false);
            if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
                ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                    protocol_->server_sample_rate(), codec->output_sample_rate());
            }
        });
        protocol_->OnAudioChannelClosed([this, &board]() {
            ESP_LOGI(TAG, "Audio channel closed");
            SendLogMessage("info", "语音通道已关闭", "语音协议", "音频通道断开,结束语音交互");
            board.SetPowerSaveMode(true);
            Schedule([this]() {
                if (mqtt_client_.IsConnected()) {
                    mqtt_client_.SendVoiceState(VoiceState::VOICE_STOP);
                }
                auto display = Board::GetInstance().GetDisplay();
                display->SetChatMessage("system", "");
                SetDeviceState(kDeviceStateIdle);
            });
        });
        protocol_->OnIncomingJson([this, display](const cJSON* root) {
        });
        protocol_->OnLogMessage([this](const std::string& level, const std::string& message,
                                       const std::string& event, const std::string& desc) {
            ESP_LOGI(TAG, "MQTT log: %s %s %s %s", level.c_str(), message.c_str(), event.c_str(), desc.c_str());
            SendLogMessage(level, message, event, desc);
        });
        bool protocol_started = protocol_->Start();

        SystemInfo::PrintHeapStats();
        SetDeviceState(kDeviceStateIdle);

        if (protocol_started) {
            std::string message = std::string(Lang::Strings::VERSION) + SystemInfo::GetVersion();
            display->ShowNotification(message.c_str());
            display->SetChatMessage("system", "");
            // Play the success sound to indicate the device is ready
            audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
            SendLogMessage("info", "语音协议连接成功", "语音协议", "Twilio语音协议启动完成,设备就绪");
        }
    });
    const std::string& mqtt_endpoint = server_device_config_.ok ? server_device_config_.mqtt_url
                                                                : kMqttBrokerEndpoint;
    mqtt_client_.ConnectFromSettings(mqtt_endpoint, cfg.token);
}

void Application::SendLogMessage(const std::string& level, const std::string& message,
                                  const std::string& event, const std::string& desc) {
    if (mqtt_client_.IsConnected()) {
        time_t now = time(nullptr);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
        ESP_LOGI(TAG, "Send log message: %s %s %s", time_str, level.c_str(), message.c_str());
        mqtt_client_.SendDeviceLog(time_str, level, message, event, desc);
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (audio_service_.IsVoiceDetected()) {
                vad_silence_seconds_ = 0;
            }
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            if (device_state_ == kDeviceStateListening && protocol_ &&
                protocol_->IsAudioChannelOpened() && !audio_service_.IsVoiceDetected()) {
                vad_silence_seconds_++;
                if (vad_silence_seconds_ >= kVadSilenceCloseChannelSeconds) {
                    vad_silence_seconds_ = 0;
                    ESP_LOGI(TAG, "VAD silent for %ds, closing audio channel",
                        kVadSilenceCloseChannelSeconds);
                    Schedule([this]() {
                        if (protocol_ && protocol_->IsAudioChannelOpened()) {
                            protocol_->CloseAudioChannel();
                        }
                    });
                }
            } else {
                vad_silence_seconds_ = 0;
            }

            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }
            if (clock_ticks_ % 30 == 0) {
                if (mqtt_client_.IsConnected()) {
                    mqtt_client_.SendHeartBeat();
                }
            }
            if (clock_ticks_ % (60 * 10) == 0) {
                Schedule([this]() {
                    AppNvsConfig token_cfg;
                    if (LoadAppConfig(token_cfg) != ESP_OK || token_cfg.user_id.empty() ||
                        token_cfg.user_token.empty() || token_cfg.token.empty()) {
                        ESP_LOGW(TAG, "Periodic GetToken skipped: no activation credentials");
                        return;
                    }
                    if (!has_server_time_) {
                        ESP_LOGW(TAG, "Periodic GetToken skipped: server time not synchronized");
                        return;
                    }
    
                    auto& board = Board::GetInstance();
                    auto* network = board.GetNetwork();
                    if (network == nullptr) {
                        return;
                    }
                    auto http = network->CreateHttp(0);
                    if (!http) {
                        return;
                    }
                    const std::string url_get_token =
                        activate_api::JoinEndpoint(kActivateApiBase, "/get_token");
                    const std::string device_id = SystemInfo::GetMacAddress();
                    const std::string app_version = SystemInfo::GetVersion();
                    if (!GetToken(http.get(), url_get_token, app_version, device_id, token_cfg)) {
                        ESP_LOGW(TAG, "Periodic GetToken check failed");
                    }
                    ESP_LOGI(TAG, "Periodic GetToken check success");
                });
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        //audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            vad_silence_seconds_ = 0;

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& ota, const std::string& url, const std::string& firmware_md5) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    
    // Use provided URL or get from OTA object
    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string upgrade_md5 = firmware_md5;
    if (upgrade_md5.empty() && url.empty()) {
        upgrade_md5 = ota.GetFirmwareMd5();
    }
    std::string version_info = url.empty() ? ota.GetFirmwareVersion() : "(Manual upgrade)";
    
    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());
    
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);
    
    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveMode(false);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = ota.StartUpgradeFromUrl(
        upgrade_url,
        upgrade_md5,
        [display](int progress, size_t speed) {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        },
        [this](OtaUpgradeState state, esp_err_t err, const std::string& desc) {
                ESP_LOGI(TAG, "Firmware upgrade state: %d, error: %s, desc: %s", state, esp_err_to_name(err), desc.c_str());
                if (mqtt_client_.IsConnected()) {
                    mqtt_client_.SendOtaUpdateState(static_cast<int>(state), desc);
                }
                SendLogMessage("info", "固件升级状态变更", "升级", desc.empty() ? "状态: " + std::to_string(state) + " 原因: " + esp_err_to_name(err) : desc);
            }
        );

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveMode(true); // Restore power save mode

        // Show error alert first
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));

        // Restore device state to Idle so wake word detection is re-enabled
        // (kDeviceStateIdle branch in SetDeviceState calls EnableWakeWordDetection(true))
        Schedule([this]() {
            // If a voice session somehow got opened during the failure window, close it first
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->CloseAudioChannel();
            }
            SetDeviceState(kDeviceStateIdle);
        });
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    if (protocol_ == nullptr) {
        return;
    }

    // Make sure you are using main thread to send MCP message
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        protocol_->SendMcpMessage(payload);
    } else {
        Schedule([this, payload = std::move(payload)]() {
            protocol_->SendMcpMessage(payload);
        });
    }
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

bool Application::StartNetwork(const std::string& ssid, const std::string& password) {
    auto& board = Board::GetInstance();
    //std::string user_id = "2";
    //std::string user_token = "mp_v1.eyJ1c2VyX2lkIjoyLCJvcGVuaWQiOiJvWU9RMzNiYjdQTlR6N3hiTEplVEpnUF94WmJrIiwiaWF0IjoxNzc4NjUyNTcxLCJleHAiOjE3Nzg3Mzg5NzEsInR5cCI6ImFjY2VzcyJ9.ee6066b0d2f7c1bffdccb223816ad71c2cef38eab09020c729263ee4d92cf804";
    //ESP_LOGI(TAG, "StartNetwork: user_id=%s, user_token=%s", user_id.c_str(), user_token.c_str());
    //ActivateParameter("{\"user_id\":\"" + user_id + "\",\"user_token\":\"" + user_token + "\"}");
    //return board.StartNetwork("鹏涛's Galaxy S10+", "sagg9141");
    return board.StartNetwork(ssid, password);
}

bool Application::ActivateMain(AppNvsConfig& cfg) {
    const std::string url_set = activate_api::JoinEndpoint(kActivateApiBase, "/set_activate");
    const std::string url_get_state = activate_api::JoinEndpoint(kActivateApiBase, "/get_activate");
    const std::string url_server_time = activate_api::JoinEndpoint(kActivateApiBase, "/server_time");
    const std::string url_get_token = activate_api::JoinEndpoint(kActivateApiBase, "/get_token");
    const std::string url_get_config = activate_api::JoinEndpoint(kActivateApiBase, "/get_link_config");

    //cfg.user_id = "user_id";
    //cfg.user_token = "user_token";
    //cfg.token = "1715404800000";
    //cfg.timestamp = 1715404800000;
    //cfg.activate_state = false;
    //cfg.activate_state = true;

    auto& board = Board::GetInstance();
    if (cfg.user_id.empty() || cfg.user_token.empty()) {
        ESP_LOGE(TAG, "ActivateMain: skip (empty user_id or user_token)");
        last_error_message_ = "用户凭证为空";
        board.ResetWifiConfiguration(); 
        return false;
    }

    auto http = board.GetNetwork()->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "ActivateMain: CreateHttp failed");
        last_error_message_ = "创建网络请求失败";
        return false;
    }

    const std::string device_id = SystemInfo::GetMacAddress();
    const std::string app_version = SystemInfo::GetVersion();
    bool server_activated = false;

    if (cfg.activate_state) {
        if (!SetActivate(http.get(), url_set, app_version, device_id, cfg)) {
            return false;
        }
        server_activated = true;
    } else {
        server_activated = GetActivate(http.get(), url_get_state, app_version, device_id, cfg);
        if (server_activated && cfg.activate_state) {
            last_error_message_ = "设备未激活";
            board.ResetWifiConfiguration(); 
            return false;
        }
    }

    if (!server_activated) {
        ESP_LOGE(TAG, "ActivateMain: skip ServerTime/GetToken (not activated)");
        last_error_message_ = "设备未激活";
        return false;
    }

    if (!ServerTime(http.get(), url_server_time, app_version, device_id, cfg)) {
        return false;
    }

    if (!GetToken(http.get(), url_get_token, app_version, device_id, cfg)) {
        return false;
    }

    if (!GetLinkConfig(http.get(), url_get_config, app_version, device_id, cfg)) {
        return false;
    }

    return true;
}

bool Application::SetActivate(Http* http, const std::string& endpoint_url, const std::string& app_version,
                              const std::string& device_id, AppNvsConfig& cfg) {
    activate_api::TokenPayload out{};
    esp_err_t err =
        activate_api::SetActivate(http, endpoint_url, app_version, device_id, cfg.user_id, cfg.user_token, &out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ActivateMain: SetActivate failed: %s", esp_err_to_name(err));
        last_error_message_ = "设备激活失败";
        return false;
    }
    if (!out.ok) {
        ESP_LOGE(TAG, "ActivateMain: SetActivate invalid response");
        last_error_message_ = "设备激活响应无效";
        return false;
    }

    ESP_LOGI(TAG, "ActivateMain: SetActivate ok body=%s", out.raw_body.c_str());
    cfg.token = out.token;
    cfg.timestamp = out.timestamp_ms;
    cfg.activate_state = false;
    esp_err_t save_err = SaveAppConfig(cfg);
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "ActivateMain: settings save after SetActivate failed: %s", esp_err_to_name(save_err));
        return false;
    }

    ESP_LOGI(TAG, "ActivateMain: SetActivate ok");
    return true;
}

bool Application::GetActivate(Http* http, const std::string& endpoint_url, const std::string& app_version,
                              const std::string& device_id, AppNvsConfig& cfg) {
    activate_api::ActivateStatePayload out{};
    esp_err_t err =
        activate_api::GetActivate(http, endpoint_url, app_version, device_id, cfg.user_id, cfg.token, &out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ActivateMain: GetActivate failed: %s", esp_err_to_name(err));
        last_error_message_ = "查询激活状态失败";
        return false;
    }
    if (!out.ok) {
        ESP_LOGE(TAG, "ActivateMain: GetActivate invalid response");
        last_error_message_ = "查询激活状态响应无效";
        return false;
    }

    ESP_LOGI(TAG, "ActivateMain: GetActivate ok body=%s", out.raw_body.c_str());
    if (out.state == activate_api::ActivationBindState::kActivated) {
        cfg.activate_state = false;
        ESP_LOGI(TAG, "ActivateMain: GetActivate ok (activated)");
        return true;
    }

    ESP_LOGI(TAG, "ActivateMain: GetActivate ok (not activated)");
    cfg = {};
    cfg.activate_state = true;
    esp_err_t save_err = SaveAppConfig(cfg);
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "ActivateMain: settings save after GetActivate failed: %s", esp_err_to_name(save_err));
        return false;
    }

    ESP_LOGI(TAG, "ActivateMain: GetActivate ok (not activated), activate_state saved");
    return true;
}

bool Application::ServerTime(Http* http, const std::string& endpoint_url, const std::string& app_version,
                             const std::string& device_id, AppNvsConfig& cfg) {
    activate_api::ServerTimePayload st{};
    esp_err_t err_st =
        activate_api::ServerTime(http, endpoint_url, app_version, device_id, cfg.user_id, cfg.token, &st);
    if (err_st != ESP_OK) {
        ESP_LOGE(TAG, "ActivateMain: ServerTime failed: %s", esp_err_to_name(err_st));
        last_error_message_ = "获取服务器时间失败";
        return false;
    }
    if (!st.ok) {
        ESP_LOGE(TAG, "ActivateMain: ServerTime invalid response");
        last_error_message_ = "获取服务器时间响应无效";
        return false;
    }

    ESP_LOGI(TAG, "ActivateMain: ServerTime ok body=%s", st.raw_body.c_str());

    {
        struct timeval tv {};
        double ts = static_cast<double>(st.timestamp_ms);
        ts += static_cast<double>(st.timezone_offset_minutes) * 60.0 * 1000.0;
        tv.tv_sec = static_cast<time_t>(ts / 1000.0);
        tv.tv_usec = static_cast<suseconds_t>((static_cast<long long>(ts) % 1000) * 1000);
        if (settimeofday(&tv, nullptr) == 0) {
            has_server_time_ = true;
        } else {
            ESP_LOGE(TAG, "ActivateMain: settimeofday failed");
            return false;
        }
    }

    return true;
}

bool Application::GetToken(Http* http, const std::string& endpoint_url, const std::string& app_version,
                           const std::string& device_id, AppNvsConfig& cfg) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    //当前时间-8小时，utc时间戳，用于判断token是否过期
    //token过期时间，utc时间戳-1小时，用于提前1小时获取新的token
    const int64_t server_now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 - 8 * 60 * 60 * 1000;
    ESP_LOGI(TAG, "ActivateMain: server_now_ms=%s, cfg.timestamp=%ls", std::to_string(server_now_ms).c_str(), std::to_string(cfg.timestamp).c_str());
    const bool token_expired = (cfg.timestamp <= 0) || (server_now_ms >= (cfg.timestamp - 1000 * 60 * 60));
    if (token_expired) 
    {
        activate_api::TokenPayload gt{};
        esp_err_t err_gt =
            activate_api::GetToken(http, endpoint_url, app_version, device_id, cfg.user_id, cfg.token, &gt);
        if (err_gt != ESP_OK) {
            ESP_LOGE(TAG, "ActivateMain: GetToken failed: %s", esp_err_to_name(err_gt));
            last_error_message_ = "获取令牌失败";
            return false;
        }
        if (!gt.ok) {
            ESP_LOGE(TAG, "ActivateMain: GetToken invalid response");
            last_error_message_ = "获取令牌响应无效";
            return false;
        }
        ESP_LOGI(TAG, "ActivateMain: GetToken ok body=%s", gt.raw_body.c_str());
        cfg.token = gt.token;
        cfg.timestamp = gt.timestamp_ms;
    }

    cfg.activate_state = false;
    esp_err_t save_gt = SaveAppConfig(cfg);
    if (save_gt != ESP_OK) {
        ESP_LOGE(TAG, "ActivateMain: settings save after GetToken failed: %s", esp_err_to_name(save_gt));
        return false;
    }
    ESP_LOGI(TAG, "ActivateMain: GetToken ok, token refreshed");
    return true;
}

bool Application::GetLinkConfig(Http* http, const std::string& endpoint_url, const std::string& app_version,
                            const std::string& device_id, AppNvsConfig& cfg) {
    activate_api::DeviceConfigPayload out{};
    esp_err_t err =
        activate_api::GetLinkConfig(http, endpoint_url, app_version, device_id, cfg.user_id, cfg.token, &out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ActivateMain: GetConfig failed: %s", esp_err_to_name(err));
        last_error_message_ = "获取设备配置失败";
        return false;
    }
    if (!out.ok) {
        ESP_LOGE(TAG, "ActivateMain: GetConfig invalid response");
        last_error_message_ = "获取设备配置响应无效";
        return false;
    }

    ESP_LOGI(TAG,
             "ActivateMain: GetConfig ok mqtt_url=%s ws_voice_url=%s rtc_voice_url=%s body=%s",
             out.mqtt_url.c_str(), out.ws_voice_url.c_str(), out.rtc_voice_url.c_str(), out.raw_body.c_str());
    server_device_config_ = std::move(out);
    return true;
}

void Application::ActivateParameter(const std::string& param) {
    cJSON* root = cJSON_Parse(param.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse activate parameter");
        return;
    }
    cJSON* user_id = cJSON_GetObjectItem(root, "user_id");
    if (user_id == nullptr) {
        ESP_LOGE(TAG, "Failed to get user id");
        cJSON_Delete(root);
        return;
    }
    cJSON* user_token = cJSON_GetObjectItem(root, "user_token");
    if (user_token == nullptr) {
        ESP_LOGE(TAG, "Failed to get user token");
        cJSON_Delete(root);
        return;
    }
    std::string user_id_str;
    if (cJSON_IsString(user_id) && user_id->valuestring) {
        user_id_str = user_id->valuestring;
    } else if (cJSON_IsNumber(user_id)) {
        user_id_str = std::to_string((int)user_id->valuedouble); 
    } else {
        user_id_str = "";
    }
    Settings settings(kAppConfigNamespace, true);
    std::string stored_user_id = settings.GetString(kAppConfigKeyUserId);
    if (user_id_str != stored_user_id) {
        settings.SetBool(kAppConfigKeyActivateState, true);
        ESP_LOGI(TAG, "user_id changed, activate_state saved");
    }

    settings.SetString(kAppConfigKeyUserId, user_id_str);
    ESP_LOGI(TAG, "Save user id: %s", user_id_str.c_str());
    std::string user_token_str = user_token->valuestring ? user_token->valuestring : "";
    settings.SetString(kAppConfigKeyUserToken, user_token_str);
    ESP_LOGI(TAG, "Save user token: %s", user_token_str.c_str());
    cJSON_Delete(root);
}
