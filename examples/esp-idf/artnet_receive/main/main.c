/*
 * Art-Net receive example.
 *
 * Joins a Wi-Fi network, listens on UDP 6454 and logs incoming ArtDmx frames.
 *
 * Logging is rate limited on purpose. A log line costs milliseconds of
 * blocking UART time, and the callback runs on the receive path, so logging
 * every packet of a multi-universe rig would make the node drop frames and
 * blame the network. Every frame is still counted.
 *
 * This example may be copied under the terms of the MIT license, see the
 * LICENSE file for details.
 */

#include <inttypes.h>

#include "artnet.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_connect.h"

#define WIFI_SSID "ssid"     /* CHANGE FOR YOUR SETUP */
#define WIFI_PASS "pAsSwOrD" /* CHANGE FOR YOUR SETUP */

#define LOG_INTERVAL_MS 200  /* at most 5 log lines per second */
#define LOG_BYTES       16   /* channels shown per logged frame */

static const char *TAG = "example";

static uint32_t   s_frames;
static uint32_t   s_skipped;
static TickType_t s_last_log;

static void on_dmx(const artnet_dmx_t *frame, void *user_ctx)
{
    TickType_t now = xTaskGetTickCount();

    (void)user_ctx;
    s_frames++;

    if (now - s_last_log < pdMS_TO_TICKS(LOG_INTERVAL_MS)) {
        s_skipped++;
        return;
    }
    s_last_log = now;

    ESP_LOGI(TAG, "DMX: Univ: %u, Seq: %u, Len: %u, frame %" PRIu32 " (%" PRIu32 " not shown)",
             frame->universe, frame->sequence, frame->length, s_frames, s_skipped);
    ESP_LOG_BUFFER_HEX(TAG, frame->data, frame->length < LOG_BYTES ? frame->length : LOG_BYTES);
    s_skipped = 0;
}

void app_main(void)
{
    artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
    artnet_handle_t artnet = NULL;

    ESP_ERROR_CHECK(wifi_connect(WIFI_SSID, WIFI_PASS));

    cfg.dmx_cb = on_dmx;
    cfg.node.short_name = "ESP debug"; /* what controllers list this node as */
    cfg.node.ip = wifi_connect_ip();   /* and the address they list it under */
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
