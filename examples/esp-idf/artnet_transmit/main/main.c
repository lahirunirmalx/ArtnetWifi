/*
 * Art-Net transmit example.
 *
 * Sends a single universe with 3 channels, ramping an RGB lamp up to white.
 * Counterpart of the upstream ArtnetWifiTransmit sketch.
 *
 * This example may be copied under the terms of the MIT license, see the
 * LICENSE file for details.
 */

#include "artnet.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_connect.h"

#define WIFI_SSID   "ssid"     /* CHANGE FOR YOUR SETUP */
#define WIFI_PASS   "pAsSwOrD" /* CHANGE FOR YOUR SETUP */
#define ARTNET_HOST "2.1.1.1"  /* CHANGE FOR YOUR SETUP, your destination */
#define START_UNIVERSE 0

static const char *TAG = "example";

void app_main(void)
{
    artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
    artnet_handle_t artnet = NULL;

    ESP_ERROR_CHECK(wifi_connect(WIFI_SSID, WIFI_PASS));

    cfg.host = ARTNET_HOST;
    cfg.tx_only = true; /* nothing here reads, so do not queue the LAN's Art-Net traffic */
    ESP_ERROR_CHECK(artnet_init(&cfg, &artnet));

    artnet_set_universe(artnet, START_UNIVERSE);
    artnet_set_length(artnet, 3);

    ESP_LOGI(TAG, "sending to %s universe %d", ARTNET_HOST, START_UNIVERSE);

    while (true) {
        for (int level = 0; level < 256; level++) {
            for (uint16_t ch = 0; ch < 3; ch++) {
                artnet_set_byte(artnet, ch, (uint8_t)level);
            }
            if (artnet_write(artnet) != ESP_OK) {
                ESP_LOGE(TAG, "send failed");
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
