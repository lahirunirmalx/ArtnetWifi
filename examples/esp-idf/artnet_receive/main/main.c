/*
 * Art-Net receive example.
 *
 * Joins a Wi-Fi network, listens on UDP 6454 and logs every ArtDmx frame.
 * Counterpart of the upstream ArtnetWifiDebug sketch.
 *
 * This example may be copied under the terms of the MIT license, see the
 * LICENSE file for details.
 */

#include <inttypes.h>
#include <stdio.h>

#include "artnet.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_connect.h"

#define WIFI_SSID "ssid"     /* CHANGE FOR YOUR SETUP */
#define WIFI_PASS "pAsSwOrD" /* CHANGE FOR YOUR SETUP */

static const char *TAG = "example";

static void on_dmx(const artnet_dmx_t *frame, void *user_ctx)
{
    char line[16 * 4 + 1];
    int pos = 0;

    (void)user_ctx;

    for (uint16_t i = 0; i < frame->length && i < 16; i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, "%u ", frame->data[i]);
    }

    ESP_LOGI(TAG, "DMX: Univ: %u, Seq: %u, Data (%u): %s%s",
             frame->universe, frame->sequence, frame->length, line,
             frame->length > 16 ? "..." : "");
}

void app_main(void)
{
    artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
    artnet_handle_t artnet = NULL;

    ESP_ERROR_CHECK(wifi_connect(WIFI_SSID, WIFI_PASS));

    cfg.dmx_cb = on_dmx;
    ESP_ERROR_CHECK(artnet_init(&cfg, &artnet));

    /* Blocking receive loop. Use artnet_start_task() to run it in the
     * background and keep app_main free for other work. */
    while (true) {
        uint16_t opcode = 0;
        esp_err_t err = artnet_read(artnet, 1000, &opcode);

        if (err == ESP_OK && opcode != ARTNET_OP_DMX) {
            ESP_LOGI(TAG, "non-DMX packet, opcode 0x%04x", opcode);
        }
    }
}
