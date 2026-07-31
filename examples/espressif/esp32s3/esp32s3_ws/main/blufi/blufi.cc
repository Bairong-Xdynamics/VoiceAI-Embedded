#include "blufi.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <string>

#include <esp_timer.h>

#include "application.h"
#include "system_info.h"
#include "wifi_configuration_ap.h"

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include <esp_bt.h>
#endif
#include <esp_blufi.h>
#if CONFIG_BT_BLUEDROID_ENABLED
#include <esp_gap_ble_api.h>
#include <esp_mac.h>
#endif

#include "blu_wifi.h"

namespace {

/** 从 BLUFI custom_data 原始字节构造激活用 JSON 字符串：按长度截取、去首尾空白、可选去 UTF-8 BOM。 */
std::string BuildActivatePayloadFromCustomData(const uint8_t* data, uint32_t data_len) {
    if (data == nullptr || data_len == 0) {
        return {};
    }
    std::string s(reinterpret_cast<const char*>(data), static_cast<size_t>(data_len));
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) {
        --j;
    }
    s = s.substr(i, j - i);
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

#if CONFIG_BT_BLUEDROID_ENABLED
/** BLE 广播「完整本地名」，长度需不超过 CONFIG_BT_DEVICE_NAME_MAX_LEN（默认 32，含结束符）。 */
std::string BuildBlufiAdvDeviceName() {
    std::string mac = SystemInfo::GetMacAddress();
    //std::string name = std::string("BR_ESP32S3_") + mac;
    std::string name = std::string("BR_ESP32S3");
    return name;
}

/**
 * 与 IDF `bluedroid_host/esp_blufi.c` 中 blufi_adv_data 一致。
 * 不能调用 `esp_blufi_adv_start()`：其实现会先 `esp_ble_gap_set_device_name(BLUFI_DEVICE_NAME)`，
 * 会覆盖应用层刚设置的名称；对端扫描到的名称来自 `config_adv_data` 打包时的 GAP 设备名。
 */
static uint8_t k_blufi_adv_service_uuid[32] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00,
};

/** 厂商数据：[0xE5,0x02]=Espressif CID(0x02E5) + 6 字节 STA MAC；放在 Scan Response 以免主广播超 31 字节。 */
static uint8_t k_blufi_manufacturer_data[8] = {0xE5, 0x02, 0, 0, 0, 0, 0, 0};

static esp_ble_adv_data_t k_blufi_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = nullptr,
    .service_data_len = 0,
    .p_service_data = nullptr,
    .service_uuid_len = 16,
    .p_service_uuid = k_blufi_adv_service_uuid,
    .flag = 0x6,
};

static esp_ble_adv_data_t k_blufi_scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = false,
    .include_txpower = false,
    .min_interval = 0,
    .max_interval = 0,
    .appearance = 0x00,
    .manufacturer_len = sizeof(k_blufi_manufacturer_data),
    .p_manufacturer_data = k_blufi_manufacturer_data,
    .service_data_len = 0,
    .p_service_data = nullptr,
    .service_uuid_len = 0,
    .p_service_uuid = nullptr,
    .flag = 0,
};

static esp_ble_adv_params_t k_blufi_adv_params = {
    .adv_int_min = 0x100,
    .adv_int_max = 0x100,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

void FillBlufiAdvManufacturerData() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    memcpy(&k_blufi_manufacturer_data[2], mac, sizeof(mac));
}

extern "C" void esp_blufi_app_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        if (param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_config_adv_data(&k_blufi_scan_rsp_data);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        if (param->scan_rsp_data_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_start_advertising(&k_blufi_adv_params);
        }
        break;
    default:
        break;
    }
}
#endif

