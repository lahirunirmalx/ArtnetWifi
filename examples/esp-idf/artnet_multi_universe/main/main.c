/*
 * Art-Net multi-universe example.
 *
 * Assembles a 240 LED strip (720 channels, 2 universes) from Art-Net and hands
 * each complete frame to a render task. Counterpart of the upstream FastLED and
 * NeoPixel sketches, minus the LED driver: the render task is a stub that logs
 * frame statistics, with a comment where led_strip / RMT output goes.
 *
 * The shape is the point. The DMX callback runs on the receive task and must
 * stay short, so it only copies channels into a staging buffer. When every
 * universe of a frame has landed, the whole frame is posted to a length-1
 * queue and the render task wakes. The two tasks never share a buffer.
 *
 * This example may be copied under the terms of the MIT license, see the
 * LICENSE file for details.
 */

#include <inttypes.h>
#include <string.h>

#include "artnet.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "wifi_connect.h"

#define WIFI_SSID "ssid"     /* CHANGE FOR YOUR SETUP */
#define WIFI_PASS "pAsSwOrD" /* CHANGE FOR YOUR SETUP */

#define NUM_LEDS       240   /* CHANGE FOR YOUR SETUP */
#define START_UNIVERSE 0     /* CHANGE FOR YOUR SETUP, most software starts at 1, some at 0 */

/* 512 channels is 170 RGB LEDs with 2 bytes spare; use 510 per universe so a
 * LED never straddles two packets. */
#define CHANNELS_PER_UNIVERSE 510
#define NUM_CHANNELS          (NUM_LEDS * 3)
#define NUM_UNIVERSES         ((NUM_CHANNELS + CHANNELS_PER_UNIVERSE - 1) / CHANNELS_PER_UNIVERSE)

static const char *TAG = "example";

typedef struct {
    uint8_t rgb[NUM_CHANNELS];
} frame_t;

static QueueHandle_t s_frames;               /* length 1, newest frame wins */
static frame_t       s_staging;              /* written by the callback only */
static uint32_t      s_universes_seen;       /* bit per universe of the current frame */
static const uint32_t s_all_universes = (1u << NUM_UNIVERSES) - 1;

static void on_dmx(const artnet_dmx_t *frame, void *user_ctx)
{
    uint32_t index;
    size_t offset;
    size_t count;

    (void)user_ctx;

    /* Unsigned subtraction: a universe below START_UNIVERSE wraps to a huge
     * index and fails the same range check as one above the end. */
    index = (uint32_t)frame->universe - START_UNIVERSE;
    if (index >= NUM_UNIVERSES) {
        return; /* not ours */
    }
    offset = index * CHANNELS_PER_UNIVERSE;

    count = frame->length < CHANNELS_PER_UNIVERSE ? frame->length : CHANNELS_PER_UNIVERSE;
    if (offset + count > NUM_CHANNELS) {
        count = NUM_CHANNELS - offset; /* the last universe is usually partial */
    }
    memcpy(s_staging.rgb + offset, frame->data, count);

    s_universes_seen |= 1u << index;
    if (s_universes_seen == s_all_universes) {
        /* Complete frame. Copy it out and wake the renderer; if the renderer
         * is behind, the older unrendered frame is simply replaced. */
        xQueueOverwrite(s_frames, &s_staging);
        s_universes_seen = 0;
    }
}

static void render_task(void *arg)
{
    frame_t frame;
    uint32_t rendered = 0;
    uint32_t sum = 0;
    TickType_t last_report = xTaskGetTickCount();

    (void)arg;

    while (true) {
        if (xQueueReceive(s_frames, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /*
         * Push frame.rgb to the LEDs here, for example with the led_strip
         * component:
         *   for (int i = 0; i < NUM_LEDS; i++)
         *       led_strip_set_pixel(strip, i, frame.rgb[3*i], frame.rgb[3*i+1], frame.rgb[3*i+2]);
         *   led_strip_refresh(strip);
         * This stub just keeps statistics.
         */
        sum = 0;
        for (size_t i = 0; i < NUM_CHANNELS; i++) {
            sum += frame.rgb[i];
        }
        rendered++;

        if (xTaskGetTickCount() - last_report >= pdMS_TO_TICKS(1000)) {
            ESP_LOGI(TAG, "%" PRIu32 " frames/s, average channel level %" PRIu32,
                     rendered, sum / NUM_CHANNELS);
            rendered = 0;
            last_report = xTaskGetTickCount();
        }
    }
}

void app_main(void)
{
    artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
    artnet_task_config_t tcfg = ARTNET_TASK_CONFIG_DEFAULT();
    artnet_handle_t artnet = NULL;

    ESP_ERROR_CHECK(wifi_connect(WIFI_SSID, WIFI_PASS));

    s_frames = xQueueCreate(1, sizeof(frame_t));
    configASSERT(s_frames != NULL);

    /* Renderer below the receiver in priority, so a burst of universes is
     * drained from lwIP before any pixels are pushed. */
    xTaskCreate(render_task, "render", 4096, NULL, 4, NULL);

    /* Describe the node so controllers list it with the right universes. */
    cfg.node.short_name = "ESP strip";
    cfg.node.long_name = "ESP-IDF Art-Net LED strip, 240 pixels";
    cfg.node.first_universe = START_UNIVERSE;
    cfg.node.num_ports = NUM_UNIVERSES;
    esp_wifi_get_mac(WIFI_IF_STA, cfg.node.mac);

    cfg.dmx_cb = on_dmx;
    ESP_ERROR_CHECK(artnet_init(&cfg, &artnet));

    tcfg.priority = 6;
    ESP_ERROR_CHECK(artnet_start_task(artnet, &tcfg));

    ESP_LOGI(TAG, "%d LEDs, %d channels, universes %d..%d",
             NUM_LEDS, NUM_CHANNELS, START_UNIVERSE, START_UNIVERSE + NUM_UNIVERSES - 1);
}
