#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "ml307_ssl_transport.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "iot/thing_manager.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "audio_debugger.h"

#if CONFIG_USE_AUDIO_PROCESSOR
#include "afe_audio_processor.h"
#else
#include "no_audio_processor.h"
#endif

#if CONFIG_USE_AFE_WAKE_WORD
#include "afe_wake_word.h"
#elif CONFIG_USE_ESP_WAKE_WORD
#include "esp_wake_word.h"
#else
#include "no_wake_word.h"
#endif

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <dirent.h>
#include "esp_camera.h"
#include <fstream>

#define TAG "Application"


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

Application::Application() {
    event_group_ = xEventGroupCreate();
    background_task_ = new BackgroundTask(4096 * 7);

#if CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

#if CONFIG_USE_AFE_WAKE_WORD
    wake_word_ = std::make_unique<AfeWakeWord>();
#elif CONFIG_USE_ESP_WAKE_WORD
    wake_word_ = std::make_unique<EspWakeWord>();
#else
    wake_word_ = std::make_unique<NoWakeWord>();
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            app->OnClockTimer();
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
    if (background_task_ != nullptr) {
        delete background_task_;
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[128];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "sad", Lang::Sounds::P3_EXCLAMATION);

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
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "happy", Lang::Sounds::P3_UPGRADE);

            vTaskDelay(pdMS_TO_TICKS(3000));

            SetDeviceState(kDeviceStateUpgrading);
            
            display->SetIcon(FONT_AWESOME_DOWNLOAD);
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            auto& board = Board::GetInstance();
            board.SetPowerSaveMode(false);
            wake_word_->StopDetection();
            // 预先关闭音频输出，避免升级过程有音频操作
            auto codec = board.GetAudioCodec();
            codec->EnableInput(false);
            codec->EnableOutput(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                audio_decode_queue_.clear();
            }
            background_task_->WaitForCompletion();
            delete background_task_;
            background_task_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(1000));

            ota.StartUpgrade([display](int progress, size_t speed) {
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            });

            // If upgrade success, the device will reboot and never reach here
            display->SetStatus(Lang::Strings::UPGRADE_FAILED);
            ESP_LOGI(TAG, "Firmware upgrade failed...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            Reboot();
            return;
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::P3_0},
        digit_sound{'1', Lang::Sounds::P3_1}, 
        digit_sound{'2', Lang::Sounds::P3_2},
        digit_sound{'3', Lang::Sounds::P3_3},
        digit_sound{'4', Lang::Sounds::P3_4},
        digit_sound{'5', Lang::Sounds::P3_5},
        digit_sound{'6', Lang::Sounds::P3_6},
        digit_sound{'7', Lang::Sounds::P3_7},
        digit_sound{'8', Lang::Sounds::P3_8},
        digit_sound{'9', Lang::Sounds::P3_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "happy", Lang::Sounds::P3_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert %s: %s [%s]", status, message, emotion);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        ResetDecoder();
        PlaySound(sound);
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

void Application::PlaySound(const std::string_view& sound) {
    // Wait for the previous sound to finish
    {
        std::unique_lock<std::mutex> lock(mutex_);
        audio_decode_cv_.wait(lock, [this]() {
            return audio_decode_queue_.empty();
        });
    }
    background_task_->WaitForCompletion();

    const char* data = sound.data();
    size_t size = sound.size();
    for (const char* p = data; p < data + size; ) {
        auto p3 = (BinaryProtocol3*)p;
        p += sizeof(BinaryProtocol3);

        auto payload_size = ntohs(p3->payload_size);
        AudioStreamPacket packet;
        packet.sample_rate = 16000;
        packet.frame_duration = 60;
        packet.payload.resize(payload_size);
        memcpy(packet.payload.data(), p3->payload, payload_size);
        p += payload_size;

        std::lock_guard<std::mutex> lock(mutex_);
        audio_decode_queue_.emplace_back(std::move(packet));
    }
}

void Application::EnterAudioTestingMode() {
    ESP_LOGI(TAG, "Entering audio testing mode");
    ResetDecoder();
    SetDeviceState(kDeviceStateAudioTesting);
}

void Application::ExitAudioTestingMode() {
    ESP_LOGI(TAG, "Exiting audio testing mode");
    SetDeviceState(kDeviceStateWifiConfiguring);
    // Copy audio_testing_queue_ to audio_decode_queue_
    std::lock_guard<std::mutex> lock(mutex_);
    audio_decode_queue_ = std::move(audio_testing_queue_);
    audio_decode_cv_.notify_all();
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        EnterAudioTestingMode();
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        ExitAudioTestingMode();
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
        EnterAudioTestingMode();
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
        ExitAudioTestingMode();
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
// --- 宏定义你的key和API参数 ---
// --- 宏定义你的key和API参数 ---
#define ALIYUN_ACCESS_KEY_ID     "YOUR_ACCESS_KEY_ID"
#define ALIYUN_ACCESS_KEY_SECRET "YOUR_ACCESS_KEY_SECRET"
#define ALIYUN_API_URL           "https://facebody.cn-shanghai.aliyuncs.com"
#define ALIYUN_API_VERSION       "2019-12-30"
#define ALIYUN_FACE_DB_NAME      "xiaozhi_face_db"  // 人脸库名称

#include "mbedtls/md.h"
#include <ctime>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <dirent.h>
#include "esp_http_client.h"
#include "cJSON.h"

// 获取UTC时间字符串
std::string GetUtcDateString() {
    char buf[64];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm); // 阿里云要求ISO8601格式
    return std::string(buf);
}

// 前置声明 UrlEncode 和 BuildAliyunSignature
std::string UrlEncode(const std::string& value);
std::string BuildAliyunSignature(const std::string& http_method, const std::string& canonicalized_query_string);

// base64编码
std::string base64_encode(const unsigned char* data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ret;
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; ++i) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            ret.push_back(table[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) ret.push_back(table[((val << 8) >> (valb + 8)) & 0x3F]);
    while (ret.size() % 4) ret.push_back('=');
    return ret;
}

// 创建人脸库
std::string Application::CreateFaceDB(const std::string& db_name) {
    std::string nonce, timestamp;
    std::ostringstream oss;
    oss << "AccessKeyId=" << UrlEncode(ALIYUN_ACCESS_KEY_ID)
        << "&Action=CreateFaceDb"
        << "&Format=json"
        << "&SignatureMethod=HMAC-SHA1"
        << "&SignatureNonce=" << (nonce = std::to_string(time(NULL)))
        << "&SignatureVersion=1.0"
        << "&Timestamp=" << UrlEncode(GetUtcDateString())
        << "&Version=" << ALIYUN_API_VERSION
        << "&Name=" << UrlEncode(db_name);
    
    std::string query_string = oss.str();
    std::string signature = BuildAliyunSignature("POST", query_string);
    std::string full_query = query_string + "&Signature=" + UrlEncode(signature);

    esp_http_client_config_t config = {
        .url = ALIYUN_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, full_query.c_str(), full_query.size());

    esp_err_t err = esp_http_client_perform(client);
    std::string response;
    if (err == ESP_OK) {
        int content_length = esp_http_client_get_content_length(client);
        if (content_length > 0) {
            char* buffer = new char[content_length + 1];
            int read_len = esp_http_client_read_response(client, buffer, content_length);
            if (read_len > 0) {
                buffer[read_len] = '\0';
                response.assign(buffer, read_len);
                ESP_LOGI("AliyunFace", "CreateFaceDB Response: %s", buffer);
            }
            delete[] buffer;
        }
    }
    esp_http_client_cleanup(client);
    return response;
}

// 在现有的阿里云相关函数后添加以下新函数

// 获取人脸库中的所有人员列表
std::string Application::ListFacesInAliyunDB() {
    std::string nonce, timestamp;
    std::ostringstream oss;
    oss << "AccessKeyId=" << UrlEncode(ALIYUN_ACCESS_KEY_ID)
        << "&Action=ListFaces"
        << "&Format=json"
        << "&SignatureMethod=HMAC-SHA1"
        << "&SignatureNonce=" << (nonce = std::to_string(time(NULL)))
        << "&SignatureVersion=1.0"
        << "&Timestamp=" << UrlEncode(GetUtcDateString())
        << "&Version=" << ALIYUN_API_VERSION
        << "&DbName=" << UrlEncode(ALIYUN_FACE_DB_NAME)
        << "&Limit=100"  // 最多返回100个人员
        << "&Offset=0";  // 从第0个开始
    
    std::string query_string = oss.str();
    std::string signature = BuildAliyunSignature("POST", query_string);
    std::string full_query = query_string + "&Signature=" + UrlEncode(signature);

    ESP_LOGI("AliyunFace", "Listing faces in DB...");

    esp_http_client_config_t config = {
        .url = ALIYUN_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, full_query.c_str(), full_query.size());

    esp_err_t err = esp_http_client_perform(client);
    std::string response;
    if (err == ESP_OK) {
        int content_length = esp_http_client_get_content_length(client);
        if (content_length > 0) {
            char* buffer = new char[content_length + 1];
            int read_len = esp_http_client_read_response(client, buffer, content_length);
            if (read_len > 0) {
                buffer[read_len] = '\0';
                response.assign(buffer, read_len);
                ESP_LOGI("AliyunFace", "ListFaces Response: %s", buffer);
            }
            delete[] buffer;
        }
    } else {
        ESP_LOGE("AliyunFace", "ListFaces HTTP POST failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return response;
}

// 解析ListFaces结果，返回逗号分隔的人员名单
std::string Application::ParseListFacesResult(const std::string& response) {
    ESP_LOGI("AliyunFace", "Parsing list faces result...");
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        ESP_LOGE("AliyunFace", "Failed to parse JSON response");
        return "";
    }
    
    cJSON* data = cJSON_GetObjectItem(root, "Data");
    if (!data) {
        ESP_LOGE("AliyunFace", "Data field not found in response");
        cJSON_Delete(root);
        return "";
    }
    
    cJSON* faces = cJSON_GetObjectItem(data, "Faces");
    if (!cJSON_IsArray(faces)) {
        ESP_LOGI("AliyunFace", "No faces found in database");
        cJSON_Delete(root);
        return "";
    }
    
    std::vector<std::string> person_names;
    int faces_count = cJSON_GetArraySize(faces);
    
    for (int i = 0; i < faces_count; i++) {
        cJSON* face = cJSON_GetArrayItem(faces, i);
        if (face) {
            cJSON* entity_id = cJSON_GetObjectItem(face, "EntityId");
            if (cJSON_IsString(entity_id)) {
                std::string person_name = entity_id->valuestring;
                // 避免重复添加同一个人
                if (std::find(person_names.begin(), person_names.end(), person_name) == person_names.end()) {
                    person_names.push_back(person_name);
                    ESP_LOGI("AliyunFace", "Found person: %s", person_name.c_str());
                }
            }
        }
    }
    
    cJSON_Delete(root);
    
    // 将人员名单转换为逗号分隔的字符串
    std::string result;
    for (size_t i = 0; i < person_names.size(); ++i) {
        result += person_names[i];
        if (i != person_names.size() - 1) {
            result += ",";
        }
    }
    
    ESP_LOGI("AliyunFace", "Total unique persons found: %zu", person_names.size());
    return result;
}

// 添加人脸到数据库
std::string Application::AddFaceToAliyunDB(const std::string& person_name, const std::string& image_base64) {
    std::string nonce, timestamp;
    std::ostringstream oss;
    oss << "AccessKeyId=" << UrlEncode(ALIYUN_ACCESS_KEY_ID)
        << "&Action=AddFace"
        << "&Format=json"
        << "&SignatureMethod=HMAC-SHA1"
        << "&SignatureNonce=" << (nonce = std::to_string(time(NULL)))
        << "&SignatureVersion=1.0"
        << "&Timestamp=" << UrlEncode(GetUtcDateString())
        << "&Version=" << ALIYUN_API_VERSION
        << "&DbName=" << UrlEncode(ALIYUN_FACE_DB_NAME)
        << "&EntityId=" << UrlEncode(person_name)
        << "&ImageType=BASE64"
        << "&ImageData=" << UrlEncode(image_base64);
    
    std::string query_string = oss.str();
    std::string signature = BuildAliyunSignature("POST", query_string);
    std::string full_query = query_string + "&Signature=" + UrlEncode(signature);

    ESP_LOGI("AliyunFace", "Adding face to DB for person: %s", person_name.c_str());

    esp_http_client_config_t config = {
        .url = ALIYUN_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,  // 增加超时时间
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, full_query.c_str(), full_query.size());

    esp_err_t err = esp_http_client_perform(client);
    std::string response;
    if (err == ESP_OK) {
        int content_length = esp_http_client_get_content_length(client);
        if (content_length > 0) {
            char* buffer = new char[content_length + 1];
            int read_len = esp_http_client_read_response(client, buffer, content_length);
            if (read_len > 0) {
                buffer[read_len] = '\0';
                response.assign(buffer, read_len);
                ESP_LOGI("AliyunFace", "AddFace Response: %s", buffer);
            }
            delete[] buffer;
        }
    } else {
        ESP_LOGE("AliyunFace", "AddFace HTTP POST failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return response;
}

// 搜索人脸（1:N比对）
std::string Application::SearchFaceInAliyunDB(const std::string& image_base64) {
    std::string nonce, timestamp;
    std::ostringstream oss;
    oss << "AccessKeyId=" << UrlEncode(ALIYUN_ACCESS_KEY_ID)
        << "&Action=SearchFace"
        << "&Format=json"
        << "&SignatureMethod=HMAC-SHA1"
        << "&SignatureNonce=" << (nonce = std::to_string(time(NULL)))
        << "&SignatureVersion=1.0"
        << "&Timestamp=" << UrlEncode(GetUtcDateString())
        << "&Version=" << ALIYUN_API_VERSION
        << "&DbName=" << UrlEncode(ALIYUN_FACE_DB_NAME)
        << "&ImageType=BASE64"
        << "&ImageData=" << UrlEncode(image_base64)
        << "&Limit=5"  // 返回最多5个匹配结果
        << "&MaxFaceNum=1";  // 图片中最多识别1个人脸
    
    std::string query_string = oss.str();
    std::string signature = BuildAliyunSignature("POST", query_string);
    std::string full_query = query_string + "&Signature=" + UrlEncode(signature);

    ESP_LOGI("AliyunFace", "Searching face in DB...");

    esp_http_client_config_t config = {
        .url = ALIYUN_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, full_query.c_str(), full_query.size());

    esp_err_t err = esp_http_client_perform(client);
    std::string response;
    if (err == ESP_OK) {
        int content_length = esp_http_client_get_content_length(client);
        if (content_length > 0) {
            char* buffer = new char[content_length + 1];
            int read_len = esp_http_client_read_response(client, buffer, content_length);
            if (read_len > 0) {
                buffer[read_len] = '\0';
                response.assign(buffer, read_len);
                ESP_LOGI("AliyunFace", "SearchFace Response: %s", buffer);
            }
            delete[] buffer;
        }
    } else {
        ESP_LOGE("AliyunFace", "SearchFace HTTP POST failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return response;
}

// 解析搜索结果，返回匹配的人名
std::string Application::ParseSearchFaceResult(const std::string& response) {
    ESP_LOGI("AliyunFace", "Parsing search face result...");
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        ESP_LOGE("AliyunFace", "Failed to parse JSON response");
        return "";
    }
    
    cJSON* data = cJSON_GetObjectItem(root, "Data");
    if (!data) {
        ESP_LOGE("AliyunFace", "Data field not found in response");
        cJSON_Delete(root);
        return "";
    }
    
    cJSON* match_list = cJSON_GetObjectItem(data, "MatchList");
    if (!cJSON_IsArray(match_list) || cJSON_GetArraySize(match_list) == 0) {
        ESP_LOGI("AliyunFace", "No matches found");
        cJSON_Delete(root);
        return "";
    }
    
    // 获取第一个匹配结果
    cJSON* first_match = cJSON_GetArrayItem(match_list, 0);
    if (first_match) {
        cJSON* entity_id = cJSON_GetObjectItem(first_match, "EntityId");
        cJSON* similarity = cJSON_GetObjectItem(first_match, "Similarity");
        
        if (cJSON_IsString(entity_id) && cJSON_IsNumber(similarity)) {
            float score = similarity->valuedouble;
            if (score > 80.0f) {  // 相似度阈值
                std::string person_name = entity_id->valuestring;
                ESP_LOGI("AliyunFace", "Matched person: %s (similarity: %.2f)", person_name.c_str(), score);
                cJSON_Delete(root);
                return person_name;
            }
        }
    }
    
    cJSON_Delete(root);
    return "";
}

// URL编码
std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (char c : value) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << int((unsigned char)c);
        }
    }
    return escaped.str();
}

// HMAC-SHA1签名并base64编码
// HMAC-SHA1签名并base64编码 (使用mbedTLS)
std::string HmacSha1Base64(const std::string& key, const std::string& data) {
    unsigned char result[20]; // SHA1输出长度为20字节
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!md_info) {
        ESP_LOGE("AliyunFace", "mbedtls_md_info_from_type failed");
        return "";
    }
    int ret = mbedtls_md_hmac(md_info,
                              reinterpret_cast<const unsigned char*>(key.c_str()), key.length(),
                              reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
                              result);
    if (ret != 0) {
        ESP_LOGE("AliyunFace", "mbedtls_md_hmac failed: %d", ret);
        return "";
    }
    return base64_encode(result, sizeof(result));
}
// 构造请求参数（URL编码）
std::string BuildAliyunQueryString(const std::string& imgA_base64, const std::string& imgB_base64, std::string& nonce, std::string& timestamp) {
    nonce = std::to_string(time(NULL));
    timestamp = GetUtcDateString();
    std::ostringstream oss;
    oss << "AccessKeyId=" << UrlEncode(ALIYUN_ACCESS_KEY_ID)
        << "&Action=CompareFace"
        << "&Format=json"
        << "&SignatureMethod=HMAC-SHA1"
        << "&SignatureNonce=" << nonce
        << "&SignatureVersion=1.0"
        << "&Timestamp=" << UrlEncode(timestamp)
        << "&Version=" << ALIYUN_API_VERSION
        << "&ImageType=BASE64"
        << "&ImageA=" << UrlEncode(imgA_base64)
        << "&ImageB=" << UrlEncode(imgB_base64);
    return oss.str();
}

// 构造签名字符串（严格参考阿里云文档）
std::string BuildAliyunSignature(const std::string& http_method, const std::string& canonicalized_query_string) {
    std::string string_to_sign = http_method + "&%2F&" + UrlEncode(canonicalized_query_string);
    ESP_LOGI("AliyunFace", "StringToSign: %s", string_to_sign.c_str());
    return HmacSha1Base64(std::string(ALIYUN_ACCESS_KEY_SECRET) + "&", string_to_sign);
}

// 发送请求
std::string SendAliyunFaceCompareRequestStrict(const std::string& imgA_base64, const std::string& imgB_base64) {
    std::string nonce, timestamp;
    std::string query_string = BuildAliyunQueryString(imgA_base64, imgB_base64, nonce, timestamp);
    std::string signature = BuildAliyunSignature("POST", query_string);
    std::string full_query = query_string + "&Signature=" + UrlEncode(signature);

    ESP_LOGI("AliyunFace", "Request Query: %s", full_query.c_str());

    esp_http_client_config_t config = {
        .url = ALIYUN_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

    esp_http_client_set_post_field(client, full_query.c_str(), full_query.size());

    esp_err_t err = esp_http_client_perform(client);
    std::string response;
    if (err == ESP_OK) {
        int content_length = esp_http_client_get_content_length(client);
        ESP_LOGI("AliyunFace", "HTTP POST success, content_length=%d", content_length);
        if (content_length > 0) {
            char* buffer = new char[content_length + 1];
            int read_len = esp_http_client_read_response(client, buffer, content_length);
            if (read_len > 0) {
                buffer[read_len] = '\0';
                response.assign(buffer, read_len);
                ESP_LOGI("AliyunFace", "Response: %s", buffer);
            }
            delete[] buffer;
        }
    } else {
        ESP_LOGE("AliyunFace", "HTTP POST failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return response;
}

// 解析比对分数
float ParseAliyunFaceCompareScore(const std::string& response) {
    ESP_LOGI("AliyunFace", "Parsing response...");
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        ESP_LOGE("AliyunFace", "Failed to parse JSON response");
        return 0.0f;
    }
    cJSON* data = cJSON_GetObjectItem(root, "Data");
    float score = 0.0f;
    if (data) {
        cJSON* confidence = cJSON_GetObjectItem(data, "Confidence");
        if (cJSON_IsNumber(confidence)) {
            score = confidence->valuedouble;
            ESP_LOGI("AliyunFace", "Face compare score: %.2f", score);
        } else {
            ESP_LOGE("AliyunFace", "Confidence field not found or not a number");
        }
    } else {
        ESP_LOGE("AliyunFace", "Data field not found in response");
    }
    cJSON_Delete(root);
    return score;
}

// 对外接口：比对两张照片，返回分数
float AliyunFaceCompare(const std::string& imgA_base64, const std::string& imgB_base64) {
    ESP_LOGI("AliyunFace", "Start face compare...");
    std::string response = SendAliyunFaceCompareRequestStrict(imgA_base64, imgB_base64);
    return ParseAliyunFaceCompareScore(response);
}

// 遍历SPIFFS照片库，与内存照片比对，返回匹配文件名
std::string CompareCapturedFaceWithSpiffs(const std::string& captured_base64) {
    ESP_LOGI("AliyunFace", "Start SPIFFS face compare...");
    DIR* dir = opendir("/spiffs");
    if (!dir) {
        ESP_LOGE("AliyunFace", "Failed to open /spiffs directory");
        return "";
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename.find(".jpg") == std::string::npos && filename.find(".jpeg") == std::string::npos) continue;
        std::string path = "/spiffs/" + filename;
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            ESP_LOGW("AliyunFace", "Failed to open file: %s", path.c_str());
            continue;
        }
        std::vector<unsigned char> buffer(std::istreambuf_iterator<char>(file), {});
        std::string db_base64 = base64_encode(buffer.data(), buffer.size());

        ESP_LOGI("AliyunFace", "Comparing with photo: %s", filename.c_str());
        float score = AliyunFaceCompare(captured_base64, db_base64);

        if (score > 80.0f) { // 阈值可调整
            ESP_LOGI("AliyunFace", "Matched photo: %s (score: %.2f)", filename.c_str(), score);
            closedir(dir);
            return filename;
        }
    }
    closedir(dir);
    ESP_LOGI("AliyunFace", "No matched photo found");
    return "";
}
std::string Application::whoareyou() {
    ESP_LOGI("AliyunFace", "Capturing photo for face recognition...");
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb || !fb->buf || fb->len == 0) {
        ESP_LOGE("AliyunFace", "Camera capture failed");
        Alert(Lang::Strings::ERROR, "Camera capture failed", "sad", Lang::Sounds::P3_EXCLAMATION);
        return "";
    }
    std::string captured_base64 = base64_encode(fb->buf, fb->len);
    esp_camera_fb_return(fb);

    // 在阿里云人脸数据库中搜索
    std::string response = SearchFaceInAliyunDB(captured_base64);
    std::string matched_person = ParseSearchFaceResult(response);
    
    if (!matched_person.empty()) {
        ESP_LOGI("AliyunFace", "Final matched person: %s", matched_person.c_str());
        Alert(Lang::Strings::INFO, ("识别到人脸：" + matched_person).c_str(), "happy", Lang::Sounds::P3_SUCCESS);
        return matched_person;
    } else {
        ESP_LOGI("AliyunFace", "No matched person");
        Alert(Lang::Strings::INFO, "未识别到已知人脸", "sad", Lang::Sounds::P3_EXCLAMATION);
        return "";
    }
}
void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio codec */
    auto codec = board.GetAudioCodec();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(codec->output_sample_rate(), 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_ = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
    opus_encoder_->SetComplexity(0);
    if (aec_mode_ != kAecOff) {
        ESP_LOGI(TAG, "AEC mode: %d, setting opus encoder complexity to 0", aec_mode_);
        opus_encoder_->SetComplexity(0);
    } else {
#if CONFIG_USE_AUDIO_PROCESSOR
        ESP_LOGI(TAG, "Audio processor detected, setting opus encoder complexity to 5");
        opus_encoder_->SetComplexity(5);
#else
        ESP_LOGI(TAG, "Audio processor not detected, setting opus encoder complexity to 0");
        opus_encoder_->SetComplexity(0);
#endif
    }

    if (codec->input_sample_rate() != 16000) {
        input_resampler_.Configure(codec->input_sample_rate(), 16000);
        reference_resampler_.Configure(codec->input_sample_rate(), 16000);
    }
    codec->Start();

#if CONFIG_USE_AUDIO_PROCESSOR
    xTaskCreatePinnedToCore([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_, 1);
#else
    xTaskCreate([](void* arg) {
        Application* app = (Application*)arg;
        app->AudioLoop();
        vTaskDelete(NULL);
    }, "audio_loop", 4096 * 2, this, 8, &audio_loop_task_handle_);
#endif

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
#if CONFIG_IOT_PROTOCOL_MCP
    McpServer::GetInstance().AddCommonTools();
#endif

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnNetworkError([this](const std::string& message) {
        SetDeviceState(kDeviceStateIdle);
        Alert(Lang::Strings::ERROR, message.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
    });
    protocol_->OnIncomingAudio([this](AudioStreamPacket&& packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (device_state_ == kDeviceStateSpeaking && audio_decode_queue_.size() < MAX_AUDIO_PACKETS_IN_QUEUE) {
            audio_decode_queue_.emplace_back(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }

#if CONFIG_IOT_PROTOCOL_XIAOZHI
        auto& thing_manager = iot::ThingManager::GetInstance();
        protocol_->SendIotDescriptors(thing_manager.GetDescriptorsJson());
        std::string states;
        if (thing_manager.GetStatesJson(states, false)) {
            protocol_->SendIotStates(states);
        }
#endif
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
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    background_task_->WaitForCompletion();
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
#if CONFIG_IOT_PROTOCOL_MCP
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
#endif
#if CONFIG_IOT_PROTOCOL_XIAOZHI
        } else if (strcmp(type->valuestring, "iot") == 0) {
            auto commands = cJSON_GetObjectItem(root, "commands");
            if (cJSON_IsArray(commands)) {
                auto& thing_manager = iot::ThingManager::GetInstance();
                for (int i = 0; i < cJSON_GetArraySize(commands); ++i) {
                    auto command = cJSON_GetArrayItem(commands, i);
                    thing_manager.Invoke(command);
                }
            }
#endif
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::P3_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    audio_debugger_ = std::make_unique<AudioDebugger>();
    audio_processor_->Initialize(codec);
    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (audio_send_queue_.size() >= MAX_AUDIO_PACKETS_IN_QUEUE) {
                ESP_LOGW(TAG, "Too many audio packets in queue, drop the newest packet");
                return;
            }
        }
        background_task_->Schedule([this, data = std::move(data)]() mutable {
            opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                AudioStreamPacket packet;
                packet.payload = std::move(opus);
#ifdef CONFIG_USE_SERVER_AEC
                {
                    std::lock_guard<std::mutex> lock(timestamp_mutex_);
                    if (!timestamp_queue_.empty()) {
                        packet.timestamp = timestamp_queue_.front();
                        timestamp_queue_.pop_front();
                    } else {
                        packet.timestamp = 0;
                    }

                    if (timestamp_queue_.size() > 3) { // 限制队列长度3
                        timestamp_queue_.pop_front(); // 该包发送前先出队保持队列长度
                        return;
                    }
                }
#endif
                std::lock_guard<std::mutex> lock(mutex_);
                if (audio_send_queue_.size() >= MAX_AUDIO_PACKETS_IN_QUEUE) {
                    ESP_LOGW(TAG, "Too many audio packets in queue, drop the oldest packet");
                    audio_send_queue_.pop_front();
                }
                audio_send_queue_.emplace_back(std::move(packet));
                xEventGroupSetBits(event_group_, SEND_AUDIO_EVENT);
            });
        });
    });
    audio_processor_->OnVadStateChange([this](bool speaking) {
        if (device_state_ == kDeviceStateListening) {
            Schedule([this, speaking]() {
                if (speaking) {
                    voice_detected_ = true;
                } else {
                    voice_detected_ = false;
                }
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            });
        }
    });

    wake_word_->Initialize(codec);
    wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
        Schedule([this, &wake_word]() {
            if (!protocol_) {
                return;
            }

            if (device_state_ == kDeviceStateIdle) {
                wake_word_->EncodeWakeWordData();

                if (!protocol_->IsAudioChannelOpened()) {
                    SetDeviceState(kDeviceStateConnecting);
                    if (!protocol_->OpenAudioChannel()) {
                        wake_word_->StartDetection();
                        return;
                    }
                }

                ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD
                AudioStreamPacket packet;
                // Encode and send the wake word data to the server
                while (wake_word_->GetWakeWordOpus(packet.payload)) {
                    protocol_->SendAudio(packet);
                }
                // Set the chat state to wake word detected
                protocol_->SendWakeWordDetected(wake_word);
#else
                // Play the pop up sound to indicate the wake word is detected
                // And wait 60ms to make sure the queue has been processed by audio task
                ResetDecoder();
                PlaySound(Lang::Sounds::P3_POPUP);
                vTaskDelay(pdMS_TO_TICKS(60));
#endif
                SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
            } else if (device_state_ == kDeviceStateSpeaking) {
                AbortSpeaking(kAbortReasonWakeWordDetected);
            } else if (device_state_ == kDeviceStateActivating) {
                SetDeviceState(kDeviceStateIdle);
            }

            std::string who = whoareyou(); // 自动拍照比对人脸
        });
    });
    wake_word_->StartDetection();

    // Wait for the new version check to finish
    xEventGroupWaitBits(event_group_, CHECK_NEW_VERSION_DONE_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        // Play the success sound to indicate the device is ready
        ResetDecoder();
        PlaySound(Lang::Sounds::P3_SUCCESS);
    }

    // Print heap stats
    SystemInfo::PrintHeapStats();
    
    // Enter the main event loop
    MainEventLoop();
}

