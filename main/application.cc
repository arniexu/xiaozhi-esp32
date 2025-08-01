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
        if(display)
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
            if(display)
            display->SetIcon(FONT_AWESOME_DOWNLOAD);
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota.GetFirmwareVersion();
            if(display)
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
                if(display)
                display->SetChatMessage("system", buffer);
            });

            // If upgrade success, the device will reboot and never reach here
            if(display)
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

        if(display)
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
    if(display)
    {
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    }
    if (!sound.empty()) {
        ResetDecoder();
        PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        if(display)
        {
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
        }
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

#include "mbedtls/md.h"
#include <ctime>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <dirent.h>
#include "espcurl.h"
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
// URL编码

std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex << std::uppercase; // 强制十六进制为大写

    for (unsigned char c : value) {
        // 保留字符：字母、数字、- _ . ~
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            // 其他字符按%XX格式编码
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }

    return escaped.str();
}

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

// 服务器上传地址（根据实际情况修改）
#define UPLOAD_URL "http://47.110.233.243:8003/api/upload/image"

/**
 * 通过HTTP发送照片
 * @param image_data 图像数据缓冲区
 * @param image_len 图像数据长度
 * @param filename 文件名
 * @return ESP_OK 成功，其他失败
 */

std::string Application::UploadImageToHttp(camera_fb_t* fb, const std::string& filename) {
    struct AudioGuard {
        decltype(audio_processor_)& proc;
        AudioGuard(decltype(audio_processor_)& p) : proc(p) { if (proc) proc->Stop(); }
        ~AudioGuard() { if (proc) proc->Start(); }
    } audio_guard(audio_processor_);

    if (!fb || !fb->buf || fb->len == 0 || filename.empty()) {
        ESP_LOGE("UploadImageToHttp", "无效参数: 图像数据或文件名为空");
        return "";
    }

    ESP_LOGI("UploadImageToHttp", "准备上传图片: %s (大小: %u 字节)", filename.c_str(), fb->len);

    // 分配header/body缓冲区
    char *hdrbuf = (char*)calloc(1024, 1);
    char *bodybuf = (char*)calloc(4096, 1);
    if (!hdrbuf || !bodybuf) {
        ESP_LOGE("UploadImageToHttp", "内存分配失败");
        if (hdrbuf) free(hdrbuf);
        if (bodybuf) free(bodybuf);
        return "";
    }

    // 构造multipart参数，image字段，直接用内存数据
    extern struct curl_httppost *formpost;
    extern struct curl_httppost *lastptr;
    formpost = NULL;
    lastptr = NULL;
    // 传递图片buffer
    curl_formadd(&formpost, &lastptr,
        CURLFORM_COPYNAME, "image",
        CURLFORM_BUFFER, filename.c_str(),
        CURLFORM_BUFFERPTR, fb->buf,
        CURLFORM_BUFFERLENGTH, (long)fb->len,
        CURLFORM_CONTENTTYPE, "image/jpeg",
        CURLFORM_END);

    // 可选：添加额外参数
    // curl_formadd(&formpost, &lastptr, CURLFORM_COPYNAME, "param1", CURLFORM_COPYCONTENTS, "value", CURLFORM_END);

    int res = Curl_POST((char*)UPLOAD_URL, hdrbuf, bodybuf, 1024, 4096);
    std::string result_path;
    if (res == 0) {
        cJSON* root = cJSON_Parse(bodybuf);
        if (root) {
            cJSON* data = cJSON_GetObjectItem(root, "data");
            if (data && cJSON_IsObject(data)) {
                cJSON* file_path = cJSON_GetObjectItem(data, "file_path");
                if (file_path && cJSON_IsString(file_path)) {
                    result_path = file_path->valuestring;
                    ESP_LOGI("UploadImageToHttp", "图片已上传，服务器路径: %s", result_path.c_str());
                }
            }
            cJSON_Delete(root);
        }
    } else {
        ESP_LOGE("UploadImageToHttp", "Curl_POST 上传失败: %d", res);
    }
    free(hdrbuf);
    free(bodybuf);
    return result_path;
}