/** Bluedroid：先设设备名再 `config_adv_data`（与 IDF BLUFI 广播内容一致），由 gap 回调启动广播。 */
void BlufiAdvStartWithLocalName() {
#if CONFIG_BT_BLUEDROID_ENABLED
    const std::string name = BuildBlufiAdvDeviceName();
    esp_err_t err = esp_ble_gap_set_device_name(name.c_str());
    if (err != ESP_OK) {
        ESP_LOGW(BLU_WIFI_TAG, "esp_ble_gap_set_device_name: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(BLU_WIFI_TAG, "BLE adv device name: %s", name.c_str());
    }
    FillBlufiAdvManufacturerData();
    ESP_LOGI(BLU_WIFI_TAG, "BLE adv manufacturer MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             k_blufi_manufacturer_data[2], k_blufi_manufacturer_data[3], k_blufi_manufacturer_data[4],
             k_blufi_manufacturer_data[5], k_blufi_manufacturer_data[6], k_blufi_manufacturer_data[7]);
    err = esp_ble_gap_config_adv_data(&k_blufi_adv_data);
    if (err != ESP_OK) {
        ESP_LOGE(BLU_WIFI_TAG, "esp_ble_gap_config_adv_data: %s", esp_err_to_name(err));
    }
#else
    esp_blufi_adv_start();
#endif
}

}  // namespace


#ifndef CONFIG_BLU_WIFI_AUTH_WPA2_PSK
#define CONFIG_BLU_WIFI_AUTH_WPA2_PSK 1
#endif

#if CONFIG_BLU_WIFI_AUTH_OPEN
#define BLU_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_BLU_WIFI_AUTH_WEP
#define BLU_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_BLU_WIFI_AUTH_WPA_PSK
#define BLU_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_BLU_WIFI_AUTH_WPA2_PSK
#define BLU_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_BLU_WIFI_AUTH_WPA_WPA2_PSK
#define BLU_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_BLU_WIFI_AUTH_WPA3_PSK
#define BLU_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_BLU_WIFI_AUTH_WPA2_WPA3_PSK
#define BLU_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_BLU_WIFI_AUTH_WAPI_PSK
#define BLU_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

Blufi::Blufi() {
    callbacks_ = {
        .event_cb = EventCallback,
        .negotiate_data_handler = blufi_dh_negotiate_data_handler,
        .encrypt_func = blufi_aes_encrypt,
        .decrypt_func = blufi_aes_decrypt,
        .checksum_func = blufi_crc_checksum,
    };
}

void Blufi::EventCallback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    GetInstance().OnBlufiEvent(event, param);
}