void Application::OnClockTimer() {
    clock_ticks_++;

    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar();

    // Print the debug info every 10 seconds
    if (clock_ticks_ % 10 == 0) {
        // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
        // SystemInfo::PrintTaskList();
        SystemInfo::PrintHeapStats();

        // If we have synchronized server time, set the status to clock "HH:MM" if the device is idle
        if (has_server_time_) {
            if (device_state_ == kDeviceStateIdle) {
                Schedule([this]() {
                    // Set status to clock "HH:MM"
                    time_t now = time(NULL);
                    char time_str[64];
                    strftime(time_str, sizeof(time_str), "%H:%M  ", localtime(&now));
                    Board::GetInstance().GetDisplay()->SetStatus(time_str);
                });
            }
        }
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, SCHEDULE_EVENT);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    // Raise the priority of the main event loop to avoid being interrupted by background tasks (which has priority 2)
    vTaskPrioritySet(NULL, 3);

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, SCHEDULE_EVENT | SEND_AUDIO_EVENT, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & SEND_AUDIO_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto packets = std::move(audio_send_queue_);
            lock.unlock();
            for (auto& packet : packets) {
                if (!protocol_->SendAudio(packet)) {
                    break;
                }
            }
        }

        if (bits & SCHEDULE_EVENT) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
    }
}