// WebSocket人脸识别函数
std::string Application::CreateFaceDB(const std::string& db_name) {
    ESP_LOGI("FaceRec", "=== Creating Face DB ===");
    ESP_LOGI("FaceRec", "DB name: %s", db_name.c_str());
    
    if (!protocol_) {
        ESP_LOGE("FaceRec", "Protocol not initialized");
        return "";
    }

    cJSON* request = cJSON_CreateObject();
    cJSON* payload = cJSON_CreateObject();
    
    cJSON_AddStringToObject(request, "type", "face_api");
    cJSON_AddStringToObject(payload, "action", "create_face_db");
    cJSON_AddStringToObject(payload, "db_name", db_name.c_str());
    cJSON_AddItemToObject(request, "payload", payload);
    
    char* json_string = cJSON_Print(request);
    ESP_LOGI("FaceRec", "Request JSON: %s", json_string);
    
    std::string response = SendFaceApiRequest(json_string);
    
    ESP_LOGI("FaceRec", "Response: %s", response.c_str());
    ESP_LOGI("FaceRec", "=== Create Face DB Complete ===");
    
    free(json_string);
    cJSON_Delete(request);
    return response;
}

std::string Application::ListFacesInAliyunDB() {
    ESP_LOGI("FaceRec", "=== Listing Faces in DB ===");
    
    if (!protocol_) {
        ESP_LOGE("FaceRec", "Protocol not initialized");
        return "";
    }

    cJSON* request = cJSON_CreateObject();
    cJSON* payload = cJSON_CreateObject();
    
    cJSON_AddStringToObject(request, "type", "face_api");
    cJSON_AddStringToObject(payload, "action", "list_faces");
    cJSON_AddNumberToObject(payload, "limit", 100);
    cJSON_AddNumberToObject(payload, "offset", 0);
    cJSON_AddItemToObject(request, "payload", payload);
    
    char* json_string = cJSON_Print(request);
    
    // 🔥 详细打印WebSocket请求构造信息
    ESP_LOGI("FaceRec", "=== Constructing List Faces WebSocket Request ===");
    ESP_LOGI("FaceRec", "Request Type: face_api");
    ESP_LOGI("FaceRec", "Action: list_faces");
    ESP_LOGI("FaceRec", "Limit: 100");
    ESP_LOGI("FaceRec", "Offset: 0");
    ESP_LOGI("FaceRec", "Complete Request JSON: %s", json_string);
    ESP_LOGI("FaceRec", "=== Sending List Faces Request to WebSocket ===");
    
    std::string response = SendFaceApiRequest(json_string);
    
    ESP_LOGI("FaceRec", "Response length: %d bytes", response.length());
    ESP_LOGI("FaceRec", "Response: %s", response.c_str());
    ESP_LOGI("FaceRec", "=== List Faces Complete ===");
    
    free(json_string);
    cJSON_Delete(request);
    return response;
}

// 修改AddFaceToAliyunDB函数签名和实现
std::string Application::AddFaceToAliyunDB(const std::string& person_name, camera_fb_t* fb) {
    ESP_LOGI("FaceRec", "=== Adding Face to DB ===");
    ESP_LOGI("FaceRec", "Person name: %s", person_name.c_str());
    ESP_LOGI("FaceRec", "Image size: %d bytes", fb->len);
    
    if (!protocol_) {
        ESP_LOGE("FaceRec", "Protocol not initialized");
        return "";
    }

    if (person_name.empty()) {
        ESP_LOGE("FaceRec", "Person name is empty");
        return "";
    }

    // 生成唯一文件名
    uint64_t timestamp = esp_timer_get_time();
    std::string filename = "face_" + person_name + "_" + std::to_string(timestamp) + ".jpg";
    
    // 🔥 直接上传原始JPEG图片到HTTP服务器
    std::string image_path = UploadImageToHttp(fb, filename);  // 🔥 变量名改为 image_path
    if (image_path.empty()) {
        ESP_LOGE("FaceRec", "Failed to upload image to HTTP server");
        Alert(Lang::Strings::ERROR, "图片上传失败", "sad", Lang::Sounds::P3_EXCLAMATION);
        return "";
    }

    // 构建WebSocket请求
    cJSON* request = cJSON_CreateObject();
    cJSON* payload = cJSON_CreateObject();
    
    cJSON_AddStringToObject(request, "type", "face_api");
    cJSON_AddStringToObject(payload, "action", "add_face");
    cJSON_AddStringToObject(payload, "person_name", person_name.c_str());
    // 🔥 修改：使用 image_path 而不是 image_url
    cJSON_AddStringToObject(payload, "image_path", image_path.c_str());
    cJSON_AddItemToObject(request, "payload", payload);
    
    char* json_string = cJSON_Print(request);
    
    // 🔥 详细打印WebSocket请求构造信息
    ESP_LOGI("FaceRec", "=== Constructing Add Face WebSocket Request ===");
    ESP_LOGI("FaceRec", "Request Type: face_api");
    ESP_LOGI("FaceRec", "Action: add_face");
    ESP_LOGI("FaceRec", "Person Name: %s", person_name.c_str());
    ESP_LOGI("FaceRec", "Image Path: %s", image_path.c_str());
    ESP_LOGI("FaceRec", "Complete Request JSON: %s", json_string);
    ESP_LOGI("FaceRec", "=== Sending Add Face Request to WebSocket ===");
    
    std::string response = SendFaceApiRequest(json_string);
    
    ESP_LOGI("FaceRec", "Response: %s", response.c_str());
    ESP_LOGI("FaceRec", "=== Add Face Complete ===");
    
    free(json_string);
    cJSON_Delete(request);
    return response;
}

