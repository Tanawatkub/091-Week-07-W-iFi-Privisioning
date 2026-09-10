#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "network_provisioning/manager.h"      // เดิม: "wifi_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"    // เดิม: "wifi_provisioning/scheme_ble.h"
#include "protocomm_security.h"                 // ยังคงชื่อเดิม ไม่ถูกย้าย/เปลี่ยนชื่อใน v6.0

static const char *TAG = "LAB7_4_CUSTOM";

#define LED_PIN_WIFI_STA     GPIO_NUM_2    // LED 1: Wi-Fi STA Status
#define LED_PIN_BLE_PROV     GPIO_NUM_4    // LED 2: BLE Status
#define PROV_POP_KEY         "abcd1234"   // Proof-of-Possession (PoP)

/* Handler สำหรับ Custom Endpoint: "custom-data" */
esp_err_t custom_prov_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                   uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    if (inbuf) {
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "[CUSTOM DATA RECEIVED]: %.*s", (int)inlen, (char *)inbuf);
        ESP_LOGI(TAG, "=================================================");
    }

    // สร้างข้อความตอบกลับแบบไดนามิกบน Heap (Protocomm จะ Free ให้อัตโนมัติ)
    char response[] = "ACK_FROM_ESP32_SUCCESS";
    *outbuf = (uint8_t *)strdup(response);
    if (*outbuf == NULL) {
        ESP_LOGE(TAG, "Heap out of memory");
        return ESP_ERR_NO_MEM;
    }
    *outlen = strlen(response) + 1;

    return ESP_OK;
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        if (event_id == PROTOCOMM_SECURITY_SESSION_SETUP_OK) {
            ESP_LOGI(TAG, "--------------------------------------------------");
            ESP_LOGI(TAG, "[SECURITY SUCCESS]: Valid PoP! Secured Session OK!");
            ESP_LOGI(TAG, "--------------------------------------------------");
        } else if (event_id == PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH) {
            ESP_LOGE(TAG, "--------------------------------------------------");
            ESP_LOGE(TAG, "[SECURITY ALERT]: INVALID PoP / Unauthorized Access!");
            ESP_LOGE(TAG, "--------------------------------------------------");
        }
    } else if (event_base == NETWORK_PROV_EVENT) {                    // เดิม: WIFI_PROV_EVENT
        if (event_id == NETWORK_PROV_WIFI_CRED_SUCCESS) {             // เดิม: WIFI_PROV_CRED_SUCCESS
            ESP_LOGI(TAG, "[SUCCESS]: Provisioning Completed!");
            gpio_set_level(LED_PIN_BLE_PROV, 0);
        } else if (event_id == NETWORK_PROV_END) {                    // เดิม: WIFI_PROV_END
            network_prov_mgr_deinit();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "[ONLINE]: IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        gpio_set_level(LED_PIN_WIFI_STA, 1);
    }
}

void app_main(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN_WIFI_STA) | (1ULL << LED_PIN_BLE_PROV),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));               // เดิม: WIFI_PROV_EVENT
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    network_prov_mgr_config_t config = {                              // เดิม: wifi_prov_mgr_config_t
        .scheme = network_prov_scheme_ble,                            // เดิม: wifi_prov_scheme_ble
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM  // เดิม: WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));                   // เดิม: wifi_prov_mgr_init

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));  // เดิม: wifi_prov_mgr_is_provisioned

    if (!provisioned) {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        char service_name[16];
        snprintf(service_name, sizeof(service_name), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);

        uint8_t custom_service_uuid[] = {
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
            0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
        };
        network_prov_scheme_ble_set_service_uuid(custom_service_uuid);   // เดิม: wifi_prov_scheme_ble_set_service_uuid

        // 1. สร้าง Custom Endpoint ก่อนสั่ง Start Provisioning
        network_prov_mgr_endpoint_create("custom-data");                 // เดิม: wifi_prov_mgr_endpoint_create

        // 2. เริ่มต้น Provisioning Service
        network_prov_security_t security = NETWORK_PROV_SECURITY_1;      // เดิม: wifi_prov_security_t / WIFI_PROV_SECURITY_1
        const char *pop = PROV_POP_KEY;
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(security, (const void *)pop, service_name, NULL));  // เดิม: wifi_prov_mgr_start_provisioning

        // 3. ผูกฟังก์ชัน Callback เข้ากับ Endpoint หลัง Start Service
        network_prov_mgr_endpoint_register("custom-data", custom_prov_data_handler, NULL);  // เดิม: wifi_prov_mgr_endpoint_register

        gpio_set_level(LED_PIN_BLE_PROV, 1);

        ESP_LOGI(TAG, "--------------------------------------------------");
        ESP_LOGI(TAG, "[QR CODE URL]: Click or copy the URL below:");
        ESP_LOGI(TAG, "https://espressif.github.io/esp-jumpstart/qrcode.html?data=%%7B%%22ver%%22%%3A%%22v1%%22%%2C%%22name%%22%%3A%%22%s%%22%%2C%%22pop%%22%%3A%%22%s%%22%%2C%%22transport%%22%%3A%%22ble%%22%%7D",
                 service_name, pop);
        ESP_LOGI(TAG, "Payload JSON: {\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
                 service_name, pop);
        ESP_LOGI(TAG, "--------------------------------------------------");
    } else {
        ESP_LOGI(TAG, "Already provisioned! Starting Wi-Fi Station");
        network_prov_mgr_deinit();                                       // เดิม: wifi_prov_mgr_deinit
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
}