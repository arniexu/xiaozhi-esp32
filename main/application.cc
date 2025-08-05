#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "font_awesome_symbols.h"
#include "assets/lang_config.h"
#include "mcp_server.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include "audio_processor.h"

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
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = Board::GetInstance().GetDisplay();
        if(display) {
            display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);
        }

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
            if(display) {
                display->SetIcon(FONT_AWESOME_DOWNLOAD);
            }
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota.GetFirmwareVersion();
            if(display) {
                display->SetChatMessage("system", message.c_str());
            }

            auto& board = Board::GetInstance();
            board.SetPowerSaveMode(false);
            audio_service_.Stop();
            vTaskDelay(pdMS_TO_TICKS(1000));

            bool upgrade_success = ota.StartUpgrade([display](int progress, size_t speed) {
                char buffer[64];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                if(display) {
                    display->SetChatMessage("system", buffer);
                }
            });

            if (!upgrade_success) {
                // Upgrade failed, restart audio service and continue running
                ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
                audio_service_.Start(); // Restart audio service
                board.SetPowerSaveMode(true); // Restore power save mode
                Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "sad", Lang::Sounds::P3_EXCLAMATION);
                vTaskDelay(pdMS_TO_TICKS(3000));
                // Continue to normal operation (don't break, just fall through)
            } else {
                // Upgrade success, reboot immediately
                ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
                if (display) {
                    display->SetChatMessage("system", "Upgrade successful, rebooting...");
                }
                vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
                Reboot();
                return; // This line will never be reached after reboot
            }
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        if(display) {
            display->SetStatus(Lang::Strings::ACTIVATION);
        }
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
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
            audio_service_.PlaySound(it->sound);
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
        audio_service_.PlaySound(sound);
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

#include "mbedtls/md.h"
#include <ctime>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
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



/**
 * 生成安全的文件名（只包含英文字母、数字、下划线和连字符）
 * @param base_name 文件名基础部分
 * @param suffix 文件名后缀（如 ".jpg"）
 * @return 安全的文件名
 */
std::string Application::GenerateSafeFilename(const std::string& base_name, const std::string& suffix) {
    uint64_t timestamp = esp_timer_get_time() / 1000;  // 转换为毫秒
    uint32_t random = esp_random();
    
    // 如果没有指定后缀，默认使用 .jpg
    std::string file_suffix = suffix.empty() ? ".jpg" : suffix;
    
    return base_name + "_" + std::to_string(timestamp) + "_" + std::to_string(random) + file_suffix;
}

/**
 * 通过FTP上传照片
 * @param fb 相机帧缓冲区
 * @param filename 文件名
 * @return 上传后的文件路径，失败返回空字符串
 */