std::string Application::SearchFaceInAliyunDB(camera_fb_t* fb) {
    ESP_LOGI("FaceRec", "=== Searching Face in DB ===");
    ESP_LOGI("FaceRec", "Image size: %d bytes", fb->len);
    
    if (!protocol_) {
        ESP_LOGE("FaceRec", "Protocol not initialized");
        return "";
    }

    // 生成临时文件名用于搜索
    uint64_t timestamp = esp_timer_get_time();
    std::string filename = "search_" + std::to_string(timestamp) + ".jpg";
    
    // 🔥 直接上传原始JPEG图片到HTTP服务器
    std::string image_path = UploadImageToHttp(fb, filename);  // 🔥 变量名改为 image_path
    if (image_path.empty()) {
        ESP_LOGE("FaceRec", "Failed to upload search image to HTTP server");
        Alert(Lang::Strings::ERROR, "搜索图片上传失败", "sad", Lang::Sounds::P3_EXCLAMATION);
        return "";
    }

    // 构建WebSocket请求
    cJSON* request = cJSON_CreateObject();
    cJSON* payload = cJSON_CreateObject();
    
    cJSON_AddStringToObject(request, "type", "face_api");
    cJSON_AddStringToObject(payload, "action", "search_face");
    // 🔥 修改：使用 image_path 而不是 image_url
    cJSON_AddStringToObject(payload, "image_path", image_path.c_str());
    cJSON_AddNumberToObject(payload, "limit", 5);
    cJSON_AddNumberToObject(payload, "threshold", 80.0);
    cJSON_AddItemToObject(request, "payload", payload);
    
    char* json_string = cJSON_Print(request);
    
    // 🔥 详细打印WebSocket请求构造信息
    ESP_LOGI("FaceRec", "=== Constructing Search Face WebSocket Request ===");
    ESP_LOGI("FaceRec", "Request Type: face_api");
    ESP_LOGI("FaceRec", "Action: search_face");
    ESP_LOGI("FaceRec", "Image Path: %s", image_path.c_str());
    ESP_LOGI("FaceRec", "Search Limit: 5");
    ESP_LOGI("FaceRec", "Confidence Threshold: 80.0");
    ESP_LOGI("FaceRec", "Complete Request JSON: %s", json_string);
    ESP_LOGI("FaceRec", "=== Sending Search Face Request to WebSocket ===");
    
    std::string response = SendFaceApiRequest(json_string);
    
    ESP_LOGI("FaceRec", "Response: %s", response.c_str());
    ESP_LOGI("FaceRec", "=== Search Face Complete ===");
    
    free(json_string);
    cJSON_Delete(request);
    return response;
}

