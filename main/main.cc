#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_system.h>

#include "application.h"
#include "system_info.h"

#define TAG "main"

static void print_reset_reason(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", reason);
    
    switch (reason) {
        case ESP_RST_UNKNOWN:
            ESP_LOGI(TAG, "Reset reason: UNKNOWN");
            break;
        case ESP_RST_POWERON:
            ESP_LOGI(TAG, "Reset reason: POWER ON");
            break;
        case ESP_RST_EXT:
            ESP_LOGI(TAG, "Reset reason: EXTERNAL PIN");
            break;
        case ESP_RST_SW:
            ESP_LOGI(TAG, "Reset reason: SOFTWARE");
            break;
        case ESP_RST_PANIC:
            ESP_LOGI(TAG, "Reset reason: SOFTWARE PANIC");
            break;
        case ESP_RST_INT_WDT:
            ESP_LOGI(TAG, "Reset reason: INTERRUPT WATCHDOG");
            break;
        case ESP_RST_TASK_WDT:
            ESP_LOGI(TAG, "Reset reason: TASK WATCHDOG");
            break;
        case ESP_RST_WDT:
            ESP_LOGI(TAG, "Reset reason: OTHER WATCHDOG");
            break;
        case ESP_RST_DEEPSLEEP:
            ESP_LOGI(TAG, "Reset reason: DEEP SLEEP");
            break;
        case ESP_RST_BROWNOUT:
            ESP_LOGI(TAG, "Reset reason: BROWNOUT");
            break;
        case ESP_RST_SDIO:
            ESP_LOGI(TAG, "Reset reason: SDIO");
            break;
        default:
            ESP_LOGI(TAG, "Reset reason: OTHER (%d)", reason);
            break;
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== XIAOZHI ESP32 Starting ===");
    ESP_LOGI(TAG, "Free heap at startup: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Minimum free heap ever: %lu bytes", esp_get_minimum_free_heap_size());
    
    // 打印重启原因
    print_reset_reason();
    
    // Initialize the default event loop
    ESP_LOGI(TAG, "Initializing default event loop...");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "Event loop initialized");

    // Initialize NVS flash for WiFi configuration
    ESP_LOGI(TAG, "Initializing NVS flash...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS flash initialized");
    ESP_LOGI(TAG, "Free heap after NVS init: %lu bytes", esp_get_free_heap_size());

    // Launch the application
    ESP_LOGI(TAG, "Getting application instance...");
    auto& app = Application::GetInstance();
    ESP_LOGI(TAG, "Application instance obtained");
    ESP_LOGI(TAG, "Starting application...");
    app.Start();
    ESP_LOGI(TAG, "Application started, entering main event loop...");
    app.MainEventLoop();
}
