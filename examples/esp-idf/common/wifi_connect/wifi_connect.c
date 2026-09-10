/*
 * Minimal Wi-Fi station helper shared by the Art-Net examples. MIT licensed.
 *
 * ESP-IDF ships a richer version of this as examples/common_components/
 * protocol_examples_common (example_connect(), menuconfig driven, Ethernet and
 * IPv6 aware). This copy exists so the examples build with no Kconfig step and
 * stay readable in one file; if you outgrow it, switch to example_connect().
 */

#include "wifi_connect.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_events;
static int                s_retry_count;
static uint32_t           s_ip;   /* last address assigned, network byte order */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Retry forever. A lighting node with no network is useless, and a
         * panic-reboot loop would only add a backtrace to every attempt. The
         * driver spaces reconnects itself, so no delay is needed here. */
        s_retry_count++;
        ESP_LOGW(TAG, "disconnected, reconnecting (attempt %d)", s_retry_count);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;

        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        s_ip = event->ip_info.ip.addr;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_connect(const char *ssid, const char *password)
{
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t sta_cfg = { 0 };
    size_t ssid_len;
    size_t pass_len;
    esp_err_t err;

    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The driver fields are fixed size and need no terminator when full, so a
     * 32 character SSID and a 64 hex digit PSK are both legal. Anything longer
     * is a configuration error, not something to truncate quietly. */
    ssid_len = strnlen(ssid, sizeof(sta_cfg.sta.ssid) + 1);
    pass_len = strnlen(password, sizeof(sta_cfg.sta.password) + 1);
    if (ssid_len == 0 || ssid_len > sizeof(sta_cfg.sta.ssid) ||
        pass_len > sizeof(sta_cfg.sta.password)) {
        ESP_LOGE(TAG, "SSID must be 1..32 bytes and the password at most 64");
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    memcpy(sta_cfg.sta.ssid, ssid, ssid_len);
    memcpy(sta_cfg.sta.password, password, pass_len);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to '%s'", ssid);
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    return ESP_OK;
}

uint32_t wifi_connect_ip(void)
{
    return s_ip;
}
