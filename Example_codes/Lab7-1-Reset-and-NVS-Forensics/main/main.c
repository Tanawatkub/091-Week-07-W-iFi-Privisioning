#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"
#include "qrcode.h"

static const char *TAG = "LAB7_1_RESET";

#define FACTORY_RESET_BUTTON_GPIO  GPIO_NUM_18   // ปุ่ม Factory Reset (Active-Low ต่อลง GND)
#define LED_PIN_WIFI_STA           GPIO_NUM_2    // LED 1: สถานะ Wi-Fi Station (Onboard/External)

#define PROV_POP                   "abcd1234"    // Proof-of-Possession (Security 1)
#define PROV_QR_VERSION             "v1"

typedef enum {
    LED_STA_OFF = 0,
    LED_STA_CONNECTED,       // Heartbeat: ติด 200ms ในทุก 1 วินาที
    LED_STA_DISCONNECTED     // Alert: ติด 200ms ดับ 200ms
} led_mode_t;

static volatile led_mode_t g_led_mode = LED_STA_OFF;

/* FreeRTOS Background Task สำหรับกระพริบไฟ LED 1 */
static void led_status_task(void *pvParameters)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN_WIFI_STA),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    uint32_t tick = 0;
    while (1) {
        if (g_led_mode == LED_STA_CONNECTED) {
            gpio_set_level(LED_PIN_WIFI_STA, (tick % 10 < 2) ? 1 : 0);
        } else if (g_led_mode == LED_STA_DISCONNECTED) {
            gpio_set_level(LED_PIN_WIFI_STA, (tick % 4 < 2) ? 1 : 0);
        } else {
            gpio_set_level(LED_PIN_WIFI_STA, 0);
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ตรวจสอบการกดปุ่ม GPIO 18 ค้างไว้ 3 วินาที (Active Low) */
static bool check_factory_reset_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FACTORY_RESET_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Hold GPIO 18 button for 3 seconds to trigger Factory Reset...");
    int hold_count = 0;
    while (gpio_get_level(FACTORY_RESET_BUTTON_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        hold_count++;
        if (hold_count % 10 == 0) {
            ESP_LOGI(TAG, "Holding button... %d/3 seconds", hold_count / 10);
        }
        if (hold_count >= 30) {
            ESP_LOGW(TAG, "=================================================");
            ESP_LOGW(TAG, ">>> FACTORY RESET TRIGGERED! ERASING NVS FLASH <<<");
            ESP_LOGW(TAG, "=================================================");
            return true;
        }
    }
    return false;
}

/* สร้างชื่อ SoftAP อัตโนมัติจาก MAC Address เช่น PROV_A1B2C3 */
static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "PROV_%02X%02X%02X", eth_mac[3], eth_mac[4], eth_mac[5]);
}

/* พิมพ์ QR Code แบบ ASCII บน Serial Monitor */
static void print_provisioning_qr(const char *service_name)
{
#ifdef CONFIG_EXAMPLE_PROV_SHOW_QR
    char payload[200] = {0};
    snprintf(payload, sizeof(payload),
             "{\"ver\":\"%s\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"softap\"}",
             PROV_QR_VERSION, service_name, PROV_POP);

    ESP_LOGI(TAG, "Scan this QR code from the ESP BLE Provisioning app:");
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    esp_qrcode_generate(&cfg, payload);
    ESP_LOGI(TAG, "If QR code is not visible, use manual config:");
    ESP_LOGI(TAG, "  SSID (AP)  : %s", service_name);
    ESP_LOGI(TAG, "  PoP        : %s", PROV_POP);
#endif
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        g_led_mode = LED_STA_DISCONNECTED;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi Disconnected. Connecting again...");
        g_led_mode = LED_STA_DISCONNECTED;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "[ONLINE]: Connected with IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "=================================================");
        g_led_mode = LED_STA_CONNECTED;
    } else if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
            case NETWORK_PROV_WIFI_CRED_RECV:
                ESP_LOGI(TAG, "Received Wi-Fi credentials");
                break;
            case NETWORK_PROV_WIFI_CRED_FAIL:
                ESP_LOGE(TAG, "Provisioning failed! Please reset to factory and retry.");
                break;
            case NETWORK_PROV_WIFI_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
                break;
            case NETWORK_PROV_END:
                network_prov_mgr_deinit();
                break;
            default:
                break;
        }
    }
}

void app_main(void)
{
    xTaskCreate(led_status_task, "led_task", 2048, NULL, 5, NULL);

    // 1. ตรวจสอบการกดปุ่ม GPIO 18 ทางกายภาพ (ตอนที่ 3: Hardware Reset)
    if (check_factory_reset_button()) {
        ESP_LOGW(TAG, "[FORENSIC]: User requested Flash Erase!");
        ESP_ERROR_CHECK(nvs_flash_erase());
    }

    // 2. เริ่มต้นระบบ NVS Flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 3. เริ่มต้น Network Interface และ Event Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    // 4. เริ่มต้น Provisioning Manager
    network_prov_mgr_config_t prov_cfg = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(prov_cfg));

    // 5. ตอนที่ 2: บังคับ Reset ผ่าน Menuconfig Flag
#ifdef CONFIG_EXAMPLE_RESET_PROVISIONED
    ESP_LOGW(TAG, ">>> CONFIG_EXAMPLE_RESET_PROVISIONED is enabled: forcing reset <<<");
    network_prov_mgr_reset_wifi_provisioning();
#endif

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        ESP_LOGW(TAG, "--------------------------------------------------");
        ESP_LOGW(TAG, "[STATUS]: Device is NOT provisioned (NVS is empty)");
        ESP_LOGW(TAG, "--------------------------------------------------");

        char service_name[16];
        get_device_service_name(service_name, sizeof(service_name));

        ESP_LOGI(TAG, "Starting provisioning");
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_1, PROV_POP, service_name, NULL));

        print_provisioning_qr(service_name);
    } else {
        ESP_LOGI(TAG, "--------------------------------------------------");
        ESP_LOGI(TAG, "[STATUS]: Already provisioned! Starting Wi-Fi Station");
        ESP_LOGI(TAG, "--------------------------------------------------");
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}