#include "ota.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_https_ota.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <memory>

#include <mbedtls/md5.h>

#define TAG "Ota"

namespace {

bool NormalizeMd5Hex(std::string* s) {
    std::string out;
    out.reserve(32);
    for (char c : *s) {
        if (c == ' ' || c == '\t') {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    *s = std::move(out);
    if (s->size() != 32) {
        return false;
    }
    return std::all_of(s->begin(), s->end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string Md5DigestToHex(const unsigned char digest[16]) {
    static const char* hex = "0123456789abcdef";
    std::string out(32, '\0');
    for (int i = 0; i < 16; ++i) {
        out[static_cast<size_t>(i * 2)] = hex[digest[i] >> 4];
        out[static_cast<size_t>(i * 2 + 1)] = hex[digest[i] & 0xf];
    }
    return out;
}

class StreamingMd5 {
public:
    explicit StreamingMd5(std::string expected_hex32_normalized)
        : enabled_(!expected_hex32_normalized.empty()),
          expected_(std::move(expected_hex32_normalized)) {
        if (enabled_) {
            mbedtls_md5_init(&ctx_);
            (void)mbedtls_md5_starts(&ctx_);
            started_ = true;
        }
    }

    ~StreamingMd5() {
        if (started_) {
            mbedtls_md5_free(&ctx_);
        }
    }

    void Update(const void* data, size_t len) {
        if (enabled_ && len > 0) {
            (void)mbedtls_md5_update(&ctx_, static_cast<const unsigned char*>(data), len);
        }
    }

    bool Verify() {
        if (!enabled_) {
            return true;
        }
        unsigned char digest[16];
        (void)mbedtls_md5_finish(&ctx_, digest);
        mbedtls_md5_free(&ctx_);
        started_ = false;
        std::string hex = Md5DigestToHex(digest);
        if (hex != expected_) {
            ESP_LOGE(TAG, "Firmware MD5 mismatch: expected %s, actual %s", expected_.c_str(), hex.c_str());
            return false;
        }
        ESP_LOGI(TAG, "Firmware MD5 verified: %s", hex.c_str());
        return true;
    }

private:
    bool enabled_;
    bool started_{false};
    std::string expected_;
    mbedtls_md5_context ctx_{};
};

}  // namespace

Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

/* 
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
 esp_err_t Ota::CheckVersion() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());


    std::string data = "{\"server_time\":{\"timestamp\":1777109194165,\"timezone_offset\":480}}";
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "JSON response: %s", cJSON_Print(root));

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;
            
            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
            
            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            struct tm tm_info = {};
            gmtime_r(&tv.tv_sec, &tm_info);
            char formatted_server_time[20] = {0};
            if (strftime(
                    formatted_server_time,
                    sizeof(formatted_server_time),
                    "%Y-%m-%d %H:%M:%S",
                    &tm_info) == 0) {
                ESP_LOGW(TAG, "Failed to format server time");
                formatted_server_time[0] = '\0';
            }
            ESP_LOGI(TAG, "Server time (UTC+8): %s", formatted_server_time);
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url, const std::string& expected_md5) {
    auto notify_state = [this](OtaUpgradeState state, esp_err_t err = ESP_OK, const std::string& desc = {}) {
        if (upgrade_state_callback_) {
            upgrade_state_callback_(state, err, desc);
        }
    };

    notify_state(OTA_UPGRADE_START, ESP_OK, "开始固件升级");

    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    std::string md5_norm = expected_md5;
    std::unique_ptr<StreamingMd5> md5_verifier;
    if (!expected_md5.empty()) {
        if (!NormalizeMd5Hex(&md5_norm)) {
            ESP_LOGE(TAG, "Invalid md5 (expect 32 hex characters): %s", expected_md5.c_str());
            notify_state(OTA_UPGRADE_FAILED, ESP_ERR_INVALID_ARG, "MD5校验值格式无效");
            return false;
        }
        md5_verifier = std::make_unique<StreamingMd5>(std::move(md5_norm));
    }

    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        notify_state(OTA_UPGRADE_FAILED, ESP_ERR_NOT_FOUND, "未找到可用的OTA分区");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    notify_state(OTA_DOWNLOAD_START, ESP_OK, "开始下载固件");

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        notify_state(OTA_DOWNLOAD_FAILED, ESP_FAIL, "HTTP连接失败");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        notify_state(OTA_DOWNLOAD_FAILED, ESP_ERR_INVALID_RESPONSE, "HTTP响应状态码异常: " + std::to_string(http->GetStatusCode()));
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        notify_state(OTA_DOWNLOAD_FAILED, ESP_ERR_INVALID_SIZE, "固件文件大小为0");
        return false;
    }

    char buffer[512];
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        int ret = http->Read(buffer, sizeof(buffer));
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            notify_state(OTA_DOWNLOAD_FAILED, static_cast<esp_err_t>(ret), "读取HTTP数据失败");
            return false;
        }

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (upgrade_callback_) {
                upgrade_callback_(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (ret == 0) {
            break;
        }

        if (md5_verifier) {
            md5_verifier->Update(buffer, static_cast<size_t>(ret));
        }

        if (!image_header_checked) {
            image_header.append(buffer, ret);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));
                
                auto current_version = esp_app_get_description()->version;
                ESP_LOGI(TAG, "Current version: %s, New version: %s", current_version, new_app_info.version);

                esp_err_t begin_err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
                if (begin_err != ESP_OK) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    notify_state(OTA_DOWNLOAD_FAILED, begin_err, "OTA写入初始化失败");
                    return false;
                }

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }
        auto err = esp_ota_write(update_handle, buffer, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            notify_state(OTA_DOWNLOAD_FAILED, err, "写入OTA数据失败");
            return false;
        }
    }
    http->Close();

    notify_state(OTA_DOWNLOAD_SUCCESS, ESP_OK, "固件下载完成");
    if (md5_verifier) {
        if (total_read != content_length) {
            ESP_LOGE(TAG, "Incomplete download: %u/%u bytes", static_cast<unsigned>(total_read),
                     static_cast<unsigned>(content_length));
            esp_ota_abort(update_handle);
            notify_state(OTA_UPGRADE_FAILED, ESP_ERR_INVALID_SIZE, "固件下载不完整");
            return false;
        }
        if (!md5_verifier->Verify()) {
            esp_ota_abort(update_handle);
            notify_state(OTA_UPGRADE_FAILED, ESP_ERR_INVALID_CRC, "MD5校验失败");
            return false;
        }
        md5_verifier.reset();
    }

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        notify_state(OTA_UPGRADE_FAILED, err, "固件镜像验证失败");
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        notify_state(OTA_UPGRADE_FAILED, err, "设置启动分区失败");
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    notify_state(OTA_UPGRADE_SUCCESS, ESP_OK, "固件升级成功");
    return true;
}

namespace {

// HTTP 事件处理上下文，用于在 esp_https_ota 内部 HTTP 客户端收到 body 数据时
// 同步喂给 MD5 校验器（esp_https_ota 自身不暴露原始数据缓冲区）。
struct OtaHttpEventCtx {
    StreamingMd5* md5_verifier;
};

esp_err_t OtaHttpEventHandler(esp_http_client_event_t* evt) {
    if (evt == nullptr) {
        return ESP_OK;
    }
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        auto* ctx = static_cast<OtaHttpEventCtx*>(evt->user_data);
        if (ctx != nullptr && ctx->md5_verifier != nullptr) {
            ctx->md5_verifier->Update(evt->data, static_cast<size_t>(evt->data_len));
        }
    }
    return ESP_OK;
}

}  // namespace

bool Ota::UpgradeNew(const std::string& firmware_url, const std::string& expected_md5) {
    auto notify_state = [this](OtaUpgradeState state, esp_err_t err = ESP_OK, const std::string& desc = {}) {
        if (upgrade_state_callback_) {
            upgrade_state_callback_(state, err, desc);
        }
    };

    notify_state(OTA_UPGRADE_START, ESP_OK, "开始固件升级(HTTPS)");

    ESP_LOGI(TAG, "Upgrading firmware (https_ota) from %s", firmware_url.c_str());

    std::string md5_norm = expected_md5;
    std::unique_ptr<StreamingMd5> md5_verifier;
    if (!expected_md5.empty()) {
        if (!NormalizeMd5Hex(&md5_norm)) {
            ESP_LOGE(TAG, "Invalid md5 (expect 32 hex characters): %s", expected_md5.c_str());
            notify_state(OTA_UPGRADE_FAILED, ESP_ERR_INVALID_ARG, "MD5校验值格式无效");
            return false;
        }
        md5_verifier = std::make_unique<StreamingMd5>(std::move(md5_norm));
    }

    OtaHttpEventCtx http_ctx{ md5_verifier.get() };

    esp_http_client_config_t http_config = {};
    http_config.url = firmware_url.c_str();
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.timeout_ms = 15000;
    http_config.keep_alive_enable = true;
    http_config.buffer_size = 4096;        // 4KB
    http_config.buffer_size_tx = 4096;
    http_config.event_handler = OtaHttpEventHandler;
    http_config.user_data = &http_ctx;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;

    // 注意：不要开启 partial_http_download。
    // ESP-IDF 在 partial 模式下会先发一次 HEAD 请求拿 Content-Length，
    // 而 saibotan-pre 网关对 HEAD 返回 404（只允许 GET），会直接导致
    // "esp_https_ota: Received incorrect http status 404" / OTA begin failed.
    ota_config.partial_http_download = false;

    notify_state(OTA_DOWNLOAD_START, ESP_OK, "开始下载固件(HTTPS)");

    esp_https_ota_handle_t https_ota_handle = nullptr;
    esp_err_t ret = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (ret != ESP_OK || https_ota_handle == nullptr) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        notify_state(OTA_DOWNLOAD_FAILED, ret, "HTTPS OTA初始化失败");
        return false;
    }

    ESP_LOGI(TAG, "OTA started...");

    int image_size = esp_https_ota_get_image_size(https_ota_handle);
    int last_image_len = 0;
    int64_t last_calc_time = esp_timer_get_time();

    while (true) {
        ret = esp_https_ota_perform(https_ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        int cur_len = esp_https_ota_get_image_len_read(https_ota_handle);
        int64_t now = esp_timer_get_time();
        if (now - last_calc_time >= 1000000) {
            int recent = cur_len - last_image_len;
            int progress = image_size > 0 ? (cur_len * 100 / image_size) : 0;
            ESP_LOGI(TAG, "Progress: %d%% (%d/%d), Speed: %dB/s",
                     progress, cur_len, image_size, recent);
            if (upgrade_callback_) {
                upgrade_callback_(progress, static_cast<size_t>(recent));
            }
            last_calc_time = now;
            last_image_len = cur_len;
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(ret));
        esp_https_ota_abort(https_ota_handle);
        notify_state(OTA_DOWNLOAD_FAILED, ret, "固件下载过程中出错");
        return false;
    }

    if (!esp_https_ota_is_complete_data_received(https_ota_handle)) {
        ESP_LOGE(TAG, "OTA image incomplete");
        esp_https_ota_abort(https_ota_handle);
        notify_state(OTA_DOWNLOAD_FAILED, ESP_ERR_INVALID_SIZE, "固件下载不完整");
        return false;
    }

    // 最后一次进度回调，确保 UI 能看到 100%
    {
        int cur_len = esp_https_ota_get_image_len_read(https_ota_handle);
        int recent = cur_len - last_image_len;
        int progress = image_size > 0 ? (cur_len * 100 / image_size) : 100;
        ESP_LOGI(TAG, "OTA image complete, %d/%d bytes", cur_len, image_size);
        if (upgrade_callback_) {
            upgrade_callback_(progress, static_cast<size_t>(recent > 0 ? recent : 0));
        }
    }

    notify_state(OTA_DOWNLOAD_SUCCESS, ESP_OK, "固件下载完成(HTTPS)");

    if (md5_verifier && !md5_verifier->Verify()) {
        esp_https_ota_abort(https_ota_handle);
        notify_state(OTA_UPGRADE_FAILED, ESP_ERR_INVALID_CRC, "MD5校验失败");
        return false;
    }
    md5_verifier.reset();

    esp_err_t finish_err = esp_https_ota_finish(https_ota_handle);
    if (finish_err != ESP_OK) {
        if (finish_err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to finish OTA: %s", esp_err_to_name(finish_err));
        }
        notify_state(OTA_UPGRADE_FAILED, finish_err, "固件镜像验证失败");
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful (https_ota)");
    notify_state(OTA_UPGRADE_SUCCESS, ESP_OK, "固件升级成功(HTTPS)");
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    upgrade_state_callback_ = nullptr;
    upgrade_callback_ = callback;
    return UpgradeNew(firmware_url_, firmware_md5_);
}

bool Ota::StartUpgradeFromUrl(const std::string& url, const std::string& expected_md5,
    std::function<void(int progress, size_t speed)> callback,
    std::function<void(OtaUpgradeState state, esp_err_t err, const std::string& desc)> state_callback) {
    upgrade_callback_ = callback;
    upgrade_state_callback_ = std::move(state_callback);
    bool ok = UpgradeNew(url, expected_md5);
    upgrade_state_callback_ = nullptr;
    return ok;
}

std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;
    
    while (std::getline(ss, segment, '.')) {
        versionNumbers.push_back(std::stoi(segment));
    }
    
    return versionNumbers;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }
    
    return newer.size() > current.size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }
    
    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