std::string Application::SendFaceApiRequest(const std::string& json_request) {
    ESP_LOGI("FaceRec", "=== SendFaceApiRequest START ===");
    
    if (!protocol_) {
        ESP_LOGE("FaceRec", "Protocol not initialized");
        return "";
    }

    uint64_t timestamp = esp_timer_get_time();
    uint32_t random = esp_random();
    std::string request_id = std::to_string(timestamp) + "_" + std::to_string(random);
    ESP_LOGI("FaceRec", "Generated request ID: %s", request_id.c_str());

    // 🔥 打印原始请求JSON
    ESP_LOGI("FaceRec", "Original request JSON: %s", json_request.c_str());

    cJSON* json_obj = cJSON_Parse(json_request.c_str());
    if (!json_obj) {
        ESP_LOGE("FaceRec", "Failed to parse JSON request");
        return "";
    }

    cJSON_AddStringToObject(json_obj, "request_id", request_id.c_str());
    char* updated_json = cJSON_Print(json_obj);
    
    // 🔥 详细打印WebSocket请求信息
    ESP_LOGI("FaceRec", "=== WebSocket Request Details ===");
    ESP_LOGI("FaceRec", "Request ID: %s", request_id.c_str());
    ESP_LOGI("FaceRec", "Final WebSocket JSON: %s", updated_json);
    ESP_LOGI("FaceRec", "JSON Length: %d bytes", strlen(updated_json));
    ESP_LOGI("FaceRec", "Sending to WebSocket protocol...");
    
    protocol_->SendFaceRecMessage(updated_json);  // 使用正确的函数
    
    free(updated_json);
    cJSON_Delete(json_obj);

    std::string response = WaitForFaceApiResponse(request_id, 30);
    
    if (response.empty()) {
        ESP_LOGE("FaceRec", "No response received");
    } else {
        ESP_LOGI("FaceRec", "Response received: %d bytes", response.length());
    }
    
    ESP_LOGI("FaceRec", "=== SendFaceApiRequest END ===");
    return response;
}

std::string Application::whoareyou() {
    ESP_LOGI("FaceRec", "=== WHO ARE YOU - Starting Face Recognition ===");
    ESP_LOGI("FaceRec", "Initializing camera capture...");

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE("FaceRec", "Camera frame buffer is NULL");
        Alert(Lang::Strings::ERROR, "Camera capture failed", "sad", Lang::Sounds::P3_EXCLAMATION);
        return "";
    }

    if (!fb->buf || fb->len == 0) {
        ESP_LOGE("FaceRec", "Camera buffer is invalid");
        esp_camera_fb_return(fb);
        Alert(Lang::Strings::ERROR, "Camera buffer error", "sad", Lang::Sounds::P3_EXCLAMATION);
        return "";
    }

    ESP_LOGI("FaceRec", "Camera capture successful:");
    ESP_LOGI("FaceRec", "  - Width: %d", fb->width);
    ESP_LOGI("FaceRec", "  - Height: %d", fb->height);
    ESP_LOGI("FaceRec", "  - Format: %d", fb->format);
    ESP_LOGI("FaceRec", "  - Buffer length: %d bytes", fb->len);

    // 🔥 直接使用相机帧缓冲区进行人脸搜索，无需base64编码
    ESP_LOGI("FaceRec", "Sending search request to backend server...");
    std::string response = SearchFaceInAliyunDB(fb);

    // 释放相机帧缓冲区
    esp_camera_fb_return(fb);
    ESP_LOGI("FaceRec", "Camera frame buffer returned");
    // camera_lock自动释放

    if (response.empty()) {
        ESP_LOGE("FaceRec", "No response from server");
        Alert(Lang::Strings::ERROR, "Server communication failed", "sad", Lang::Sounds::P3_EXCLAMATION);
        return "";
    }

    ESP_LOGI("FaceRec", "Parsing search response...");
    std::string matched_person = ParseSearchFaceResult(response);

    if (!matched_person.empty()) {
        ESP_LOGI("FaceRec", "SUCCESS: Matched person found: %s", matched_person.c_str());
        std::string alert_message = "识别到人脸：" + matched_person;
        Alert(Lang::Strings::INFO, alert_message.c_str(), "happy", Lang::Sounds::P3_SUCCESS);
        ESP_LOGI("FaceRec", "=== WHO ARE YOU - Face Recognition SUCCESS ===");
        return matched_person;
    } else {
        ESP_LOGI("FaceRec", "No matched person found in database");
        Alert(Lang::Strings::INFO, "未识别到已知人脸", "sad", Lang::Sounds::P3_EXCLAMATION);
        ESP_LOGI("FaceRec", "=== WHO ARE YOU - Face Recognition FAILED ===");
        return "";
    }
}

