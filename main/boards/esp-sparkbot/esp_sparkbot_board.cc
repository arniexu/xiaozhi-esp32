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
#include "esp_crt_bundle.h"   // ESP32证书包
#include <sstream>            // 用于 std::stringstream
#include <set>                // 用于 std::set
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
        ESP_LOGI(TAG, "=== Mounting Storage Partition ===");
        
        // Mount the storage partition
        esp_vfs_spiffs_conf_t conf = {
            .base_path = "/storage",
            .partition_label = "storage",
            .max_files = 5,
            .format_if_mount_failed = true,
        };
        
        ESP_LOGI(TAG, "Attempting to mount SPIFFS partition 'storage' at '/storage'");
        esp_err_t ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK) {
            if (ret == ESP_FAIL) {
                ESP_LOGE(TAG, "❌ Failed to mount or format filesystem");
            } else if (ret == ESP_ERR_NOT_FOUND) {
                ESP_LOGE(TAG, "❌ Failed to find SPIFFS partition 'storage' - check partition table");
            } else {
                ESP_LOGE(TAG, "❌ Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
            }
            return;
        }
        
        ESP_LOGI(TAG, "✅ SPIFFS mounted successfully");
        
        size_t total = 0, used = 0;
        ret = esp_spiffs_info("storage", &total, &used);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "📊 Storage partition size: total: %d KB, used: %d KB", total / 1024, used / 1024);
        }
        
        // Test write access
        FILE* test_file = fopen("/storage/test.txt", "w");
        if (test_file) {
            fprintf(test_file, "test");
            fclose(test_file);
            remove("/storage/test.txt");
            ESP_LOGI(TAG, "✅ Storage write test successful");
        } else {
            ESP_LOGE(TAG, "❌ Storage write test failed");
        }
    }
    // 初始化红外传感器GPIO35
    void initializeIRSensor() {
        // 配置 GPIO35 为输入模式
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << 35);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);

        // 任务1 使用红外传感器主动触发小智使其进入激活状态，播放提示音，进行人脸识别，主动向用户打招呼
        // 任务1 步骤1 创建定时器，周期性轮询红外传感器状态
        const esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto& app = Application::GetInstance();
                int ir_state = gpio_get_level((gpio_num_t)38);
                // 如果当前设备已经被激活，则不处理红外传感器事件
                if (app.GetDeviceState() == kDeviceStateActivating) {
                    return;
                }
                ESP_LOGI(TAG, "IR Sensor(GPIO38) state: %d", ir_state);
                // 可在此处添加进一步处理逻辑
                if (ir_state == 0) {
                     // 发送wake up word detected事件
                    ESP_LOGI(TAG, "IR Sensor triggered - Simulating Wake Word Detected");   
                    app.SimulateWakeWordDetected();
                }
                else {
                    ESP_LOGI(TAG, "IR Sensor not triggered");
                    // 没有action，不使用红外决定是否退出activating模式
                }
            },
            .arg = nullptr,
            .name = "ir_sensor_poll"
        };
        esp_timer_handle_t timer_handle;
        // 获取 Application 实例并作为 arg 传递
        auto& app = Application::GetInstance();
        esp_timer_create_args_t timer_args_with_arg = timer_args;
        timer_args_with_arg.arg = &app;
        esp_timer_create(&timer_args_with_arg, &timer_handle);
        // 每500ms轮询一次
        esp_timer_start_periodic(timer_handle, 500 * 1000);
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
    
    //实现函数初始化gpio3和gpio47为输出并且输出低电平
    void InitializeGpio() {
        gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT);
        gpio_set_direction(GPIO_NUM_47, GPIO_MODE_OUTPUT);
        gpio_set_level(GPIO_NUM_3, 0);
        gpio_set_level(GPIO_NUM_47, 0);
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
        // bool camera_flipped = static_cast<bool>(settings.GetInt("camera-flipped", 1));
        // camera_->SetHMirror(camera_flipped);
        // camera_->SetVFlip(camera_flipped);
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
    #define XIANGLAO_API_URL "https://sign.upcif.com/xlh-api/senior/queryhealthdata"
    #define XIANGLAO_DOMAIN "sign.upcif.com"

    // 生成流水号：YYYYMMDDHHMISS + 5位随机数
    std::string GenerateSerialNumber() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        
        // 使用 gmtime 确保使用 UTC 时间，与服务器保持一致
        auto tm = *std::gmtime(&time_t);
        
        // 生成5位随机数
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(10000, 99999);
        int random_num = dis(gen);
        
        // 格式：YYYYMMDDHHMISS + 5位随机数（总共25位）
        char serial[64]; // 增大缓冲区以避免截断警告
        snprintf(serial, sizeof(serial), "%04d%02d%02d%02d%02d%02d%05d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, random_num);
        
        ESP_LOGI("XiangLaoHui", "Generated serial number: %s", serial);
        return std::string(serial);
    }

    // 计算SHA-256哈希值：流水号#手机号#域名
    std::string CalculateNonce(const std::string& serial_number,
                              const std::string& mobile_phone, 
                              const std::string& domain) {
        // 拼接格式：流水号#手机号#域名 (使用#作为分隔符)
        std::string combined = serial_number + "#" + mobile_phone + "#" + domain;
        
        ESP_LOGI("XiangLaoHui", "Nonce input: %s", combined.c_str());
        
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
        ESP_LOGI("XiangLaoHui", "Nonce result: %s", result.c_str());
        return result;
    }

    // 实现一个函数来控制安卓播
    // 查询享老汇健康数据
    std::string QueryXiangLaoHuiHealthData(const std::string &mobile_phone)
    {
        ESP_LOGI("XiangLaoHui", "Querying health data for mobile: %s", mobile_phone.c_str());

        // 生成流水号和nonce_str
        std::string serial_number = GenerateSerialNumber();
        std::string nonce_str = CalculateNonce(serial_number, mobile_phone, XIANGLAO_DOMAIN);

        ESP_LOGI("XiangLaoHui", "Serial number: %s", serial_number.c_str());
        ESP_LOGI("XiangLaoHui", "Nonce string: %s", nonce_str.c_str());

        // 构建请求JSON - 严格按照API规范
        cJSON *request_json = cJSON_CreateObject();
        cJSON_AddStringToObject(request_json, "serial_number", serial_number.c_str());
        cJSON_AddStringToObject(request_json, "mobile_phone", mobile_phone.c_str());
        cJSON_AddStringToObject(request_json, "nonce_str", nonce_str.c_str());

        char *json_string = cJSON_Print(request_json);
        std::string request_body(json_string);
        free(json_string);
        cJSON_Delete(request_json);

        ESP_LOGI("XiangLaoHui", "Request body: %s", request_body.c_str());

        // 发送HTTP POST请求
        esp_http_client_config_t config = {};
        config.url = XIANGLAO_API_URL;
        config.method = HTTP_METHOD_POST;
        config.timeout_ms = 15000;
        config.skip_cert_common_name_check = false;  // 启用证书域名检查
        config.crt_bundle_attach = esp_crt_bundle_attach;  // 使用证书包
        config.is_async = false;
        config.transport_type = HTTP_TRANSPORT_OVER_SSL;
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/json;charset=utf-8");
        esp_http_client_set_header(client, "User-Agent", "ESP32-XiangLaoHui/1.0");  // 添加User-Agent
        esp_http_client_set_post_field(client, request_body.c_str(), request_body.length());
        
        // 首先开始请求，但不等待完成
        esp_err_t err = esp_http_client_open(client, request_body.length());
        if (err != ESP_OK) {
            ESP_LOGE("XiangLaoHui", "Failed to open HTTP connection: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return "HTTP connection failed";
        }
        
        // 写入请求体
        int write_len = esp_http_client_write(client, request_body.c_str(), request_body.length());
        if (write_len < 0) {
            ESP_LOGE("XiangLaoHui", "Failed to write request body");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return "Failed to send request";
        }
        
        ESP_LOGI("XiangLaoHui", "Request body written: %d bytes", write_len);
        
        // 获取响应头
        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI("XiangLaoHui", "HTTP Status: %d", status_code);
        ESP_LOGI("XiangLaoHui", "Content length: %d", content_length);
        
        std::string response;
        
        if (status_code == 200) {
            // 读取响应数据
            if (content_length > 0) {
                // 已知内容长度
                char* buffer = new char[content_length + 1];
                int read_len = esp_http_client_read(client, buffer, content_length);
                if (read_len > 0) {
                    buffer[read_len] = '\0';
                    response.assign(buffer, read_len);
                    ESP_LOGI("XiangLaoHui", "Response read successfully, length: %d", read_len);
                    ESP_LOGI("XiangLaoHui", "Response: %s", response.c_str());
                } else {
                    ESP_LOGE("XiangLaoHui", "Failed to read response data, read_len: %d", read_len);
                }
                delete[] buffer;
            } else {
                // 未知内容长度或分块传输
                ESP_LOGW("XiangLaoHui", "Content-Length unknown, reading in chunks");
                char buffer[1024];
                int total_read = 0;
                while (true) {
                    int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
                    if (read_len <= 0) {
                        break;
                    }
                    buffer[read_len] = '\0';
                    response.append(buffer, read_len);
                    total_read += read_len;
                    ESP_LOGI("XiangLaoHui", "Read chunk: %d bytes, total: %d", read_len, total_read);
                }
                ESP_LOGI("XiangLaoHui", "Total response read: %d bytes", total_read);
                ESP_LOGI("XiangLaoHui", "Response: %s", response.c_str());
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
        
        // 关闭连接并清理
        esp_http_client_close(client);
        
        esp_http_client_cleanup(client);
        return response;
    }
    // 获取 Home Assistant 某个实体的状态
    std::string GetHomeAssistantState(const std::string &ha_url, const std::string &token, const std::string &entity_id)
    {
        std::string url = ha_url + "/api/states/" + entity_id;

        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.method = HTTP_METHOD_GET;
        config.timeout_ms = 10000;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.transport_type = HTTP_TRANSPORT_OVER_SSL; // 如果是 https

        esp_http_client_handle_t client = esp_http_client_init(&config);
        std::string auth_header = "Bearer " + token;
        esp_http_client_set_header(client, "Authorization", auth_header.c_str());
        esp_http_client_set_header(client, "Content-Type", "application/json");

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK)
        {
            esp_http_client_cleanup(client);
            return "HTTP open failed";
        }

        int content_length = esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);

        std::string response;
        if (status_code == 200 && content_length > 0)
        {
            char *buffer = new char[content_length + 1];
            int read_len = esp_http_client_read(client, buffer, content_length);
            if (read_len > 0)
            {
                buffer[read_len] = '\0';
                response.assign(buffer, read_len);
            }
            delete[] buffer;
        }
        else
        {
            response = "HTTP error: " + std::to_string(status_code);
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return response;
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        // MCP方法统一包装器，支持失败返回值也显示表情
        auto mcp_wrapper = [this](auto func) {
            return [this, func](const PropertyList& properties) -> ReturnValue {
                auto mcp_fail_count_ = 0;
                try {
                    auto result = func(properties);
                    bool failed = false;
                    // 针对常见类型判断失败
                    if constexpr (std::is_same_v<decltype(result), bool>) {
                        failed = !result;
                    } else if constexpr (std::is_same_v<decltype(result), std::string>) {
                        failed = result.empty() || result.find("❌") != std::string::npos;
                    } else if constexpr (std::is_pointer_v<decltype(result)>) {
                        failed = (result == nullptr);
                    }
                    if (failed) {
                        mcp_fail_count_++;
                        if (mcp_fail_count_ >= 3) {
                            ShowAndroidEmoji("angry");
                        } else {
                            ShowAndroidEmoji("sad");
                        }
                    } else {
                        mcp_fail_count_ = 0;
                        ShowAndroidEmoji("smile");
                    }
                    return result;
                } catch (const std::exception& e) {
                    mcp_fail_count_++;
                    if (mcp_fail_count_ >= 3) {
                        ShowAndroidEmoji("angry");
                    } else {
                        ShowAndroidEmoji("sad");
                    }
                    throw;
                }
            };
        };
        // 定义设备的属性
        mcp_server.AddTool("self.chassis.get_light_mode", "获取灯光效果编号", PropertyList(), mcp_wrapper([this](const PropertyList& properties) -> ReturnValue {
            if (light_mode_ < 2) {
                return 1;
            } else {
                return light_mode_ - 2;
            }
        }));

            // MCP tool: Reduce display brightness
        mcp_server.AddTool("self.display.reduce_brightness", "降低屏幕亮度", PropertyList(), mcp_wrapper([this](const PropertyList& properties) -> ReturnValue {
            auto backlight = GetBacklight();
            if (backlight) {
                backlight->SetBrightness(10, true); // Set brightness to 10 (example value)
                ESP_LOGI(TAG, "屏幕亮度已降低");
                return true;
            } else {
                ESP_LOGE(TAG, "Backlight 控制不可用");
                return false;
            }
        }));

        mcp_server.AddTool("self.chassis.go_forward", "前进", PropertyList(), mcp_wrapper([this](const PropertyList& properties) -> ReturnValue {
            SendUartMessage("x0.0 y1.0");
            return true;
        }));

        mcp_server.AddTool("self.chassis.go_back", "后退", PropertyList(), mcp_wrapper([this](const PropertyList& properties) -> ReturnValue {
            SendUartMessage("x0.0 y-1.0");
            return true;
        }));

        mcp_server.AddTool("self.chassis.turn_left", "向左转", PropertyList(), mcp_wrapper([this](const PropertyList& properties) -> ReturnValue {
            SendUartMessage("x-1.0 y0.0");
            return true;
        }));

        mcp_server.AddTool("self.chassis.turn_right", "向右转", PropertyList(), mcp_wrapper([this](const PropertyList& properties) -> ReturnValue {
            SendUartMessage("x1.0 y0.0");
            return true;
        }));
        
        mcp_server.AddTool("self.chassis.dance", "跳舞", PropertyList(), mcp_wrapper([this](const PropertyList& properties) -> ReturnValue {
            SendUartMessage("d1");
            light_mode_ = LIGHT_MODE_MAX;
            return true;
        }));
        // AddTool("self.screen.set_theme",
        //     "Set the theme of the screen. The theme can be `light` or `dark`.",
        //     PropertyList({
        //         Property("theme", kPropertyTypeString)
        //     }),
        //     [display](const PropertyList& properties) -> ReturnValue {
        //         display->SetTheme(properties["theme"].value<std::string>().c_str());
        //         return true;
        //     });
        // // 实现mcp方法给安卓发送http请求，请求格式http://192.168.2.240:8080?type=llm&text=文本
        // 将 text 参数做成表情的枚举，只允许传入几个固定的表情类型
        // 允许的表情类型: "smile", "sad", "angry", "surprised", "love"
        mcp_server.AddTool("self.android.make_emoji", 
            "做个表情，表情可以是: `smile`, `sad`, `angry`, `surprised`, `love`", PropertyList({
            Property("emoji", kPropertyTypeString)
        }), [this](const PropertyList& properties) -> ReturnValue {
            static const std::set<std::string> allowed_emojis = {
            "smile", "sad", "angry", "surprised", "love"
            };
            std::string emoji = properties["emoji"].value<std::string>();
            if (allowed_emojis.find(emoji) == allowed_emojis.end()) {
            throw std::runtime_error("不支持的表情类型，只能是: smile, sad, angry, surprised, love");
            }
            ShowAndroidEmoji(emoji);
            return "已发送表情: " + emoji;
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
            
            // 添加调试信息
            ESP_LOGI("XiangLaoHui", "Response length: %d", response.length());
            ESP_LOGI("XiangLaoHui", "Response first 100 chars: %.100s", response.c_str());
            
            // 检查响应是否为空
            if (response.empty()) {
                ESP_LOGE("XiangLaoHui", "Empty response received");
                return std::string("❌ 服务器返回空响应");
            }
            
            // 解析响应数据
            cJSON* json = cJSON_Parse(response.c_str());
            if (!json) {
                ESP_LOGE("XiangLaoHui", "Failed to parse response JSON");
                ESP_LOGE("XiangLaoHui", "cJSON error: %s", cJSON_GetErrorPtr());
                ESP_LOGE("XiangLaoHui", "Raw response: %s", response.c_str());
                return std::string("❌ 响应数据解析失败 - JSON格式错误");
            }
            
            // 检查返回状态，API成功时没有return_code字段，直接解析数据
            cJSON* serial_number = cJSON_GetObjectItem(json, "serial_number");
            cJSON* mobile_phone_resp = cJSON_GetObjectItem(json, "mobile_phone");
            
            if (cJSON_IsString(serial_number) && cJSON_IsString(mobile_phone_resp)) {
                // 成功获取健康数据，构建友好的摘要
                std::string health_summary = "🏥 健康数据查询成功!\n\n";
                health_summary += "📱 手机号: " + std::string(mobile_phone_resp->valuestring) + "\n";
                health_summary += "📋 流水号: " + std::string(serial_number->valuestring) + "\n\n";
                
                // 解析各项健康数据
                cJSON* heart_rate = cJSON_GetObjectItem(json, "heart_rate");
                cJSON* blood_glucose = cJSON_GetObjectItem(json, "blood_glucose");
                cJSON* oxygen_saturation = cJSON_GetObjectItem(json, "oxygen_saturation");
                cJSON* body_temperature = cJSON_GetObjectItem(json, "body_temperature");
                cJSON* blood_pressure = cJSON_GetObjectItem(json, "blood_pressure");
                cJSON* response_time = cJSON_GetObjectItem(json, "response_time");
                cJSON* return_code = cJSON_GetObjectItem(json, "return_code");
                cJSON* return_msg = cJSON_GetObjectItem(json, "return_msg");
                
                //尽可能简洁的总结health_summary
                if (cJSON_IsString(heart_rate)) {
                    health_summary += "心率: " + std::string(heart_rate->valuestring) + " bpm ";
                }
                if (cJSON_IsString(blood_pressure)) {
                    health_summary += "血压: " + std::string(blood_pressure->valuestring) + " mmHg ";
                }
                if (cJSON_IsString(oxygen_saturation)) {
                    health_summary += "血氧: " + std::string(oxygen_saturation->valuestring) + "% ";
                }
                if (cJSON_IsString(body_temperature)) {
                    health_summary += "体温: " + std::string(body_temperature->valuestring) + "°C ";
                }
                if (cJSON_IsString(blood_glucose)) {
                    health_summary += "血糖: " + std::string(blood_glucose->valuestring) + " mg/dL ";
                }
                if (cJSON_IsString(return_code)) {
                    health_summary += "状态: " + std::string(return_code->valuestring) + " ";
                }
                if (cJSON_IsString(return_msg)) {
                    health_summary += "(" + std::string(return_msg->valuestring) + ")";
                }
                if (cJSON_IsString(response_time)) {
                    health_summary += " 时间: " + std::string(response_time->valuestring);
                }
                
                cJSON_Delete(json);
                ESP_LOGI("XiangLaoHui", "Health data retrieved successfully for: %s", mobile_phone.c_str());
                return health_summary;
            } else {
                // 数据格式不正确或解析失败
                std::string error_msg = "❌ 数据格式解析失败";
                
                // 尝试检查是否有错误信息
                cJSON* return_code = cJSON_GetObjectItem(json, "return_code");
                cJSON* return_msg = cJSON_GetObjectItem(json, "return_msg");
                
                if (cJSON_IsString(return_code)) {
                    error_msg += "\n状态码: " + std::string(return_code->valuestring);
                }
                if (cJSON_IsString(return_msg)) {
                    error_msg += "\n错误信息: " + std::string(return_msg->valuestring);
                }
                
                cJSON_Delete(json);
                ESP_LOGE("XiangLaoHui", "Health data parsing failed: %s", error_msg.c_str());
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
            
            ESP_LOGI("Camera", "准备为 %s 添加人脸数据", person_name.c_str());               
            // 保存到阿里云人脸数据库
            auto& app = Application::GetInstance();
            std::string response = app.AddFaceToAliyunDB(person_name);
            
            // 检查是否成功
            cJSON* root = cJSON_Parse(response.c_str());
            bool success = false;
            if (root) {
                cJSON* code = cJSON_GetObjectItem(root, "status");
                if (cJSON_IsString(code) && strcmp(code->valuestring, "success") == 0) {
                    success = true;
                }
                cJSON_Delete(root);
            }
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
        // Home Assistant 客厅温度获取工具
        // 工具名: self.homeassistant.get_living_room_temperature
        // 功能: 通过 Home Assistant API 获取客厅温度传感器的当前温度值。
        // 参数: 无（entity_id 和 token 在代码中占位，需后续替换）
        // 返回: 温度数值字符串（如 "25.3"），便于大语言模型处理
        mcp_server.AddTool("self.homeassistant.get_living_room_temperature", "获取客厅温度", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            std::string ha_url = "http://192.168.100.143:8123"; // Home Assistant 地址（占位）
            std::string token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiI4Y2E1NzFlZjU0M2Y0ODlhODE2YmFmYTdjZmYyZjcwOSIsImlhdCI6MTc1NjAzODgxNSwiZXhwIjoyMDcxMzk4ODE1fQ.ggOgXgnu9atig_hK34ucsCP-9HLYrvNaqJFhFyRe_BI"; // Token（占位）
            std::string entity_id = "sensor.miaomiaoc_cn_blt_3_1m2cf17fcck00_t9_temperature_p_3_1001"; // 客厅温度传感器实体ID（占位）

            std::string response = GetHomeAssistantState(ha_url, token, entity_id);

            // 解析温度值（假设返回JSON格式，属性为 state）
            cJSON* json = cJSON_Parse(response.c_str());
            if (!json) {
                return std::string(""); // 解析失败返回空字符串
            }
            cJSON* state = cJSON_GetObjectItem(json, "state");
            std::string result;
            if (cJSON_IsString(state)) {
                result = std::string(state->valuestring); // 只返回数值
            } else {
                result = std::string(""); // 未获取到温度值返回空字符串
            }
            cJSON_Delete(json);
            return result;
        });

        // Home Assistant 客厅湿度获取工具
        // 工具名: self.homeassistant.get_living_room_humidity
        // 功能: 通过 Home Assistant API 获取客厅湿度传感器的当前湿度值。
        // 参数: 无（entity_id 和 token 在代码中占位，需后续替换）
        // 返回: 湿度数值字符串（如 "45.2"），便于大语言模型处理
        mcp_server.AddTool("self.homeassistant.get_living_room_humidity", "获取客厅湿度", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            std::string ha_url = "http://192.168.100.143:8123"; // Home Assistant 地址（占位）
            std::string token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiI4Y2E1NzFlZjU0M2Y0ODlhODE2YmFmYTdjZmYyZjcwOSIsImlhdCI6MTc1NjAzODgxNSwiZXhwIjoyMDcxMzk4ODE1fQ.ggOgXgnu9atig_hK34ucsCP-9HLYrvNaqJFhFyRe_BI"; // Token（占位）
            std::string entity_id = "sensor.miaomiaoc_cn_blt_3_1m2cf17fcck00_t9_relative_humidity_p_3_1002"; // 客厅湿度传感器实体ID（占位）

            std::string response = GetHomeAssistantState(ha_url, token, entity_id);

            // 解析湿度值（假设返回JSON格式，属性为 state）
            cJSON* json = cJSON_Parse(response.c_str());
            if (!json) {
                return std::string(""); // 解析失败返回空字符串
            }
            cJSON* state = cJSON_GetObjectItem(json, "state");
            std::string result;
            if (cJSON_IsString(state)) {
                result = std::string(state->valuestring); // 只返回数值
            } else {
                result = std::string(""); // 未获取到湿度值返回空字符串
            }
            cJSON_Delete(json);
            return result;
        });

        // Home Assistant 关闭客厅灯工具
        // 工具名: self.homeassistant.turn_off_living_room_light
        // 功能: 通过 Home Assistant API 关闭客厅灯（light.living_room），无需参数。
        // 返回: 操作结果字符串（如成功/失败/错误信息）
        mcp_server.AddTool("self.homeassistant.turn_off_living_room_light", "关闭客厅灯", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            std::string ha_url = "http://192.168.100.143:8123"; // Home Assistant 地址（占位）
            std::string token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiI4Y2E1NzFlZjU0M2Y0ODlhODE2YmFmYTdjZmYyZjcwOSIsImlhdCI6MTc1NjAzODgxNSwiZXhwIjoyMDcxMzk4ODE1fQ.ggOgXgnu9atig_hK34ucsCP-9HLYrvNaqJFhFyRe_BI"; // Token（占位）
            std::string entity_id = "light.zhuow_cn_2022232388_wy0a02_s_2_light"; // 客厅灯实体ID（占位）

            std::string service_url = ha_url + "/api/services/light/turn_off";
            std::string request_body = std::string("{\"entity_id\": \"") + entity_id + "\"}";

            esp_http_client_config_t config = {};
            config.url = service_url.c_str();
            config.method = HTTP_METHOD_POST;
            config.timeout_ms = 10000;
            config.crt_bundle_attach = esp_crt_bundle_attach;
            config.transport_type = HTTP_TRANSPORT_OVER_SSL;

            esp_http_client_handle_t client = esp_http_client_init(&config);
            std::string auth_header = "Bearer " + token;
            esp_http_client_set_header(client, "Authorization", auth_header.c_str());
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, request_body.c_str(), request_body.length());

            esp_err_t err = esp_http_client_open(client, request_body.length());
            if (err != ESP_OK) {
                esp_http_client_cleanup(client);
                return std::string("❌ 无法连接 Home Assistant: ") + esp_err_to_name(err);
            }

            int write_len = esp_http_client_write(client, request_body.c_str(), request_body.length());
            if (write_len < 0) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return std::string("❌ 请求发送失败");
            }

            int status_code = esp_http_client_get_status_code(client);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            return std::string("✅ 客厅灯已关闭");

        });
        // Home Assistant 打开客厅灯工具
        // 工具名: self.homeassistant.turn_on_living_room_light
        // 功能: 通过 Home Assistant API 打开客厅灯（light.living_room），无需参数。
        // 返回: 操作结果字符串（如成功/失败/错误信息）
        mcp_server.AddTool("self.homeassistant.turn_on_living_room_light", "打开客厅灯", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            std::string ha_url = "http://192.168.100.143:8123"; // Home Assistant 地址（占位）
            std::string token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiI4Y2E1NzFlZjU0M2Y0ODlhODE2YmFmYTdjZmYyZjcwOSIsImlhdCI6MTc1NjAzODgxNSwiZXhwIjoyMDcxMzk4ODE1fQ.ggOgXgnu9atig_hK34ucsCP-9HLYrvNaqJFhFyRe_BI"; // Token（占位）
            std::string entity_id = "light.zhuow_cn_2022232388_wy0a02_s_2_light"; // 客厅灯实体ID（占位）

            std::string service_url = ha_url + "/api/services/light/turn_on";
            std::string request_body = std::string("{\"entity_id\": \"") + entity_id + "\"}";

            esp_http_client_config_t config = {};
            config.url = service_url.c_str();
            config.method = HTTP_METHOD_POST;
            config.timeout_ms = 10000;
            config.crt_bundle_attach = esp_crt_bundle_attach;
            config.transport_type = HTTP_TRANSPORT_OVER_SSL;

            esp_http_client_handle_t client = esp_http_client_init(&config);
            std::string auth_header = "Bearer " + token;
            esp_http_client_set_header(client, "Authorization", auth_header.c_str());
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, request_body.c_str(), request_body.length());

            esp_err_t err = esp_http_client_open(client, request_body.length());
            if (err != ESP_OK) {
                esp_http_client_cleanup(client);
                return std::string("❌ 无法连接 Home Assistant: ") + esp_err_to_name(err);
            }

            int write_len = esp_http_client_write(client, request_body.c_str(), request_body.length());
            if (write_len < 0) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return std::string("❌ 请求发送失败");
            }

            int status_code = esp_http_client_get_status_code(client);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            return std::string("✅ 客厅灯已打开");
        });
        // MCP tool: Adjust Home Assistant light brightness
        mcp_server.AddTool("self.homeassistant.set_living_room_light_brightness", "设置客厅灯亮度", PropertyList({
            Property("brightness", kPropertyTypeInteger, 0, 255)
        }), [this](const PropertyList& properties) -> ReturnValue {
            std::string ha_url = "http://192.168.100.143:8123"; // Home Assistant 地址（占位）
            std::string token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiI4Y2E1NzFlZjU0M2Y0ODlhODE2YmFmYTdjZmYyZjcwOSIsImlhdCI6MTc1NjAzODgxNSwiZXhwIjoyMDcxMzk4ODE1fQ.ggOgXgnu9atig_hK34ucsCP-9HLYrvNaqJFhFyRe_BI"; // Token（占位）
            std::string entity_id = "light.zhuow_cn_2022232388_wy0a02_s_2_light"; // 客厅灯实体ID（占位）

            int brightness = properties["brightness"].value<int>();
            std::string service_url = ha_url + "/api/services/light/turn_on";
            std::stringstream request_body;
            request_body << "{\"entity_id\": \"" << entity_id << "\", \"brightness\": " << brightness << "}";

            esp_http_client_config_t config = {};
            config.url = service_url.c_str();
            config.method = HTTP_METHOD_POST;
            config.timeout_ms = 10000;
            config.crt_bundle_attach = esp_crt_bundle_attach;
            config.transport_type = HTTP_TRANSPORT_OVER_SSL;

            esp_http_client_handle_t client = esp_http_client_init(&config);
            std::string auth_header = "Bearer " + token;
            esp_http_client_set_header(client, "Authorization", auth_header.c_str());
            esp_http_client_set_header(client, "Content-Type", "application/json");
            std::string body_str = request_body.str();
            esp_http_client_set_post_field(client, body_str.c_str(), body_str.length());

            esp_err_t err = esp_http_client_open(client, body_str.length());
            if (err != ESP_OK) {
                esp_http_client_cleanup(client);
                return std::string("❌ 无法连接 Home Assistant: ") + esp_err_to_name(err);
            }

            int write_len = esp_http_client_write(client, body_str.c_str(), body_str.length());
            if (write_len < 0) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return std::string("❌ 请求发送失败");
            }

            int status_code = esp_http_client_get_status_code(client);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            return std::string("✅ 客厅灯亮度已设置为: ") + std::to_string(brightness);

        });
    }

public:
    EspSparkBot() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeGpio();
        InitializeI2c();
        initializeIRSensor();
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
