#include "ml307_board.h"

#include "application.h"
#include "display.h"
#include "font_awesome_symbols.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <opus_encoder.h>

static const char *TAG = "Ml307Board";

Ml307Board::Ml307Board(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin) : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin) {
}

std::string Ml307Board::GetBoardType() {
    return "ml307";
}

void Ml307Board::StartNetwork() {
    auto& application = Application::GetInstance();
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::DETECTING_MODULE);

    while (true) {
        modem_ = AtModem::Detect(tx_pin_, rx_pin_, dtr_pin_, 921600);
        if (modem_ != nullptr) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    modem_->OnNetworkStateChanged([this, &application](bool network_ready) {
        if (network_ready) {
            ESP_LOGI(TAG, "Network is ready");
        } else {
            ESP_LOGE(TAG, "Network is down");
            auto device_state = application.GetDeviceState();
            if (device_state == kDeviceStateListening || device_state == kDeviceStateSpeaking) {
                application.Schedule([this, &application]() {
                    application.SetDeviceState(kDeviceStateIdle);
                });
            }
        }
    });

    // Wait for network ready
    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
    while (true) {
        auto result = modem_->WaitForNetworkReady();
        if (result == NetworkStatus::ErrorInsertPin) {
            application.Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "sad", Lang::Sounds::P3_ERR_PIN);
        } else if (result == NetworkStatus::ErrorRegistrationDenied) {
            application.Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "sad", Lang::Sounds::P3_ERR_REG);
        } else {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    // Print the ML307 modem information
    std::string module_revision = modem_->GetModuleRevision();
    std::string imei = modem_->GetImei();
    std::string iccid = modem_->GetIccid();
    ESP_LOGI(TAG, "ML307 Revision: %s", module_revision.c_str());
    ESP_LOGI(TAG, "ML307 IMEI: %s", imei.c_str());
    ESP_LOGI(TAG, "ML307 ICCID: %s", iccid.c_str());
}

NetworkInterface* Ml307Board::GetNetwork() {
    return modem_.get();
}

const char* Ml307Board::GetNetworkStateIcon() {
    if (modem_ == nullptr || !modem_->network_ready()) {
        return FONT_AWESOME_SIGNAL_OFF;
    }
    int csq = modem_->GetCsq();
    if (csq == -1) {
        return FONT_AWESOME_SIGNAL_OFF;
    } else if (csq >= 0 && csq <= 14) {
        return FONT_AWESOME_SIGNAL_1;
    } else if (csq >= 15 && csq <= 19) {
        return FONT_AWESOME_SIGNAL_2;
    } else if (csq >= 20 && csq <= 24) {
        return FONT_AWESOME_SIGNAL_3;
    } else if (csq >= 25 && csq <= 31) {
        return FONT_AWESOME_SIGNAL_4;
    }

    ESP_LOGW(TAG, "Invalid CSQ: %d", csq);
    return FONT_AWESOME_SIGNAL_OFF;
}

std::string Ml307Board::GetBoardJson() {
    // Set the board type for OTA
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    board_json += "\"revision\":\"" + modem_->GetModuleRevision() + "\",";
    board_json += "\"carrier\":\"" + modem_->GetCarrierName() + "\",";
    board_json += "\"csq\":\"" + std::to_string(modem_->GetCsq()) + "\",";
    board_json += "\"imei\":\"" + modem_->GetImei() + "\",";
    board_json += "\"iccid\":\"" + modem_->GetIccid() + "\",";
    board_json += "\"cereg\":" + modem_->GetRegistrationState().ToString() + "}";
    return board_json;
}

void Ml307Board::SetPowerSaveMode(bool enabled) {
    // TODO: Implement power save mode for ML307
}

std::string Ml307Board::GetDeviceStatusJson() {
    /*
     * 返回设备状态JSON
     * 
     * 返回的JSON结构如下：
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "cellular",
     *         "carrier": "CHINA MOBILE",
     *         "csq": 10
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        cJSON_AddStringToObject(screen, "theme", display->GetTheme().c_str());
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "cellular");
    cJSON_AddStringToObject(network, "carrier", modem_->GetCarrierName().c_str());
    int csq = modem_->GetCsq();
    if (csq == -1) {
        cJSON_AddStringToObject(network, "signal", "unknown");
    } else if (csq >= 0 && csq <= 14) {
        cJSON_AddStringToObject(network, "signal", "very weak");
    } else if (csq >= 15 && csq <= 19) {
        cJSON_AddStringToObject(network, "signal", "weak");
    } else if (csq >= 20 && csq <= 24) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else if (csq >= 25 && csq <= 31) {
        cJSON_AddStringToObject(network, "signal", "strong");
    }
    cJSON_AddItemToObject(root, "network", network);

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}

// 发送 GET 请求到指定 URL，返回响应内容
std::string Ml307Board::SendAndroidRequest(const std::string &text)
{
    char url[256];
    snprintf(url, sizeof(url), "http://192.168.2.120:8080?%s", text.c_str());

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to init http client");
        return "";
    }

    esp_err_t err = esp_http_client_perform(client);
    // 打印返回状态码
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                static_cast<int>(esp_http_client_get_status_code(client)),
                static_cast<int>(esp_http_client_get_content_length(client)));
    std::string response;
    if (err == ESP_OK)
    {
        int content_length = esp_http_client_get_content_length(client);
        if (content_length > 0)
        {
            char *buffer = new char[content_length + 1];
            int read_len = esp_http_client_read_response(client, buffer, content_length);
            if (read_len > 0)
            {
                buffer[read_len] = '\0';
                response.assign(buffer, read_len);
            }
            delete[] buffer;
        }
    }
    else
    {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return response;
}

// 实现一个函数来控制安卓显示表情 
void Ml307Board::ShowAndroidEmoji(const std::string &emoji_name)
{
    std::string command = "type=llm&text=" + emoji_name;
    SendAndroidRequest(command);
}

// 实现一个函数来控制安卓显示文本
void Ml307Board::ShowAndroidText(const std::string &text)
{
    std::string command = "type=text&text=\"" + text + "\"";
    SendAndroidRequest(command);
}

// 实现一个函数来控制安卓拍照并上传服务器
std::string Ml307Board::ShowAndroidTakePhoto()
{
    // take_photo 带两个参数一个是服务器地址一个是照片文件名，参数格式遵守http协议
    // 根据当前系统时间生成文件名
    time_t now = time(nullptr);
    struct tm *tm_info = localtime(&now);
    char filename[64];
    strftime(filename, sizeof(filename), "photo_%Y%m%d_%H%M%S.jpg", tm_info);
    std::string command = "type=takePhoto&name=" + std::string(filename);
    SendAndroidRequest(command);
    return std::string(filename);
}