// The Audio Loop is used to input and output audio data
void Application::AudioLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    while (true) {
        OnAudioInput();
        if (codec->output_enabled()) {
            OnAudioOutput();
        }
    }
}

void Application::OnAudioOutput() {
    if (busy_decoding_audio_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int max_silence_seconds = 10;

    std::unique_lock<std::mutex> lock(mutex_);
    if (audio_decode_queue_.empty()) {
        // Disable the output if there is no audio data for a long time
        if (device_state_ == kDeviceStateIdle) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_output_time_).count();
            if (duration > max_silence_seconds) {
                codec->EnableOutput(false);
            }
        }
        return;
    }

    auto packet = std::move(audio_decode_queue_.front());
    audio_decode_queue_.pop_front();
    lock.unlock();
    audio_decode_cv_.notify_all();

    // Synchronize the sample rate and frame duration
    SetDecodeSampleRate(packet.sample_rate, packet.frame_duration);

    busy_decoding_audio_ = true;
    if (!background_task_->Schedule([this, codec, packet = std::move(packet)]() mutable {
        busy_decoding_audio_ = false;
        if (aborted_) {
            return;
        }

        std::vector<int16_t> pcm;
        if (!opus_decoder_->Decode(std::move(packet.payload), pcm)) {
            return;
        }
        // Resample if the sample rate is different
        if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
            int target_size = output_resampler_.GetOutputSamples(pcm.size());
            std::vector<int16_t> resampled(target_size);
            output_resampler_.Process(pcm.data(), pcm.size(), resampled.data());
            pcm = std::move(resampled);
        }
        codec->OutputData(pcm);
#ifdef CONFIG_USE_SERVER_AEC
        std::lock_guard<std::mutex> lock(timestamp_mutex_);
        timestamp_queue_.push_back(packet.timestamp);
#endif
        last_output_time_ = std::chrono::steady_clock::now();
    })) {
        busy_decoding_audio_ = false;
    }
}