std::string Application::UploadImageToFtp(camera_fb_t* fb, const std::string& filename) {

    if (!fb || !fb->buf || fb->len == 0 || filename.empty()) {
        ESP_LOGE("UploadImageToFtp", "无效参数: 图像数据或文件名为空");
        return "";
    }

    ESP_LOGI("UploadImageToFtp", "准备FTP上传图片: %s (大小: %u 字节)", filename.c_str(), fb->len);

    // 检查存储分区是否已挂载
    DIR* storage_dir = opendir("/storage");
    if (storage_dir) {
        closedir(storage_dir);
        ESP_LOGI("UploadImageToFtp", "✅ /storage 目录可访问");
    } else {
        ESP_LOGW("UploadImageToFtp", "⚠️ /storage 目录不可访问, errno: %d (%s)", errno, strerror(errno));
    }

    // 使用基于文件的上传方式 - 优先使用已挂载的 /storage 分区
    std::string temp_file;
    FILE* file = nullptr;
    
    // 首先尝试使用 /storage 分区（SPIFFS）
    const std::string storage_path = "/storage/" + filename;
    ESP_LOGI("UploadImageToFtp", "尝试创建文件: %s", storage_path.c_str());
    file = fopen(storage_path.c_str(), "wb");
    if (file) {
        temp_file = storage_path;
        ESP_LOGI("UploadImageToFtp", "✅ 成功创建临时文件: %s", temp_file.c_str());
    } else {
        ESP_LOGW("UploadImageToFtp", "❌ /storage 路径失败 (errno: %d, %s), 尝试其他路径...", errno, strerror(errno));
        
        // 备用路径策略
        const std::vector<std::string> temp_paths = {
            "/tmp/" + filename,           // RAM 临时文件系统
            "/data/" + filename,          // 数据分区
            "/cache/" + filename          // 缓存目录
        };
        
        for (const auto& path : temp_paths) {
            ESP_LOGI("UploadImageToFtp", "尝试创建文件: %s", path.c_str());
            file = fopen(path.c_str(), "wb");
            if (file) {
                temp_file = path;
                ESP_LOGI("UploadImageToFtp", "✅ 成功创建临时文件: %s", temp_file.c_str());
                break;
            }
            ESP_LOGW("UploadImageToFtp", "❌ 路径失败: %s (errno: %d, %s)", path.c_str(), errno, strerror(errno));
        }
    }
    
    if (!file) {
        ESP_LOGE("UploadImageToFtp", "❌ 无法创建临时文件，上传失败 (最后错误: %s)", strerror(errno));
        return "";
    }
    
    // 写入图片数据到临时文件
    size_t written = fwrite(fb->buf, 1, fb->len, file);
    fclose(file);
    
    if (written != fb->len) {
        ESP_LOGE("UploadImageToFtp", "❌ 写入临时文件失败: 期望 %d 字节，实际写入 %d 字节", fb->len, written);
        remove(temp_file.c_str());
        return "";
    }
    
    ESP_LOGI("UploadImageToFtp", "✅ 图片数据已写入临时文件: %d 字节", written);

    // FTP上传配置 - 直接使用文件名，不需要路径前缀
    #define FTP_SERVER "ftp://47.110.233.243/" 
    #define FTP_USER_PASS "xuqianjin:123"  // 请修改为实际的FTP用户名密码
    
    std::string ftp_url = std::string(FTP_SERVER) + filename;
    ESP_LOGI("UploadImageToFtp", "FTP URL: %s", ftp_url.c_str());
    
    // 分配缓冲区 - 使用堆分配而不是栈分配以节省栈空间
    char *hdrbuf = (char*)heap_caps_calloc(1024, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *bodybuf = (char*)heap_caps_calloc(2048, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    // 如果PSRAM分配失败，回退到内部RAM
    if (!hdrbuf) hdrbuf = (char*)calloc(1024, 1);
    if (!bodybuf) bodybuf = (char*)calloc(2048, 1);
    
    if (!hdrbuf || !bodybuf) {
        ESP_LOGE("UploadImageToFtp", "内存分配失败");
        if (hdrbuf) free(hdrbuf);
        if (bodybuf) free(bodybuf);
        if (!temp_file.empty()) remove(temp_file.c_str());
        return "";
    }

    // 使用FTP上传文件
    ESP_LOGI("UploadImageToFtp", "开始FTP上传...");
    ESP_LOGI("UploadImageToFtp", "FTP服务器: 47.110.233.243");
    ESP_LOGI("UploadImageToFtp", "用户名: xuqianjin");
    ESP_LOGI("UploadImageToFtp", "本地文件: %s", temp_file.c_str());
    ESP_LOGI("UploadImageToFtp", "远程文件名: %s", filename.c_str());
    
    // 尝试FTP上传，如果失败则进行重试
    int res = -1;
    const int MAX_FTP_RETRIES = 3;
    int retry_count = 0;
    
    while (retry_count < MAX_FTP_RETRIES && res != 0) {
        if (retry_count > 0) {
            ESP_LOGW("UploadImageToFtp", "重试FTP上传 (%d/%d)...", retry_count + 1, MAX_FTP_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(2000));  // 等待2秒后重试
            
            // 清空缓冲区
            memset(hdrbuf, 0, 1024);
            memset(bodybuf, 0, 2048);
        }
        
        res = Curl_FTP(1, (char*)ftp_url.c_str(), (char*)FTP_USER_PASS, (char*)temp_file.c_str(), 
                       hdrbuf, bodybuf, 1024, 2048);
        
        if (res != 0) {
            ESP_LOGW("UploadImageToFtp", "第%d次FTP上传失败: 错误代码=%d", retry_count + 1, res);
            ESP_LOGW("UploadImageToFtp", "FTP错误头: %s", hdrbuf);
            ESP_LOGW("UploadImageToFtp", "FTP错误体: %s", bodybuf);
        }
        
        retry_count++;
    }
    
    std::string result_path;
    if (res == 0) {
        result_path = std::string("/home/xuqianjin/") + filename;
        ESP_LOGI("UploadImageToFtp", "✅ 图片已通过FTP上传: %s", result_path.c_str());
        ESP_LOGI("UploadImageToFtp", "FTP响应头: %s", hdrbuf);
        if (retry_count > 1) {
            ESP_LOGI("UploadImageToFtp", "✅ 重试成功: 共尝试%d次", retry_count);
        }
    } else {
        ESP_LOGE("UploadImageToFtp", "❌ FTP上传最终失败: 错误代码=%d (尝试%d次)", res, retry_count);
        ESP_LOGE("UploadImageToFtp", "FTP最终错误头: %s", hdrbuf);
        ESP_LOGE("UploadImageToFtp", "FTP最终错误体: %s", bodybuf);
        
        // 分析错误类型
        if (strstr(bodybuf, "Couldn't connect to server") != nullptr) {
            ESP_LOGE("UploadImageToFtp", "🔍 诊断: 无法连接到FTP数据端口 - 可能是被动模式问题");
            ESP_LOGE("UploadImageToFtp", "🔍 建议: 检查FTP服务器被动模式配置或防火墙设置");
        } else if (strstr(bodybuf, "Software caused connection abort") != nullptr) {
            ESP_LOGE("UploadImageToFtp", "🔍 诊断: 连接被中断 - 可能是网络超时或服务器配置问题");
        }
        
        // 打印详细的错误信息
        switch(-res) {  // res是负数，所以取反
            case 6: ESP_LOGE("UploadImageToFtp", "错误类型: 无法解析主机名"); break;
            case 7: ESP_LOGE("UploadImageToFtp", "错误类型: 无法连接到服务器 (CURLE_COULDNT_CONNECT)"); break;
            case 26: ESP_LOGE("UploadImageToFtp", "错误类型: 读取本地文件错误"); break;
            case 67: ESP_LOGE("UploadImageToFtp", "错误类型: FTP认证失败"); break;
            case 78: ESP_LOGE("UploadImageToFtp", "错误类型: 远程文件未找到或权限不足"); break;
            default: ESP_LOGE("UploadImageToFtp", "错误类型: 未知FTP错误"); break;
        }
    }
    
    // 清理资源
    if (hdrbuf) free(hdrbuf);
    if (bodybuf) free(bodybuf);
    
    // 清理临时文件 (无论是否存在)
    if (!temp_file.empty()) {
        if (remove(temp_file.c_str()) == 0) {
            ESP_LOGI("UploadImageToFtp", "🗑️ 临时文件清理成功: %s", temp_file.c_str());
        } else {
            ESP_LOGW("UploadImageToFtp", "⚠️ 临时文件清理失败: %s", temp_file.c_str());
        }
    }
    
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
    
    cJSON_AddStringToObject(request, "type", "face");
    cJSON_AddStringToObject(payload, "action", "create_face_db");
    cJSON_AddStringToObject(payload, "db_name", db_name.c_str());
    cJSON_AddItemToObject(request, "payload", payload);
    
    char* json_string = cJSON_Print(request);
    ESP_LOGI("FaceRec", "Request JSON: %s", json_string);
    
    std::string response = SendFaceRequest(json_string);
    
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
    
    cJSON_AddStringToObject(request, "type", "face");
    cJSON_AddStringToObject(payload, "action", "list_people");
    cJSON_AddNumberToObject(payload, "limit", 100);
    cJSON_AddNumberToObject(payload, "offset", 0);
    cJSON_AddItemToObject(request, "payload", payload);
    
    char* json_string = cJSON_Print(request);
    
    // 🔥 详细打印WebSocket请求构造信息
    ESP_LOGI("FaceRec", "=== Constructing List Faces WebSocket Request ===");
    ESP_LOGI("FaceRec", "Request Type: face");
    ESP_LOGI("FaceRec", "Action: list_faces");
    ESP_LOGI("FaceRec", "Limit: 100");
    ESP_LOGI("FaceRec", "Offset: 0");
    ESP_LOGI("FaceRec", "Complete Request JSON: %s", json_string);
    ESP_LOGI("FaceRec", "=== Sending List Faces Request to WebSocket ===");
    
    std::string response = SendFaceRequest(json_string);
    
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

    // 生成安全的文件名 - 避免使用中文字符
    std::string filename = GenerateSafeFilename("face");
    
    // 使用FTP上传图片
    std::string image_path = UploadImageToFtp(fb, filename);
    
    if (image_path.empty()) {
        ESP_LOGE("FaceRec", "Failed to upload image");
        Alert(Lang::Strings::ERROR, "图片上传失败", "sad", Lang::Sounds::P3_EXCLAMATION);
        return "";
    }

    // 构建WebSocket请求
    cJSON* request = cJSON_CreateObject();
    cJSON* payload = cJSON_CreateObject();
    
    cJSON_AddStringToObject(request, "type", "face");
    cJSON_AddStringToObject(payload, "action", "add_face");
    cJSON_AddStringToObject(payload, "person_name", person_name.c_str());
    // 🔥 修改：使用 image_path 而不是 image_url
    cJSON_AddStringToObject(payload, "image_path", image_path.c_str());
    cJSON_AddItemToObject(request, "payload", payload);
    
    char* json_string = cJSON_Print(request);
    
    // 🔥 详细打印WebSocket请求构造信息
    ESP_LOGI("FaceRec", "=== Constructing Add Face WebSocket Request ===");
    ESP_LOGI("FaceRec", "Request Type: face");
    ESP_LOGI("FaceRec", "Action: add_face");
    ESP_LOGI("FaceRec", "Person Name: %s", person_name.c_str());
    ESP_LOGI("FaceRec", "Image Path: %s", image_path.c_str());
    ESP_LOGI("FaceRec", "Complete Request JSON: %s", json_string);
    ESP_LOGI("FaceRec", "=== Sending Add Face Request to WebSocket ===");
    
    std::string response = SendFaceRequest(json_string);
    
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

    // 生成安全的搜索文件名 - 避免使用中文字符
    std::string filename = GenerateSafeFilename("search");
    
    // 使用FTP上传图片
    std::string image_path = UploadImageToFtp(fb, filename);
    
    if (image_path.empty()) {
        ESP_LOGE("FaceRec", "Failed to upload search image");
        Alert(Lang::Strings::ERROR, "搜索图片上传失败", "sad", Lang::Sounds::P3_EXCLAMATION);
        return "";
    }

    // 构建WebSocket请求
    cJSON* request = cJSON_CreateObject();
    cJSON* payload = cJSON_CreateObject();
    
    cJSON_AddStringToObject(request, "type", "face");
    cJSON_AddStringToObject(payload, "action", "search_face");
    // 🔥 修改：使用 image_path 而不是 image_url
    cJSON_AddStringToObject(payload, "image_path", image_path.c_str());
    cJSON_AddNumberToObject(payload, "limit", 5);
    cJSON_AddNumberToObject(payload, "threshold", 80.0);
    cJSON_AddItemToObject(request, "payload", payload);
    
    char* json_string = cJSON_Print(request);
    
    // 🔥 详细打印WebSocket请求构造信息
    ESP_LOGI("FaceRec", "=== Constructing Search Face WebSocket Request ===");
    ESP_LOGI("FaceRec", "Request Type: face");
    ESP_LOGI("FaceRec", "Action: search_face");
    ESP_LOGI("FaceRec", "Image Path: %s", image_path.c_str());
    ESP_LOGI("FaceRec", "Search Limit: 5");
    ESP_LOGI("FaceRec", "Confidence Threshold: 80.0");
    ESP_LOGI("FaceRec", "Complete Request JSON: %s", json_string);
    ESP_LOGI("FaceRec", "=== Sending Search Face Request to WebSocket ===");
    
    std::string response = SendFaceRequest(json_string);
    
    ESP_LOGI("FaceRec", "Response: %s", response.c_str());
    ESP_LOGI("FaceRec", "=== Search Face Complete ===");
    
    free(json_string);
    cJSON_Delete(request);
    return response;
}

std::string Application::SendFaceRequest(const std::string& json_request) {
    ESP_LOGI("FaceRec", "=== SendFaceRequest START ===");
    
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
    
    protocol_->SendMcpMessage(updated_json);  // 使用正确的函数
    
    free(updated_json);
    cJSON_Delete(json_obj);

    std::string response = WaitForFaceResponse(request_id, 50);
    
    if (response.empty()) {
        ESP_LOGE("FaceRec", "No response received");
    } else {
        ESP_LOGI("FaceRec", "Response received: %d bytes", response.length());
    }
    
    ESP_LOGI("FaceRec", "=== SendFaceRequest END ===");
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
void Application::StoreFaceResponse(const std::string& request_id, const std::string& response) {
    ESP_LOGI("FaceRec", "Storing response for request ID: %s", request_id.c_str());
    ESP_LOGD("FaceRec", "Response content: %s", response.c_str());
    
    {
        std::lock_guard<std::mutex> lock(face_mutex_);
        
        // 存储响应数据
        face_responses_[request_id] = response;
        
        ESP_LOGI("FaceRec", "Response stored successfully. Current pending responses: %zu", 
                 face_responses_.size());
    }
    
    // 通知所有等待的线程
    face_cv_.notify_all();
    ESP_LOGI("FaceRec", "Notified waiting threads for request ID: %s", request_id.c_str());
}

// 等待人脸API响应
std::string Application::WaitForFaceResponse(const std::string& request_id, int timeout_seconds) {
    ESP_LOGI("FaceRec", "Waiting for response with ID: %s (timeout: %ds)", 
             request_id.c_str(), timeout_seconds);
    
    std::unique_lock<std::mutex> lock(face_mutex_);
    
    // 使用条件变量等待响应
    bool received = face_cv_.wait_for(
        lock, 
        std::chrono::seconds(timeout_seconds),
        [this, &request_id] {
            // 检查是否收到了对应的响应
            bool found = face_responses_.find(request_id) != face_responses_.end();
            if (found) {
                ESP_LOGD("FaceRec", "Response found for request ID: %s", request_id.c_str());
            }
            return found;
        }
    );
    
    if (received) {
        // 获取响应并清理
        std::string response = face_responses_[request_id];
        face_responses_.erase(request_id);
        
        ESP_LOGI("FaceRec", "Response retrieved for request ID: %s, length: %d bytes", 
                 request_id.c_str(), response.length());
        ESP_LOGI("FaceRec", "Remaining pending responses: %zu", face_responses_.size());
        
        return response;
    } else {
        // 超时处理
        ESP_LOGE("FaceRec", "Timeout waiting for response with ID: %s after %d seconds", 
                 request_id.c_str(), timeout_seconds);
        
        // 清理可能的残留数据
        auto it = face_responses_.find(request_id);
        if (it != face_responses_.end()) {
            ESP_LOGW("FaceRec", "Found stale response for request ID: %s, cleaning up", request_id.c_str());
            face_responses_.erase(it);
        }
        
        return "";
    }
}

// 清理过期的响应（可选的辅助函数）
void Application::CleanupExpiredResponses() {
    std::lock_guard<std::mutex> lock(face_mutex_);
    
    if (face_responses_.size() > 10) {  // 如果积累太多响应
        ESP_LOGW("FaceRec", "Too many pending responses (%zu), clearing all", 
                 face_responses_.size());
        face_responses_.clear();
    }
}

void Application::Start() {
    ESP_LOGI(TAG, "=== Application::Start() BEGIN ===");
    ESP_LOGI(TAG, "Free heap before start: %lu bytes", esp_get_free_heap_size());
    
    auto& board = Board::GetInstance();
    ESP_LOGI(TAG, "Board instance obtained");
    SetDeviceState(kDeviceStateStarting);
    ESP_LOGI(TAG, "Device state set to starting");

    /* Setup the display */
    ESP_LOGI(TAG, "Getting display...");
    auto display = board.GetDisplay();
    ESP_LOGI(TAG, "Display obtained: %p", display);

    /* Setup the audio service */
    ESP_LOGI(TAG, "Getting audio codec...");
    auto codec = board.GetAudioCodec();
    ESP_LOGI(TAG, "Audio codec obtained: %p", codec);
    
    ESP_LOGI(TAG, "Initializing audio service...");
    audio_service_.Initialize(codec);
    ESP_LOGI(TAG, "Audio service initialized");
    
    ESP_LOGI(TAG, "Starting audio service...");
    audio_service_.Start();
    ESP_LOGI(TAG, "Audio service started");
    ESP_LOGI(TAG, "Free heap after audio service: %lu bytes", esp_get_free_heap_size());

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

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    ESP_LOGI(TAG, "Starting network...");
    board.StartNetwork();
    ESP_LOGI(TAG, "Network started");
    ESP_LOGI(TAG, "Free heap after network: %lu bytes", esp_get_free_heap_size());

    // Update the status bar immediately to show the network state
    ESP_LOGI(TAG, "Updating status bar...");
    if(display) {
        display->UpdateStatusBar(true);
    }
    ESP_LOGI(TAG, "Status bar updated");

    // Check for new firmware version or get the MQTT broker address
    ESP_LOGI(TAG, "Creating OTA instance...");
    Ota ota;
    ESP_LOGI(TAG, "Checking new version...");
    CheckNewVersion(ota);
    ESP_LOGI(TAG, "New version check completed");
    ESP_LOGI(TAG, "Free heap after OTA check: %lu bytes", esp_get_free_heap_size());

    // Initialize the protocol
    ESP_LOGI(TAG, "Setting display status...");
    if(display) {
        display->SetStatus(Lang::Strings::LOADING_PROTOCOL);
    }
    ESP_LOGI(TAG, "Display status set");

    // Add MCP common tools before initializing the protocol
    ESP_LOGI(TAG, "Adding MCP common tools...");
    McpServer::GetInstance().AddCommonTools();
    ESP_LOGI(TAG, "MCP common tools added");
    ESP_LOGI(TAG, "Free heap after MCP tools: %lu bytes", esp_get_free_heap_size());

    ESP_LOGI(TAG, "Free heap after MCP tools: %lu bytes", esp_get_free_heap_size());

    ESP_LOGI(TAG, "Determining protocol type...");
    if (ota.HasMqttConfig()) {
        ESP_LOGI(TAG, "Using MQTT protocol");
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        ESP_LOGI(TAG, "Using WebSocket protocol");
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }
    ESP_LOGI(TAG, "Protocol instance created");
    ESP_LOGI(TAG, "Free heap after protocol creation: %lu bytes", esp_get_free_heap_size());

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
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
            if(display) {
                display->SetChatMessage("system", "");
            }
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
                        if(display) {
                            display->SetChatMessage("assistant", message.c_str());
                        }
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    if(display) {
                        display->SetChatMessage("user", message.c_str());
                    }
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
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
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
        } else if (strcmp(type->valuestring, "face_response") == 0) {
            ESP_LOGI("FaceRec", "=== Received face_response ===");
            
            auto request_id = cJSON_GetObjectItem(root, "request_id");
            auto payload = cJSON_GetObjectItem(root, "payload");
            
            if (cJSON_IsString(request_id) && cJSON_IsObject(payload)) {
                std::string response_id = request_id->valuestring;
                char* payload_str = cJSON_Print(payload);
                
                ESP_LOGI("FaceRec", "Processing response for request ID: %s", response_id.c_str());
                ESP_LOGD("FaceRec", "Payload: %s", payload_str);
                
                StoreFaceResponse(response_id, payload_str);
                
                free(payload_str);
            }
            ESP_LOGI("FaceRec", "=== face_response processed ===");
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    ESP_LOGI(TAG, "Protocol callbacks set");
    ESP_LOGI(TAG, "Free heap before protocol start: %lu bytes", esp_get_free_heap_size());
    
    ESP_LOGI(TAG, "Starting protocol...");
    bool protocol_started = protocol_->Start();
    ESP_LOGI(TAG, "Protocol start result: %s", protocol_started ? "SUCCESS" : "FAILED");
    ESP_LOGI(TAG, "Free heap after protocol start: %lu bytes", esp_get_free_heap_size());

    ESP_LOGI(TAG, "Setting device state to idle...");
    SetDeviceState(kDeviceStateIdle);
    ESP_LOGI(TAG, "Device state set to idle");

    has_server_time_ = ota.HasServerTime();
    ESP_LOGI(TAG, "Has server time: %s", has_server_time_ ? "YES" : "NO");
    
    if (protocol_started) {
        ESP_LOGI(TAG, "Displaying success message...");
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        if (display) {
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        }
        ESP_LOGI(TAG, "Playing success sound...");
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::P3_SUCCESS);
        ESP_LOGI(TAG, "Success sound played");
    }

    // Print heap stats
    ESP_LOGI(TAG, "Printing heap stats...");
    SystemInfo::PrintHeapStats();
    ESP_LOGI(TAG, "=== Application::Start() END ===");
}

void Application::OnClockTimer() {
    clock_ticks_++;

    auto display = Board::GetInstance().GetDisplay();
    if (display)
    display->UpdateStatusBar();

    // Print the debug info every 10 seconds
    if (clock_ticks_ % 10 == 0) {
        // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
        // SystemInfo::PrintTaskList();
        SystemInfo::PrintHeapStats();
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
    // Raise the priority of the main event loop to avoid being interrupted by background tasks (which has priority 2)
    ESP_LOGI(TAG, "Setting task priority to 3...");
    vTaskPrioritySet(NULL, 3);
    ESP_LOGI(TAG, "Entering main event loop...");

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);
        
        if (bits & MAIN_EVENT_ERROR) {
            ESP_LOGE(TAG, "Main event error detected");
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "sad", Lang::Sounds::P3_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (!protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
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
        audio_service_.PlaySound(Lang::Sounds::P3_POPUP);
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

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
        if (display) {
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
        }
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
                if (display) {
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
                }
            break;
        case kDeviceStateListening:
                if (display) {
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
                }
            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            if (display)
                display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
#if CONFIG_USE_AFE_WAKE_WORD
                audio_service_.EnableWakeWordDetection(true);
#else
                audio_service_.EnableWakeWordDetection(false);
#endif
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

    if (!audio_service_.IsIdle()) {
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
            audio_service_.EnableDeviceAec(false);
            if (display)
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            if (display)
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            if (display)
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