// 解析函数
std::string Application::ParseListFacesResult(const std::string& response) {
    ESP_LOGI("FaceRec", "=== ParseListFacesResult START ===");
    ESP_LOGD("FaceRec", "Response to parse: %s", response.c_str());
    
    if (response.empty()) {
        ESP_LOGE("FaceRec", "Empty response string");
        return "";
    }
    
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        ESP_LOGE("FaceRec", "Failed to parse JSON response");
        return "";
    }
    
    cJSON* success = cJSON_GetObjectItem(root, "success");
    if (!cJSON_IsBool(success) || !cJSON_IsTrue(success)) {
        ESP_LOGE("FaceRec", "API request failed");
        cJSON* error = cJSON_GetObjectItem(root, "error");
        if (cJSON_IsString(error)) {
            ESP_LOGE("FaceRec", "API error: %s", error->valuestring);
        }
        cJSON_Delete(root);
        return "";
    }
    
    cJSON* data = cJSON_GetObjectItem(root, "data");
    cJSON* faces = cJSON_GetObjectItem(data, "faces");
    if (!cJSON_IsArray(faces)) {
        ESP_LOGI("FaceRec", "No faces found in database");
        cJSON_Delete(root);
        return "";
    }
    
    std::vector<std::string> person_names;
    int faces_count = cJSON_GetArraySize(faces);
    
    for (int i = 0; i < faces_count; i++) {
        cJSON* face = cJSON_GetArrayItem(faces, i);
        if (face) {
            cJSON* person_name = cJSON_GetObjectItem(face, "person_name");
            if (cJSON_IsString(person_name)) {
                std::string name = person_name->valuestring;
                if (std::find(person_names.begin(), person_names.end(), name) == person_names.end()) {
                    person_names.push_back(name);
                    ESP_LOGI("FaceRec", "Found person: %s", name.c_str());
                }
            }
        }
    }
    
    cJSON_Delete(root);
    
    std::string result;
    for (size_t i = 0; i < person_names.size(); ++i) {
        result += person_names[i];
        if (i != person_names.size() - 1) {
            result += ",";
        }
    }
    
    ESP_LOGI("FaceRec", "Total unique persons found: %zu", person_names.size());
    return result;
}

std::string Application::ParseSearchFaceResult(const std::string& response) {
    ESP_LOGI("FaceRec", "=== ParseSearchFaceResult START ===");
    ESP_LOGD("FaceRec", "Response to parse: %s", response.c_str());
    
    if (response.empty()) {
        ESP_LOGE("FaceRec", "Empty response string");
        return "";
    }
    
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        ESP_LOGE("FaceRec", "Failed to parse JSON response");
        return "";
    }
    
    cJSON* success = cJSON_GetObjectItem(root, "success");
    if (!cJSON_IsBool(success) || !cJSON_IsTrue(success)) {
        ESP_LOGE("FaceRec", "API request failed");
        cJSON* error = cJSON_GetObjectItem(root, "error");
        if (cJSON_IsString(error)) {
            ESP_LOGE("FaceRec", "API error: %s", error->valuestring);
        }
        cJSON_Delete(root);
        return "";
    }
    
    cJSON* data = cJSON_GetObjectItem(root, "data");
    cJSON* matches = cJSON_GetObjectItem(data, "matches");
    
    if (!cJSON_IsArray(matches) || cJSON_GetArraySize(matches) == 0) {
        ESP_LOGI("FaceRec", "No matches found");
        cJSON_Delete(root);
        return "";
    }
    
    // 获取第一个匹配结果
    cJSON* first_match = cJSON_GetArrayItem(matches, 0);
    if (first_match) {
        cJSON* person_name = cJSON_GetObjectItem(first_match, "person_name");
        cJSON* confidence = cJSON_GetObjectItem(first_match, "confidence");
        
        if (cJSON_IsString(person_name) && cJSON_IsNumber(confidence)) {
            float score = confidence->valuedouble;
            if (score > 80.0f) {
                std::string name = person_name->valuestring;
                ESP_LOGI("FaceRec", "Matched person: %s (confidence: %.2f)", name.c_str(), score);
                cJSON_Delete(root);
                return name;
            }
        }
    }
    
    cJSON_Delete(root);
    return "";
}