void Application::OnAudioInput() {
    if (device_state_ == kDeviceStateAudioTesting) {
        if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
            ExitAudioTestingMode();
            return;
        }
        std::vector<int16_t> data;
        int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
        if (ReadAudio(data, 16000, samples)) {
            background_task_->Schedule([this, data = std::move(data)]() mutable {
                opus_encoder_->Encode(std::move(data), [this](std::vector<uint8_t>&& opus) {
                    AudioStreamPacket packet;
                    packet.payload = std::move(opus);
                    packet.frame_duration = OPUS_FRAME_DURATION_MS;
                    packet.sample_rate = 16000;
                    std::lock_guard<std::mutex> lock(mutex_);
                    audio_testing_queue_.push_back(std::move(packet));
                });
            });
            return;
        }
    }

    if (wake_word_->IsDetectionRunning()) {
        std::vector<int16_t> data;
        int samples = wake_word_->GetFeedSize();
        if (samples > 0) {
            if (ReadAudio(data, 16000, samples)) {
                wake_word_->Feed(data);
                return;
            }
        }
    }

    if (audio_processor_->IsRunning()) {
        std::vector<int16_t> data;
        int samples = audio_processor_->GetFeedSize();
        if (samples > 0) {
            if (ReadAudio(data, 16000, samples)) {
                audio_processor_->Feed(data);
                return;
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(OPUS_FRAME_DURATION_MS / 2));
}

bool Application::ReadAudio(std::vector<int16_t>& data, int sample_rate, int samples) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec->input_enabled()) {
        return false;
    }

    if (codec->input_sample_rate() != sample_rate) {
        data.resize(samples * codec->input_sample_rate() / sample_rate);
        if (!codec->InputData(data)) {
            return false;
        }
        if (codec->input_channels() == 2) {
            auto mic_channel = std::vector<int16_t>(data.size() / 2);
            auto reference_channel = std::vector<int16_t>(data.size() / 2);
            for (size_t i = 0, j = 0; i < mic_channel.size(); ++i, j += 2) {
                mic_channel[i] = data[j];
                reference_channel[i] = data[j + 1];
            }
            auto resampled_mic = std::vector<int16_t>(input_resampler_.GetOutputSamples(mic_channel.size()));
            auto resampled_reference = std::vector<int16_t>(reference_resampler_.GetOutputSamples(reference_channel.size()));
            input_resampler_.Process(mic_channel.data(), mic_channel.size(), resampled_mic.data());
            reference_resampler_.Process(reference_channel.data(), reference_channel.size(), resampled_reference.data());
            data.resize(resampled_mic.size() + resampled_reference.size());
            for (size_t i = 0, j = 0; i < resampled_mic.size(); ++i, j += 2) {
                data[j] = resampled_mic[i];
                data[j + 1] = resampled_reference[i];
            }
        } else {
            auto resampled = std::vector<int16_t>(input_resampler_.GetOutputSamples(data.size()));
            input_resampler_.Process(data.data(), data.size(), resampled.data());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples);
        if (!codec->InputData(data)) {
            return false;
        }
    }
    
    // 音频调试：发送原始音频数据
    if (audio_debugger_) {
        audio_debugger_->Feed(data);
    }
    
    return true;
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
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
    // The state is changed, wait for all background tasks to finish
    background_task_->WaitForCompletion();

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_processor_->Stop();
            wake_word_->StartDetection();
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            timestamp_queue_.clear();
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            // Update the IoT states before sending the start listening command
#if CONFIG_IOT_PROTOCOL_XIAOZHI
            UpdateIotStates();
