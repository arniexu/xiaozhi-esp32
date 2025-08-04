#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <vector>
#include <memory>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"

#include "esp_camera.h"

#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED (1 << 2)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CHECK_NEW_VERSION_DONE (1 << 5)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void AbortSpeaking(AbortReason reason);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    // 阿里云人脸数据库相关方法
    std::string whoareyou();
    std::string GenerateSafeFilename(const std::string& base_name, const std::string& suffix = "");
    
    // 其他可能需要的方法
    std::string CreateFaceDB(const std::string& db_name);
    std::string ParseSearchFaceResult(const std::string& response);
    std::string ParseListFacesResult(const std::string& response);
    // 人脸识别相关函数
    std::string ListFacesInAliyunDB();
    std::string AddFaceToAliyunDB(const std::string& person_name, camera_fb_t* fb);
    std::string SearchFaceInAliyunDB(camera_fb_t* fb);
    std::string UploadImageToFtp(camera_fb_t* fb, const std::string& filename);
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    int clock_ticks_ = 0;
    TaskHandle_t check_new_version_task_handle_ = nullptr;

    
    // 人脸API响应管理
    std::map<std::string, std::string> face_responses_;
    std::mutex face_mutex_;
    std::condition_variable face_cv_;
    
    void OnWakeWordDetected();
    void CheckNewVersion(Ota& ota);
    void ShowActivationCode(const std::string& code, const std::string& message);
    void OnClockTimer();
    void SetListeningMode(ListeningMode mode);
    std::string SendFaceRequest(const std::string& json_request);
    void StoreFaceResponse(const std::string& request_id, const std::string& response);
    std::string WaitForFaceResponse(const std::string& request_id, int timeout_seconds);
    void CleanupExpiredResponses();
};

#endif // _APPLICATION_H_
