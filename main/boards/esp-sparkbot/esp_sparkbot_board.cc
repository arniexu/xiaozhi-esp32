#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "font_awesome_symbols.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "settings.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_spiffs.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/uart.h>
#include <cstring>

#include "esp32_camera.h"
#include "mbedtls/md.h"
#include <random>
#include <iomanip>
#include <chrono>
// 在现有的 #include 列表中添加以下头文件：

#include "esp_http_client.h"  // 添加这个头文件
#include "cJSON.h"            // 如果还没有的话也需要添加
#include <sstream>            // 用于 std::stringstream
// Add a simple base64_encode function declaration if not provided by any header
std::string base64_encode(const uint8_t* data, size_t len);

#define TAG "esp_sparkbot"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class SparkBotEs8311AudioCodec : public Es8311AudioCodec {
private:    

public:
    SparkBotEs8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
                        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
                        gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk = true)
        : Es8311AudioCodec(i2c_master_handle, i2c_port, input_sample_rate, output_sample_rate,
                             mclk,  bclk,  ws,  dout,  din,pa_pin,  es8311_addr,  use_mclk = true) {}

    void EnableOutput(bool enable) override {
        if (enable == output_enabled_) {
            return;
        }
        if (enable) {
            Es8311AudioCodec::EnableOutput(enable);
        } else {
           // Nothing todo because the display io and PA io conflict
        }
    }
};

class EspSparkBot : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Display* display_;
    Esp32Camera* camera_;
    light_mode_t light_mode_ = LIGHT_MODE_ALWAYS_ON;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void MountStorage() {
        // Mount the storage partition
        esp_vfs_spiffs_conf_t conf = {
            .base_path = "/storage",
            .partition_label = "storage",
            .max_files = 5,
            .format_if_mount_failed = true,
        };
        
        esp_err_t ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK) {
            if (ret == ESP_FAIL) {
                ESP_LOGE(TAG, "Failed to mount or format filesystem");
            } else if (ret == ESP_ERR_NOT_FOUND) {
                ESP_LOGE(TAG, "Failed to find SPIFFS partition 'storage'");
            } else {
                ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
            }
            return;
        }
        
        size_t total = 0, used = 0;
        ret = esp_spiffs_info("storage", &total, &used);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Storage partition size: total: %d KB, used: %d KB", total / 1024, used / 1024);
        }
    }
void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_GPIO;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_GPIO;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
    }
#if 0
    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_GPIO;
        io_config.dc_gpio_num = DISPLAY_DC_GPIO;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    {
                                        .text_font = &font_puhui_20_4,
                                        .icon_font = &font_awesome_20_4,
                                        .emoji_font = font_emoji_64_init(),
                                    });
    }
#else
    void InitializeDisplay() {
        display_ = nullptr;
    }
