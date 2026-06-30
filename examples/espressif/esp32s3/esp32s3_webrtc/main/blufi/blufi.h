#pragma once

#include <esp_blufi_api.h>
#include <esp_err.h>
#include <esp_wifi.h>

/** BLUFI 配网封装，与 `blufi_init.c` / `blufi_security.c` 配合使用。 */
class Blufi {
public:
    static Blufi& GetInstance() {
        static Blufi instance;
        return instance;
    }

    Blufi(const Blufi&) = delete;
    Blufi& operator=(const Blufi&) = delete;

    /** 初始化 BT 控制器（若启用）、BLUFI Host 与回调，并打印版本。 */
    esp_err_t Init();

    bool IsBleConnected() const { return ble_connected_; }
    const wifi_config_t& GetStaConfig() const { return sta_config_; }

private:
    Blufi();
    static void EventCallback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param);
    void OnBlufiEvent(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param);

    wifi_config_t sta_config_{};
    bool ble_connected_{false};
    esp_blufi_callbacks_t callbacks_{};
};