#endif

            // Make sure the audio processor is running
            if (!audio_processor_->IsRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                if (previous_state == kDeviceStateSpeaking) {
                    audio_decode_queue_.clear();
                    audio_decode_cv_.notify_all();
                    // FIXME: Wait for the speaker to empty the buffer
                    vTaskDelay(pdMS_TO_TICKS(120));
                }
                opus_encoder_->ResetState();
                audio_processor_->Start();
                wake_word_->StopDetection();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_processor_->Stop();
                // Only AFE wake word can be detected in speaking mode
#if CONFIG_USE_AFE_WAKE_WORD
                wake_word_->StartDetection();
#else
                wake_word_->StopDetection();
#endif
            }
            ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::ResetDecoder() {
    std::lock_guard<std::mutex> lock(mutex_);
    opus_decoder_->ResetState();
    audio_decode_queue_.clear();
    audio_decode_cv_.notify_all();
    last_output_time_ = std::chrono::steady_clock::now();
    auto codec = Board::GetInstance().GetAudioCodec();
    codec->EnableOutput(true);
}

void Application::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (opus_decoder_->sample_rate() == sample_rate && opus_decoder_->duration_ms() == frame_duration) {
        return;
    }

    opus_decoder_.reset();
    opus_decoder_ = std::make_unique<OpusDecoderWrapper>(sample_rate, 1, frame_duration);

    auto codec = Board::GetInstance().GetAudioCodec();
    if (opus_decoder_->sample_rate() != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", opus_decoder_->sample_rate(), codec->output_sample_rate());
        output_resampler_.Configure(opus_decoder_->sample_rate(), codec->output_sample_rate());
    }
}

void Application::UpdateIotStates() {
#if CONFIG_IOT_PROTOCOL_XIAOZHI
    auto& thing_manager = iot::ThingManager::GetInstance();
    std::string states;
    if (thing_manager.GetStatesJson(states, true)) {
        protocol_->SendIotStates(states);
    }
#endif
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) {
        ToggleChatState();
        Schedule([this, wake_word]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            }
        }); 
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

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_processor_->EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_processor_->EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_processor_->EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}