#endif
    void InitializeCamera() {
        camera_config_t camera_config = {};

        camera_config.pin_pwdn = SPARKBOT_CAMERA_PWDN;
        camera_config.pin_reset = SPARKBOT_CAMERA_RESET;
        camera_config.pin_xclk = SPARKBOT_CAMERA_XCLK;
        camera_config.pin_pclk = SPARKBOT_CAMERA_PCLK;
        camera_config.pin_sccb_sda = SPARKBOT_CAMERA_SIOD;
        camera_config.pin_sccb_scl = SPARKBOT_CAMERA_SIOC;

        camera_config.pin_d0 = SPARKBOT_CAMERA_D0;
        camera_config.pin_d1 = SPARKBOT_CAMERA_D1;
        camera_config.pin_d2 = SPARKBOT_CAMERA_D2;
        camera_config.pin_d3 = SPARKBOT_CAMERA_D3;
        camera_config.pin_d4 = SPARKBOT_CAMERA_D4;
        camera_config.pin_d5 = SPARKBOT_CAMERA_D5;
        camera_config.pin_d6 = SPARKBOT_CAMERA_D6;
        camera_config.pin_d7 = SPARKBOT_CAMERA_D7;

        camera_config.pin_vsync = SPARKBOT_CAMERA_VSYNC;
        camera_config.pin_href = SPARKBOT_CAMERA_HSYNC;
        camera_config.pin_pclk = SPARKBOT_CAMERA_PCLK;
        camera_config.xclk_freq_hz = SPARKBOT_CAMERA_XCLK_FREQ;
        camera_config.ledc_timer = SPARKBOT_LEDC_TIMER;
        camera_config.ledc_channel = SPARKBOT_LEDC_CHANNEL;
        camera_config.fb_location = CAMERA_FB_IN_PSRAM;
        
        camera_config.sccb_i2c_port = I2C_NUM_0;
        
        camera_config.pixel_format = PIXFORMAT_RGB565;
        camera_config.frame_size = FRAMESIZE_240X240;
        camera_config.jpeg_quality = 12;
        camera_config.fb_count = 1;
        camera_config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        
        camera_ = new Esp32Camera(camera_config);

        Settings settings("sparkbot", false);
        // 考虑到部分复刻使用了不可动摄像头的设计，默认启用翻转
        bool camera_flipped = static_cast<bool>(settings.GetInt("camera-flipped", 1));
        camera_->SetHMirror(camera_flipped);
        camera_->SetVFlip(camera_flipped);
    }

    /*
        ESP-SparkBot 的底座
        https://gitee.com/esp-friends/esp_sparkbot/tree/master/example/tank/c2_tracked_chassis
    */
    void InitializeEchoUart() {
        uart_config_t uart_config = {
            .baud_rate = ECHO_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        int intr_alloc_flags = 0;

        ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
        ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, UART_ECHO_TXD, UART_ECHO_RXD, UART_ECHO_RTS, UART_ECHO_CTS));

        SendUartMessage("w2");
    }

    void SendUartMessage(const char * command_str) {
        uint8_t len = strlen(command_str);
        uart_write_bytes(ECHO_UART_PORT_NUM, command_str, len);
        ESP_LOGI(TAG, "Sent command: %s", command_str);
    }

    // void myLog(const char * command_str) {
    //     uint8_t len = strlen(command_str);
    //     uart_write_bytes(UART_NUM_0, command_str, len);
    //     ESP_LOGI(TAG, "Sent command: %s", command_str);
    // }

    // void SavePhotoToSpiffs(const uint8_t* buf, size_t len, const std::string& filename) {
    //     std::string path = "/spiffs/" + filename;
    //     FILE* f = fopen(path.c_str(), "wb");
    //     if (f) {
    //         fwrite(buf, 1, len, f);
    //         fclose(f);
    //         ESP_LOGI(TAG, "Saved photo: %s", path.c_str());
    //     } else {
    //         ESP_LOGE(TAG, "Failed to open file for writing: %s", path.c_str());
    //     }
    // }
    // 在文件末尾添加以下实现

    // 享老汇API配置
    #define XIANGLAO_API_URL "https://sign.upcif.com/xlh/senior/queryhealthdata"
    #define XIANGLAO_DOMAIN "xiaozhi-device.com"  // 替换为你的实际域名

    // 生成流水号：YYYYMMDDHHMISS + 5位随机数
    std::string GenerateSerialNumber() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        
        // 生成5位随机数
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(10000, 99999);
        int random_num = dis(gen);
        
        // 格式：YYYYMMDDHHMISS + 5位随机数（总共25位）
        char serial[64]; // 增大缓冲区以避免截断警告
        snprintf(serial, sizeof(serial), "%04d%02d%02d%02d%02d%02d%05d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour%24, tm.tm_min, tm.tm_sec, random_num);
        
        return std::string(serial);
    }

    // 计算SHA-256哈希值：流水号|手机号|域名
    std::string CalculateSHA256(const std::string& serial_number, 
                                        const std::string& mobile_phone, 
                                        const std::string& domain) {
        // 拼接格式：流水号|手机号|域名
        std::string combined = serial_number + "|" + mobile_phone + "|" + domain;
        
        ESP_LOGI("XiangLaoHui", "SHA-256 input: %s", combined.c_str());
        
        unsigned char hash[32];
        mbedtls_md_context_t ctx;
        mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
        
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
        mbedtls_md_starts(&ctx);
        mbedtls_md_update(&ctx, (const unsigned char*)combined.c_str(), combined.length());
        mbedtls_md_finish(&ctx, hash);
        mbedtls_md_free(&ctx);
        
        // 转换为16进制字符串（64位小写）
        std::stringstream ss;
        for (int i = 0; i < 32; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        
        std::string result = ss.str();
        ESP_LOGI("XiangLaoHui", "SHA-256 result: %s", result.c_str());
        return result;
    }

    // 查询享老汇健康数据
    std::string QueryXiangLaoHuiHealthData(const std::string& mobile_phone) {
        ESP_LOGI("XiangLaoHui", "Querying health data for mobile: %s", mobile_phone.c_str());
        
        // 生成流水号和nonce_str
        std::string serial_number = GenerateSerialNumber();
        std::string nonce_str = CalculateSHA256(serial_number, mobile_phone, XIANGLAO_DOMAIN);
        
        ESP_LOGI("XiangLaoHui", "Serial number: %s", serial_number.c_str());
        ESP_LOGI("XiangLaoHui", "Nonce string: %s", nonce_str.c_str());
        
        // 构建请求JSON - 严格按照API规范
        cJSON* request_json = cJSON_CreateObject();
        cJSON_AddStringToObject(request_json, "serial_number", serial_number.c_str());
        cJSON_AddStringToObject(request_json, "mobile_phone", mobile_phone.c_str());
        cJSON_AddStringToObject(request_json, "nonce_str", nonce_str.c_str());
        
        char* json_string = cJSON_Print(request_json);
        std::string request_body(json_string);
        free(json_string);
        cJSON_Delete(request_json);
        
        ESP_LOGI("XiangLaoHui", "Request body: %s", request_body.c_str());
        
        // 发送HTTP POST请求
        esp_http_client_config_t config = {
            .url = XIANGLAO_API_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 15000,
        };
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/json;charset=utf-8");
        esp_http_client_set_post_field(client, request_body.c_str(), request_body.length());
        
        esp_err_t err = esp_http_client_perform(client);
        std::string response;
        
        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            ESP_LOGI("XiangLaoHui", "HTTP Status: %d", status_code);
            
            if (status_code == 200) {
                int content_length = esp_http_client_get_content_length(client);
                if (content_length > 0) {
                    char* buffer = new char[content_length + 1];
                    int read_len = esp_http_client_read_response(client, buffer, content_length);
                    if (read_len > 0) {
                        buffer[read_len] = '\0';
                        response.assign(buffer, read_len);
                        ESP_LOGI("XiangLaoHui", "Response: %s", response.c_str());
                    }
                    delete[] buffer;
                }
            } else {
                ESP_LOGE("XiangLaoHui", "HTTP request failed with status: %d", status_code);
                // 构建错误响应
                cJSON* error_json = cJSON_CreateObject();
                cJSON_AddStringToObject(error_json, "return_code", "FAIL");
                cJSON_AddStringToObject(error_json, "return_msg", "HTTP Error");
                char* error_str = cJSON_Print(error_json);
                response = std::string(error_str);
                free(error_str);
                cJSON_Delete(error_json);
            }
        } else {
            ESP_LOGE("XiangLaoHui", "HTTP request failed: %s", esp_err_to_name(err));
            // 构建网络错误响应
            cJSON* error_json = cJSON_CreateObject();
            cJSON_AddStringToObject(error_json, "return_code", "FAIL");
            cJSON_AddStringToObject(error_json, "return_msg", "Network Error");
            char* error_str = cJSON_Print(error_json);
            response = std::string(error_str);
            free(error_str);
            cJSON_Delete(error_json);
        }
        
        esp_http_client_cleanup(client);
        return response;
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        // 定义设备的属性
        mcp_server.AddTool("self.chassis.get_light_mode", "获取灯光效果编号", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            if (light_mode_ < 2) {
                return 1;
            } else {
                return light_mode_ - 2;
            }
        });

        mcp_server.AddTool("self.chassis.go_forward", "前进", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SendUartMessage("x0.0 y1.0");
            return true;
        });

        mcp_server.AddTool("self.chassis.go_back", "后退", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SendUartMessage("x0.0 y-1.0");
            return true;
        });

        mcp_server.AddTool("self.chassis.turn_left", "向左转", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SendUartMessage("x-1.0 y0.0");
            return true;
        });

        mcp_server.AddTool("self.chassis.turn_right", "向右转", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SendUartMessage("x1.0 y0.0");
            return true;
        });
        
        mcp_server.AddTool("self.chassis.dance", "跳舞", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SendUartMessage("d1");
            light_mode_ = LIGHT_MODE_MAX;
            return true;
        });

        // 享老汇健康数据查询工具,硬编码账户信息
        mcp_server.AddTool("self.health.query_elder_health_data", "查询老人健康数据", PropertyList({
            Property("mobile_phone", kPropertyTypeString)
        }), [this](const PropertyList& properties) -> ReturnValue {
            std::string mobile_phone = properties["mobile_phone"].value<std::string>();
            if (mobile_phone.empty()) {
                throw std::runtime_error("手机号不能为空");
            }
            
            // 手机号格式验证
            if (mobile_phone.length() != 11 || mobile_phone[0] != '1') {
                throw std::runtime_error("手机号格式不正确，请输入11位手机号");
            }
            
            std::string response = QueryXiangLaoHuiHealthData(mobile_phone);
            
            // 解析响应数据
            cJSON* json = cJSON_Parse(response.c_str());
            if (!json) {
                ESP_LOGE("XiangLaoHui", "Failed to parse response JSON");
                return std::string("响应数据解析失败");
            }
            
            cJSON* return_code = cJSON_GetObjectItem(json, "return_code");
            if (cJSON_IsString(return_code) && strcmp(return_code->valuestring, "SUCCESS") == 0) {
                // 成功获取健康数据，构建友好的摘要
                std::string health_summary = "🏥 健康数据查询成功!\n\n";
                
                cJSON* heart_rate = cJSON_GetObjectItem(json, "heart_rate");
                cJSON* blood_glucose = cJSON_GetObjectItem(json, "blood_glucose");
                cJSON* oxygen_saturation = cJSON_GetObjectItem(json, "oxygen_saturation");
                cJSON* body_temperature = cJSON_GetObjectItem(json, "body_temperature");
                cJSON* blood_pressure = cJSON_GetObjectItem(json, "blood_pressure");
                cJSON* response_time = cJSON_GetObjectItem(json, "response_time");
                
                if (cJSON_IsString(heart_rate)) {
                    health_summary += "❤️ 心率: " + std::string(heart_rate->valuestring) + " bpm\n";
                }
                if (cJSON_IsString(blood_glucose)) {
                    health_summary += "🩸 血糖: " + std::string(blood_glucose->valuestring) + " mg/dL\n";
                }
                if (cJSON_IsString(oxygen_saturation)) {
                    health_summary += "🫁 血氧: " + std::string(oxygen_saturation->valuestring) + "%\n";
                }
                if (cJSON_IsString(body_temperature)) {
                    health_summary += "🌡️ 体温: " + std::string(body_temperature->valuestring) + "°C\n";
                }
                if (cJSON_IsString(blood_pressure)) {
                    health_summary += "💉 血压: " + std::string(blood_pressure->valuestring) + " mmHg\n";
                }
                if (cJSON_IsString(response_time)) {
                    health_summary += "⏰ 数据时间: " + std::string(response_time->valuestring);
                }
                
                cJSON_Delete(json);
                ESP_LOGI("XiangLaoHui", "Health data retrieved successfully for: %s", mobile_phone.c_str());
                return health_summary;
            } else {
                // 查询失败
                cJSON* return_msg = cJSON_GetObjectItem(json, "return_msg");
                std::string error_msg = "❌ 查询失败";
                if (cJSON_IsString(return_msg)) {
                    error_msg += ": " + std::string(return_msg->valuestring);
                }
                cJSON_Delete(json);
                ESP_LOGE("XiangLaoHui", "Health data query failed: %s", error_msg.c_str());
                return error_msg;
            }
        });

        mcp_server.AddTool("self.chassis.switch_light_mode", "打开灯光效果", PropertyList({
            Property("light_mode", kPropertyTypeInteger, 1, 6)
        }), [this](const PropertyList& properties) -> ReturnValue {
            char command_str[5] = {'w', 0, 0};
            char mode = static_cast<light_mode_t>(properties["light_mode"].value<int>());

            ESP_LOGI(TAG, "Switch Light Mode: %c", (mode + '0'));

            if (mode >= 3 && mode <= 8) {
                command_str[1] = mode + '0';
                SendUartMessage(command_str);
                return true;
            }
            throw std::runtime_error("Invalid light mode");
        });
        
        mcp_server.AddTool("self.camera.user_register", "注册新用户，用户需提供用户名，用户名只能包含英文字母和数字", PropertyList({
            Property("person_name", kPropertyTypeString)
        }), [this](const PropertyList& properties) -> ReturnValue {
            if (!camera_) {
                throw std::runtime_error("Camera not initialized");
            }
            std::string person_name = properties["person_name"].value<std::string>();
            if (person_name.empty()) {
                throw std::runtime_error("Person name is empty");
            }
            
            // 拍照
            camera_fb_t* fb = esp_camera_fb_get();
            if (!fb || !fb->buf || fb->len == 0) {
                esp_camera_fb_return(fb);
                throw std::runtime_error("Camera capture failed");
            }
            ESP_LOGI("Camera", "Captured photo for person: %s", person_name.c_str());               
            // 保存到阿里云人脸数据库
            auto& app = Application::GetInstance();
            std::string response = app.AddFaceToAliyunDB(person_name, fb);
            
            // 检查是否成功
            cJSON* root = cJSON_Parse(response.c_str());
            bool success = false;
            if (root) {
                cJSON* code = cJSON_GetObjectItem(root, "Code");
                if (cJSON_IsString(code) && strcmp(code->valuestring, "OK") == 0) {
                    success = true;
                }
                cJSON_Delete(root);
            }
            esp_camera_fb_return(fb);
            if (success) {
                ESP_LOGI("Camera", "Face saved to Aliyun DB for person: %s", person_name.c_str());
                return true;
            } else {
                ESP_LOGE("Camera", "Failed to save face to Aliyun DB: %s", response.c_str());
                throw std::runtime_error("Failed to save face to Aliyun DB");
            }
        });

        // 替换现有的 "self.camera.all_users_in_medical_platform" 工具
        mcp_server.AddTool("self.camera.all_users_in_medical_platform", "获取医疗平台所有用户名", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            std::string response = app.ListFacesInAliyunDB();
            std::string user_list = app.ParseListFacesResult(response);
            
            if (!user_list.empty()) {
                ESP_LOGI("Camera", "Retrieved user list: %s", user_list.c_str());
                return user_list;
            } else {
                ESP_LOGI("Camera", "No users found in medical platform");
                return std::string("暂无注册用户");
            }
        });

        mcp_server.AddTool("self.camera.set_camera_flipped", "翻转摄像头图像方向", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            Settings settings("sparkbot", true);
            // 考虑到部分复刻使用了不可动摄像头的设计，默认启用翻转
            bool flipped = !static_cast<bool>(settings.GetInt("camera-flipped", 1));
            
            camera_->SetHMirror(flipped);
            camera_->SetVFlip(flipped);
            
            settings.SetInt("camera-flipped", flipped ? 1 : 0);
            
            return true;
        });

        mcp_server.AddTool("self.camera.get_user_name", "获取当前用户名", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            std::string name = app.whoareyou();
            if (!name.empty()) {
                return name;
            } else {
                return std::string("unknown");
            }
        });

    }

public:
    EspSparkBot() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        InitializeCamera();
        InitializeEchoUart();
        // InitializeConsoleUart();
        MountStorage();
        // 打印输出一些内容测试uart0
        InitializeTools();
        //GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
         static SparkBotEs8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
#if 0
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
#else
    virtual Backlight* GetBacklight() override {    
        return nullptr;
    }
#endif

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(EspSparkBot);
