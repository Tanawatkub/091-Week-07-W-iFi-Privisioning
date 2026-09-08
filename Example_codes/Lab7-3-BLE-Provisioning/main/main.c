#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

static const char *TAG = "LAB7_3_BLE";

#define LED_PIN_WIFI_STA     GPIO_NUM_2    // LED 1: Wi-Fi STA Status
#define LED_PIN_BLE_PROV     GPIO_NUM_4    // LED 2: BLE Provisioning Status
#define PROV_POP_KEY         "abcd1234"   // Proof-of-Possession (PoP)

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
            case NETWORK_PROV_START:
                ESP_LOGI(TAG, "[PROV EVENT]: BLE Provisioning Started (Advertising)!");
                gpio_set_level(LED_PIN_BLE_PROV, 1);
                break;
            case NETWORK_PROV_WIFI_CRED_RECV: {
                wifi_sta_config_t *sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "=================================================");
                ESP_LOGI(TAG, "[BLE CREDENTIALS RECEIVED]:");
                ESP_LOGI(TAG, "  -> SSID     : %s", (const char *)sta_cfg->ssid);
                ESP_LOGI(TAG, "  -> Password : %s", (const char *)sta_cfg->password);
                ESP_LOGI(TAG, "=================================================");
                break;
            }
            case NETWORK_PROV_WIFI_CRED_FAIL: {
                network_prov_wifi_sta_fail_reason_t *reason =
                    (network_prov_wifi_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "[FAIL]: Provisioning failed! Reason: %s",
                         (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) ?
                         "Wi-Fi station authentication failed" : "Wi-Fi AP not found");
                break;
            }
            case NETWORK_PROV_WIFI_CRED_SUCCESS:
                ESP_LOGI(TAG, "[SUCCESS]: BLE Provisioning Successful!");
                gpio_set_level(LED_PIN_BLE_PROV, 0); // ปิด LED BLE
                break;
            case NETWORK_PROV_END:
                ESP_LOGI(TAG, "[PROV EVENT]: De-initializing BLE & Releasing BT Memory...");
                network_prov_mgr_deinit();
                break;
            default:
                break;
        }
    } else if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        if (event_id == PROTOCOMM_TRANSPORT_BLE_CONNECTED) {
            ESP_LOGI(TAG, "[BLE]: Smartphone Connected to GATT Server!");
        } else if (event_id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED) {
            ESP_LOGW(TAG, "[BLE]: Smartphone Disconnected from GATT Server");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "[ONLINE]: Connected to Wi-Fi with IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "=================================================");
        gpio_set_level(LED_PIN_WIFI_STA, 1);
    }
}

void app_main(void)
{
    // กำหนด GPIO Output สำหรับ LED 1 และ LED 2
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN_WIFI_STA) | (1ULL << LED_PIN_BLE_PROV),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_PIN_WIFI_STA, 0);
    gpio_set_level(LED_PIN_BLE_PROV, 0);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // กำหนดค่า Provisioning Manager เป็น BLE Scheme + คืน RAM BT เมื่อเสร็จ
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        char service_name[16];
        snprintf(service_name, sizeof(service_name), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);

        // กำหนด Custom 128-bit UUID สำหรับ GATT Service
        uint8_t custom_service_uuid[] = {
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
            0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
        };
        network_prov_scheme_ble_set_service_uuid(custom_service_uuid);

        ESP_LOGI(TAG, "Starting BLE Provisioning (Name: %s, PoP: %s)", service_name, PROV_POP_KEY);

        network_prov_security_t security = NETWORK_PROV_SECURITY_1;
        network_prov_security1_params_t *sec_params = PROV_POP_KEY;

        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(security, (const void *)sec_params, service_name, NULL));

        ESP_LOGI(TAG, "--------------------------------------------------");
        ESP_LOGI(TAG, "[QR CODE URL]: Click or copy the URL below:");
        ESP_LOGI(TAG, "https://espressif.github.io/esp-jumpstart/qrcode.html?data=%%7B%%22ver%%22%%3A%%22v1%%22%%2C%%22name%%22%%3A%%22%s%%22%%2C%%22pop%%22%%3A%%22%s%%22%%2C%%22transport%%22%%3A%%22ble%%22%%7D",
                 service_name, PROV_POP_KEY);
        ESP_LOGI(TAG, "Payload JSON: {\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
                 service_name, PROV_POP_KEY);
        ESP_LOGI(TAG, "--------------------------------------------------");
    } else {
        ESP_LOGI(TAG, "Already provisioned! Starting Wi-Fi Station");
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
}