void Blufi::OnBlufiEvent(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        ESP_LOGI(BLU_WIFI_TAG, "BLUFI init finish");
        BlufiAdvStartWithLocalName();
        break;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        ESP_LOGI(BLU_WIFI_TAG, "BLUFI deinit finish");
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        ESP_LOGI(BLU_WIFI_TAG, "BLUFI ble connect");
        ble_connected_ = true;
        esp_blufi_adv_stop();
        blufi_security_init();
        /*
        // 模拟：连接成功后延迟 2s 主动触发 WiFi 扫描（等待安全协商完成）
        {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto* blufi = static_cast<Blufi*>(arg);
                    ESP_LOGI(BLU_WIFI_TAG, "BLE connect: sending cached WiFi list");
                    blufi->SendCachedWifiList();
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "blufi_scan",
                .skip_unhandled_events = true,
            };
            esp_timer_handle_t scan_timer = nullptr;
            esp_timer_create(&timer_args, &scan_timer);
            esp_timer_start_once(scan_timer, 2 * 1000 * 1000);
        }
        */
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        ESP_LOGI(BLU_WIFI_TAG, "BLUFI ble disconnect");
        ble_connected_ = false;
        blufi_security_deinit();
        BlufiAdvStartWithLocalName();
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        ESP_LOGI(BLU_WIFI_TAG, "BLUFI set wifi opmode");
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        ESP_LOGI(BLU_WIFI_TAG, "BLUFI request wifi connect to AP");
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        ESP_LOGI(BLU_WIFI_TAG, "BLUFI request wifi disconnect from AP");
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        ESP_LOGE(BLU_WIFI_TAG, "BLUFI report error, error code %d", param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        ESP_LOGI(BLU_WIFI_TAG, "BLUFI get wifi status");
        break;
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        ESP_LOGI(BLU_WIFI_TAG, "blufi close a gatt connection");
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(sta_config_.sta.bssid, param->sta_bssid.bssid, 6);
        sta_config_.sta.bssid_set = 1;
        ESP_LOGI(BLU_WIFI_TAG, "Recv STA BSSID %s", sta_config_.sta.ssid);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len >= sizeof(sta_config_.sta.ssid) / sizeof(sta_config_.sta.ssid[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            ESP_LOGE(BLU_WIFI_TAG, "Invalid STA SSID length: %d", param->sta_ssid.ssid_len);
            break;
        }
        strncpy(reinterpret_cast<char*>(sta_config_.sta.ssid), reinterpret_cast<char*>(param->sta_ssid.ssid),
                param->sta_ssid.ssid_len);
        sta_config_.sta.ssid[param->sta_ssid.ssid_len] = '\0';
        ESP_LOGI(BLU_WIFI_TAG, "Recv STA SSID %s", sta_config_.sta.ssid);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:{
        if (param->sta_passwd.passwd_len >=
            sizeof(sta_config_.sta.password) / sizeof(sta_config_.sta.password[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            ESP_LOGE(BLU_WIFI_TAG, "Invalid STA PASSWORD length: %d", param->sta_passwd.passwd_len);
            break;
        }
        strncpy(reinterpret_cast<char*>(sta_config_.sta.password), reinterpret_cast<char*>(param->sta_passwd.passwd),
                param->sta_passwd.passwd_len);
        sta_config_.sta.password[param->sta_passwd.passwd_len] = '\0';
        sta_config_.sta.threshold.authmode = BLU_WIFI_SCAN_AUTH_MODE_THRESHOLD;
        ESP_LOGI(BLU_WIFI_TAG, "Recv STA PASSWORD len=%d", param->sta_passwd.passwd_len);
                
        char ssid[32], password[64];
        memcpy(ssid, sta_config_.sta.ssid, sizeof(sta_config_.sta.ssid));
        memcpy(password, sta_config_.sta.password, sizeof(sta_config_.sta.password));
        ssid[sizeof(ssid) - 1] = '\0';
        password[sizeof(password) - 1] = '\0';
        const bool success =
            Application::GetInstance().StartNetwork(std::string(ssid), std::string(password));
        if (ble_connected_) {
            wifi_mode_t mode = WIFI_MODE_STA;
            if (esp_wifi_get_mode(&mode) != ESP_OK) {
                mode = WIFI_MODE_STA;
            }
            esp_blufi_extra_info_t extra{};
            extra.sta_ssid = sta_config_.sta.ssid;
            extra.sta_ssid_len =
                static_cast<int>(strnlen(reinterpret_cast<const char*>(sta_config_.sta.ssid),
                                         sizeof(sta_config_.sta.ssid)));
            const esp_blufi_sta_conn_state_t conn_state =
                success ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_CONN_FAIL;
            const esp_err_t err_report =
                esp_blufi_send_wifi_conn_report(mode, conn_state, 0, &extra);
            if (err_report != ESP_OK) {
                ESP_LOGW(BLU_WIFI_TAG, "esp_blufi_send_wifi_conn_report: %s", esp_err_to_name(err_report));
            }
            ESP_LOGI(BLU_WIFI_TAG, "Send wifi conn report: %s", conn_state == ESP_BLUFI_STA_CONN_SUCCESS ? "success" : "fail");
        }
        break;
    }
    case ESP_BLUFI_EVENT_GET_WIFI_LIST:
        ESP_LOGI(BLU_WIFI_TAG, "BLUFI get wifi list");
        SendCachedWifiList();
        break;
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA: {
        const uint8_t* const payload = param->custom_data.data;
        const uint32_t raw_len = param->custom_data.data_len;
        ESP_LOGI(BLU_WIFI_TAG, "Recv custom data: ptr=%p len=%" PRIu32, static_cast<const void*>(payload), raw_len);
        if (payload == nullptr || raw_len == 0) {
            ESP_LOGW(BLU_WIFI_TAG, "custom_data: empty");
            break;
        }
        constexpr uint32_t kMaxActivateJsonBytes = 1024;
        if (raw_len > kMaxActivateJsonBytes) {
            ESP_LOGE(BLU_WIFI_TAG, "custom_data: len %" PRIu32 " exceeds max %" PRIu32, raw_len,
                     kMaxActivateJsonBytes);
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        std::string json = BuildActivatePayloadFromCustomData(payload, raw_len);
        if (json.empty()) {
            ESP_LOGW(BLU_WIFI_TAG, "custom_data: empty after trim/BOM strip");
            break;
        }
        if (json.front() != '{') {
            ESP_LOGE(BLU_WIFI_TAG, "custom_data: expected JSON object, first=0x%02x",
                     static_cast<unsigned>(static_cast<unsigned char>(json.front())));
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        ESP_LOGI(BLU_WIFI_TAG, "Activate JSON %s", json.c_str());
        Application::GetInstance().ActivateParameter(json);
        break;
    }
    default:
        break;
    }
}

esp_err_t Blufi::Init() {
    esp_err_t ret = ESP_OK;

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = esp_blufi_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(BLU_WIFI_TAG, "%s BLUFI controller init failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
#endif

    ret = esp_blufi_host_and_cb_init(&callbacks_);
    if (ret != ESP_OK) {
        ESP_LOGE(BLU_WIFI_TAG, "%s initialise failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(BLU_WIFI_TAG, "BLUFI VERSION %04x", esp_blufi_get_version());
    return ESP_OK;
}

void Blufi::SendCachedWifiList() {
    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    auto ap_records = wifi_ap.GetAccessPoints();

    if (ap_records.empty()) {
        ESP_LOGI(BLU_WIFI_TAG, "No cached AP results from WifiConfigurationAp");
        return;
    }

    // 按 RSSI 降序排列
    std::sort(ap_records.begin(), ap_records.end(),
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });

    // 过滤：仅上报 authmode >= 阈值的 AP
    uint16_t valid_num = 0;
    for (uint16_t i = 0; i < ap_records.size(); i++) {
        if (ap_records[i].authmode >= BLU_WIFI_SCAN_AUTH_MODE_THRESHOLD) {
            if (valid_num != i) {
                ap_records[valid_num] = ap_records[i];
            }
            valid_num++;
        }
    }

    if (valid_num == 0) {
        ESP_LOGI(BLU_WIFI_TAG, "No AP matches authmode threshold");
        return;
    }

    ESP_LOGI(BLU_WIFI_TAG, "Sending WiFi list: total=%d valid=%d", ap_records.size(), valid_num);
    for (uint16_t i = 0; i < valid_num; i++) {
        ESP_LOGI(BLU_WIFI_TAG, "  AP[%d]: SSID=%s RSSI=%d Auth=%d",
                 i, ap_records[i].ssid, ap_records[i].rssi, ap_records[i].authmode);
    }

    // 转换为 esp_blufi_ap_record_t 格式
    auto* blufi_records = static_cast<esp_blufi_ap_record_t*>(
        malloc(valid_num * sizeof(esp_blufi_ap_record_t)));
    if (blufi_records != nullptr) {
        for (uint16_t i = 0; i < valid_num; i++) {
            memcpy(blufi_records[i].ssid, ap_records[i].ssid, sizeof(blufi_records[i].ssid));
            blufi_records[i].rssi = ap_records[i].rssi;
        }
        esp_blufi_send_wifi_list(valid_num, blufi_records);
        free(blufi_records);
    }
}