// 存储人脸API响应
void Application::StoreFaceApiResponse(const std::string& request_id, const std::string& response) {
    ESP_LOGI("FaceRec", "Storing response for request ID: %s", request_id.c_str());
    ESP_LOGD("FaceRec", "Response content: %s", response.c_str());
    
    {
        std::lock_guard<std::mutex> lock(face_api_mutex_);
        
        // 存储响应数据
        face_api_responses_[request_id] = response;
        
        ESP_LOGI("FaceRec", "Response stored successfully. Current pending responses: %zu", 
                 face_api_responses_.size());
    }
    
    // 通知所有等待的线程
    face_api_cv_.notify_all();
    ESP_LOGI("FaceRec", "Notified waiting threads for request ID: %s", request_id.c_str());
}

// 等待人脸API响应
std::string Application::WaitForFaceApiResponse(const std::string& request_id, int timeout_seconds) {
    ESP_LOGI("FaceRec", "Waiting for response with ID: %s (timeout: %ds)", 
             request_id.c_str(), timeout_seconds);
    
    std::unique_lock<std::mutex> lock(face_api_mutex_);
    
    // 使用条件变量等待响应
    bool received = face_api_cv_.wait_for(
        lock, 
        std::chrono::seconds(timeout_seconds),
        [this, &request_id] {
            // 检查是否收到了对应的响应
            bool found = face_api_responses_.find(request_id) != face_api_responses_.end();
            if (found) {
                ESP_LOGD("FaceRec", "Response found for request ID: %s", request_id.c_str());
            }
            return found;
        }
    );
    
    if (received) {
        // 获取响应并清理
        std::string response = face_api_responses_[request_id];
        face_api_responses_.erase(request_id);
        
        ESP_LOGI("FaceRec", "Response retrieved for request ID: %s, length: %d bytes", 
                 request_id.c_str(), response.length());
        ESP_LOGI("FaceRec", "Remaining pending responses: %zu", face_api_responses_.size());
        
        return response;
    } else {
        // 超时处理
        ESP_LOGE("FaceRec", "Timeout waiting for response with ID: %s after %d seconds", 
                 request_id.c_str(), timeout_seconds);
        
        // 清理可能的残留数据
        auto it = face_api_responses_.find(request_id);
        if (it != face_api_responses_.end()) {
            ESP_LOGW("FaceRec", "Found stale response for request ID: %s, cleaning up", request_id.c_str());
            face_api_responses_.erase(it);
        }
        
        return "";
    }
}

// 清理过期的响应（可选的辅助函数）
void Application::CleanupExpiredResponses() {
    std::lock_guard<std::mutex> lock(face_api_mutex_);
    
    if (face_api_responses_.size() > 10) {  // 如果积累太多响应
        ESP_LOGW("FaceRec", "Too many pending responses (%zu), clearing all", 
                 face_api_responses_.size());
        face_api_responses_.clear();
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
    if(display)
    display->UpdateStatusBar(true);

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    if(display)
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
            if(display)
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
                        if(display)
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    if(display)
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                                            if(display)
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
        } else if (strcmp(type->valuestring, "face_api_response") == 0) {
            ESP_LOGI("FaceRec", "=== Received face_api_response ===");
            
            auto request_id = cJSON_GetObjectItem(root, "request_id");
            auto payload = cJSON_GetObjectItem(root, "payload");
            
            if (cJSON_IsString(request_id) && cJSON_IsObject(payload)) {
                std::string response_id = request_id->valuestring;
                char* payload_str = cJSON_Print(payload);
                
                ESP_LOGI("FaceRec", "Processing response for request ID: %s", response_id.c_str());
                ESP_LOGD("FaceRec", "Payload: %s", payload_str);
                
                StoreFaceApiResponse(response_id, payload_str);
                
                free(payload_str);
            }
            ESP_LOGI("FaceRec", "=== face_api_response processed ===");
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
        if(display)
        {
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        }
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
    if(display)
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
                    const auto& display = Board::GetInstance().GetDisplay();
                    if(display)
                    display->SetStatus(time_str);
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
            if(display){
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            }
            audio_processor_->Stop();
            wake_word_->StartDetection();
            break;
        case kDeviceStateConnecting:
            if (display) {
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            }
            timestamp_queue_.clear();
            break;
        case kDeviceStateListening:
            if (display) {
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            }
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
            if (display)
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
            if (display)
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_processor_->EnableDeviceAec(false);
            if(display)
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_processor_->EnableDeviceAec(true);
            if(display)
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}
