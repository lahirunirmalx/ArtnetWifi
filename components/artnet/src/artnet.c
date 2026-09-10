/*The MIT License (MIT)

Copyright (c) 2014 Nathanael Lecaude
https://github.com/natcl/Artnet

Copyright (c) 2016,2019 Stephan Ruloff
https://github.com/rstephan/ArtnetWifi

Copyright (c) 2026 Lahiru Nirmal (ESP-IDF port)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "artnet.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/semphr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "artnet";

/* "Art-Net" plus the terminating zero, all 8 bytes are on the wire. */
static const char artnet_id[8] = ARTNET_ID;

/* Smallest packet that still carries ID, op-code and protocol version. */
#define ARTNET_MIN_PACKET 12

/* Poll interval of the receive task, and therefore the worst case stop latency. */
#define ARTNET_TASK_POLL_MS 100

/* Number of cores the target has, for validating a requested core_id. */
#if defined(configNUMBER_OF_CORES)
#define ARTNET_NUM_CORES configNUMBER_OF_CORES
#elif defined(portNUM_PROCESSORS)
#define ARTNET_NUM_CORES portNUM_PROCESSORS
#else
#define ARTNET_NUM_CORES 1
#endif

struct artnet_ctx {
    int sock;

    artnet_dmx_cb_t dmx_cb;
    void            *user_ctx;

    /* Receive state, describes the last packet handed to the application. */
    uint8_t  rx_buf[ARTNET_MAX_PACKET];
    uint16_t opcode;
    uint16_t rx_universe;
    uint16_t rx_length;
    uint8_t  rx_sequence;
    uint16_t rx_packet_size;
    uint32_t sender_ip;

    /* Transmit state, independent of the receive buffer. */
    uint8_t            tx_buf[ARTNET_MAX_PACKET];
    struct sockaddr_in dest;
    bool               dest_valid;
    bool               tx_failing; /* last sendto failed, error already reported */
    uint16_t           tx_length;
    uint16_t           tx_universe;
    uint8_t            physical;
    uint8_t            sequence;

    /* Requested SO_RCVTIMEO currently in effect, 0 when none applied yet.
     * A zero timeout never reaches the socket (it uses MSG_DONTWAIT), so 0 is
     * free to mean "not set". */
    uint32_t rcvtimeo_ms;

    /* Optional receive task. */
    TaskHandle_t      task;
    volatile bool     task_run;
    SemaphoreHandle_t task_done;
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/*
 * Resolve a host string into a sockaddr. lwIP's getaddrinfo() tries a literal
 * address before it touches DNS, so a dotted quad or broadcast address costs a
 * tcpip thread hop and nothing more.
 */
static esp_err_t artnet_resolve(const char *host, struct sockaddr_in *out)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int rc;

    if (host == NULL || host[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        ESP_LOGE(TAG, "cannot resolve '%s' (rc=%d)", host, rc);
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return ESP_ERR_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(ARTNET_PORT);
    out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    return ESP_OK;
}

static void artnet_addr_from_ip(uint32_t ipv4, struct sockaddr_in *out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(ARTNET_PORT);
    out->sin_addr.s_addr = ipv4;
}

/*
 * Build an ArtDmx packet in tx_buf and return the number of DMX bytes.
 * Wire format is fixed by the Art-Net specification:
 *   [0..7]   "Art-Net\0"
 *   [8..9]   op-code, little endian
 *   [10..11] protocol version, big endian
 *   [12]     sequence
 *   [13]     physical
 *   [14..15] universe, little endian
 *   [16..17] data length, big endian, always even
 */
static void artnet_init_tx_header(artnet_handle_t h)
{
    /* Bytes 0..11 never change, so they are written once instead of on every
     * packet. artnet_make_packet() only touches the four mutable fields. */
    memcpy(h->tx_buf, artnet_id, sizeof(artnet_id));
    h->tx_buf[8] = (uint8_t)(ARTNET_OP_DMX & 0xff);
    h->tx_buf[9] = (uint8_t)(ARTNET_OP_DMX >> 8);
    h->tx_buf[10] = (uint8_t)(ARTNET_PROTOCOL_VER >> 8);
    h->tx_buf[11] = (uint8_t)(ARTNET_PROTOCOL_VER & 0xff);
}

static uint16_t artnet_make_packet(artnet_handle_t h)
{
    h->tx_buf[12] = h->sequence;
    h->sequence++;
    if (h->sequence == 0) {
        /* 0 means "sequencing disabled", so it is skipped on wrap. */
        h->sequence = 1;
    }
    h->tx_buf[13] = h->physical;
    h->tx_buf[14] = (uint8_t)(h->tx_universe & 0xff);
    h->tx_buf[15] = (uint8_t)(h->tx_universe >> 8);

    /* tx_length is kept even and <= 512 by artnet_set_length(). */
    h->tx_buf[16] = (uint8_t)(h->tx_length >> 8);
    h->tx_buf[17] = (uint8_t)(h->tx_length & 0xff);

    return h->tx_length;
}

static esp_err_t artnet_send(artnet_handle_t h, const struct sockaddr_in *dest)
{
    uint16_t len = artnet_make_packet(h);
    int sent;

    sent = sendto(h->sock, h->tx_buf, ARTNET_DMX_START + len, 0,
                  (const struct sockaddr *)dest, sizeof(*dest));
    if (sent < 0) {
        /*
         * Report a failure once per outage rather than once per packet. During
         * a Wi-Fi drop every sendto fails, and a log line per frame would stall
         * the transmit path on the UART for longer than the frame itself.
         */
        if (!h->tx_failing) {
            h->tx_failing = true;
            ESP_LOGW(TAG, "sendto failed: errno %d (further failures not logged)", errno);
        }
        return ESP_FAIL;
    }

    if (h->tx_failing) {
        h->tx_failing = false;
        ESP_LOGI(TAG, "transmit recovered");
    }

    return ESP_OK;
}

/* Parse one received datagram. Returns the op-code, or 0 when not Art-Net. */
static uint16_t artnet_parse(artnet_handle_t h, int n, uint32_t sender_ip)
{
    uint16_t opcode;

    if (n < ARTNET_MIN_PACKET) {
        return 0;
    }
    if (memcmp(h->rx_buf, artnet_id, sizeof(artnet_id)) != 0) {
        return 0;
    }

    opcode = (uint16_t)(h->rx_buf[8] | (h->rx_buf[9] << 8));

    /* A truncated ArtDmx packet must not update any state, otherwise the
     * accessors would report a DMX packet with stale universe and length. */
    if (opcode == ARTNET_OP_DMX && n < ARTNET_DMX_START) {
        return 0;
    }

    h->opcode = opcode;
    h->rx_packet_size = (uint16_t)n;
    h->sender_ip = sender_ip;

    if (opcode != ARTNET_OP_DMX) {
        /* rx_buf now holds this packet, not DMX data. Make the DMX accessors
         * say so instead of describing a frame that is no longer there. */
        h->rx_length = 0;
        return opcode;
    }

    {
        uint16_t length;

        h->rx_sequence = h->rx_buf[12];
        h->rx_universe = (uint16_t)(h->rx_buf[14] | (h->rx_buf[15] << 8));
        length = (uint16_t)((h->rx_buf[16] << 8) | h->rx_buf[17]);

        /* Never trust the advertised length, clamp it to what really arrived. */
        if (length > (uint16_t)(n - ARTNET_DMX_START)) {
            length = (uint16_t)(n - ARTNET_DMX_START);
        }
        if (length > ARTNET_MAX_DMX) {
            length = ARTNET_MAX_DMX;
        }
        h->rx_length = length;

        if (h->dmx_cb != NULL) {
            artnet_dmx_t frame = {
                .universe = h->rx_universe,
                .length = length,
                .sequence = h->rx_sequence,
                .data = h->rx_buf + ARTNET_DMX_START,
                .sender_ip = sender_ip,
            };
            h->dmx_cb(&frame, h->user_ctx);
        }
    }

    return opcode;
}

/* True when the caller is the receive task, which must not wait on itself. */
static bool artnet_in_rx_task(artnet_handle_t h)
{
    return h->task != NULL && xTaskGetCurrentTaskHandle() == h->task;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

esp_err_t artnet_init(const artnet_config_t *config, artnet_handle_t *out_handle)
{
    artnet_config_t defaults = ARTNET_CONFIG_DEFAULT();
    struct sockaddr_in bind_addr;
    artnet_handle_t h;
    uint16_t port;
    int opt = 1;

    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config == NULL) {
        config = &defaults;
    }

    h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }
    h->sock = -1;
    h->sequence = 1;
    h->tx_length = ARTNET_MAX_DMX;
    h->dmx_cb = config->dmx_cb;
    h->user_ctx = config->user_ctx;
    artnet_init_tx_header(h);

    h->task_done = xSemaphoreCreateBinary();
    if (h->task_done == NULL) {
        free(h);
        return ESP_ERR_NO_MEM;
    }

    h->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h->sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        vSemaphoreDelete(h->task_done);
        free(h);
        return ESP_FAIL;
    }

    setsockopt(h->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (config->enable_broadcast) {
        setsockopt(h->sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    }

    /* A transmit-only node takes an ephemeral port so the LAN's Art-Net
     * broadcasts are not queued for a reader that will never come. */
    if (config->tx_only) {
        port = 0;
    } else {
        port = config->port != 0 ? config->port : ARTNET_PORT;
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(port);

    if (bind(h->sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind to port %u failed: errno %d", port, errno);
        close(h->sock);
        vSemaphoreDelete(h->task_done);
        free(h);
        return ESP_FAIL;
    }

    if (config->host != NULL && artnet_set_host(h, config->host) != ESP_OK) {
        /* Not fatal, the application can retry with artnet_set_host(). */
        ESP_LOGW(TAG, "transmit target '%s' unresolved for now", config->host);
    }

    if (config->tx_only) {
        ESP_LOGI(TAG, "transmit only, not listening on port %u", ARTNET_PORT);
    } else {
        ESP_LOGI(TAG, "listening on UDP port %u", port);
#ifdef CONFIG_LWIP_UDP_RECVMBOX_SIZE
        if (CONFIG_LWIP_UDP_RECVMBOX_SIZE <= 6) {
            /* The failure this prevents is silent, so say it once at boot. */
            ESP_LOGI(TAG, "CONFIG_LWIP_UDP_RECVMBOX_SIZE is %d: frames with more "
                          "universes than that will lose packets, raise it in sdkconfig",
                     CONFIG_LWIP_UDP_RECVMBOX_SIZE);
        }
#endif
    }

    *out_handle = h;

    return ESP_OK;
}

esp_err_t artnet_deinit(artnet_handle_t h)
{
    esp_err_t err;

    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = artnet_stop_task(h);
    if (err != ESP_OK) {
        /* Called from the receive task. Freeing the handle under our own feet
         * is not an option, so leave everything in place and say so. */
        ESP_LOGE(TAG, "artnet_deinit called from the receive task, ignored");
        return err;
    }

    if (h->sock >= 0) {
        close(h->sock);
        h->sock = -1;
    }
    vSemaphoreDelete(h->task_done);
    free(h);

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* receive                                                            */
/* ------------------------------------------------------------------ */

/*
 * Apply a receive timeout, skipping the syscall when it is already in effect.
 * A steady poll interval, which is what the receive task does, therefore costs
 * one setsockopt for the lifetime of the handle rather than one per packet.
 */
static void artnet_apply_timeout(artnet_handle_t h, uint32_t timeout_ms)
{
    struct timeval tv;
    uint32_t ms = timeout_ms;

    if (h->rcvtimeo_ms == timeout_ms) {
        return;
    }

    if (ms == UINT32_MAX) {
        /* lwIP reads an all-zero timeval as "block forever". */
        ms = 0;
    } else {
        /* lwIP rejects anything above INT_MAX milliseconds. Clamp to a tick
         * multiple so the round-up below cannot push it back over. */
        const uint32_t max_ms = ((uint32_t)INT32_MAX / portTICK_PERIOD_MS) * portTICK_PERIOD_MS;

        if (ms > max_ms) {
            ms = max_ms;
        }
        /* lwIP truncates to whole ticks. Round up so a short wait is a wait,
         * not a hot poll. */
        ms = ((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS) * portTICK_PERIOD_MS;
    }

    tv.tv_sec = (time_t)(ms / 1000U);
    tv.tv_usec = (suseconds_t)((ms % 1000U) * 1000U);

    if (setsockopt(h->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0) {
        h->rcvtimeo_ms = timeout_ms;
    } else {
        ESP_LOGW(TAG, "SO_RCVTIMEO %" PRIu32 " ms rejected: errno %d", timeout_ms, errno);
    }
}

esp_err_t artnet_read(artnet_handle_t h, uint32_t timeout_ms, uint16_t *out_opcode)
{
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    uint16_t opcode;
    int flags = 0;
    int n;

    if (h == NULL || h->sock < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->task != NULL && !artnet_in_rx_task(h)) {
        /* The receive task owns rx_buf, a second reader would corrupt it. */
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * One blocking recvfrom rather than select() followed by recvfrom. That
     * halves the number of lwIP entries per received packet, and select() is
     * the more expensive of the two: it takes the core lock and registers a
     * select callback on every call.
     */
    if (timeout_ms == 0) {
        flags = MSG_DONTWAIT;
    } else {
        artnet_apply_timeout(h, timeout_ms);
    }

    n = recvfrom(h->sock, h->rx_buf, sizeof(h->rx_buf), flags,
                 (struct sockaddr *)&from, &from_len);
    if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
        return ESP_FAIL;
    }

    opcode = artnet_parse(h, n, (uint32_t)from.sin_addr.s_addr);
    if (out_opcode != NULL) {
        *out_opcode = opcode;
    }

    return opcode != 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static void artnet_task(void *arg)
{
    artnet_handle_t h = (artnet_handle_t)arg;

    while (h->task_run) {
        /* Short timeout so a stop request is picked up quickly. */
        esp_err_t err = artnet_read(h, ARTNET_TASK_POLL_MS, NULL);

        if (err == ESP_FAIL || err == ESP_ERR_INVALID_STATE) {
            /* A socket error persists; back off instead of spinning on it.
             * Timeouts and foreign packets are normal and cost nothing. */
            vTaskDelay(pdMS_TO_TICKS(ARTNET_TASK_POLL_MS));
        }
    }

    xSemaphoreGive(h->task_done);
    vTaskDelete(NULL);
}

esp_err_t artnet_start_task(artnet_handle_t h, const artnet_task_config_t *cfg)
{
    artnet_task_config_t defaults = ARTNET_TASK_CONFIG_DEFAULT();
    BaseType_t ok;

    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg == NULL) {
        cfg = &defaults;
    }
    if (cfg->core_id != tskNO_AFFINITY &&
        (cfg->core_id < 0 || cfg->core_id >= (BaseType_t)ARTNET_NUM_CORES)) {
        /* FreeRTOS asserts on this inside xTaskCreatePinnedToCore, which
         * would reboot the device. Fail the call instead. */
        ESP_LOGE(TAG, "core_id %d does not exist on this target (%d core(s))",
                 (int)cfg->core_id, (int)ARTNET_NUM_CORES);
        return ESP_ERR_INVALID_ARG;
    }

    h->task_run = true;
    ok = xTaskCreatePinnedToCore(artnet_task,
                                 cfg->name != NULL ? cfg->name : defaults.name,
                                 cfg->stack_size != 0 ? cfg->stack_size : defaults.stack_size,
                                 h,
                                 cfg->priority != 0 ? cfg->priority : defaults.priority,
                                 &h->task,
                                 cfg->core_id);
    if (ok != pdPASS) {
        h->task_run = false;
        h->task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t artnet_stop_task(artnet_handle_t h)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->task == NULL) {
        return ESP_OK;
    }
    if (artnet_in_rx_task(h)) {
        /* The only give comes from the task we are in, after this call
         * returns. Waiting here would never end. */
        return ESP_ERR_INVALID_STATE;
    }

    h->task_run = false;
    xSemaphoreTake(h->task_done, portMAX_DELAY);
    h->task = NULL;

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* receive accessors                                                  */
/* ------------------------------------------------------------------ */

uint16_t artnet_get_opcode(artnet_handle_t h) { return h != NULL ? h->opcode : 0; }
uint16_t artnet_get_universe(artnet_handle_t h) { return h != NULL ? h->rx_universe : 0; }
uint16_t artnet_get_rx_length(artnet_handle_t h) { return h != NULL ? h->rx_length : 0; }
uint8_t  artnet_get_sequence(artnet_handle_t h) { return h != NULL ? h->rx_sequence : 0; }
uint32_t artnet_get_sender_ip(artnet_handle_t h) { return h != NULL ? h->sender_ip : 0; }

const uint8_t *artnet_get_dmx(artnet_handle_t h)
{
    return h != NULL ? h->rx_buf + ARTNET_DMX_START : NULL;
}

void artnet_log_packet(artnet_handle_t h, bool with_data)
{
    if (h == NULL) {
        return;
    }

    ESP_LOGI(TAG, "size=%u opcode=0x%04x universe=%u length=%u sequence=%u",
             h->rx_packet_size, h->opcode, h->rx_universe, h->rx_length, h->rx_sequence);

    if (with_data && h->rx_length > 0) {
        ESP_LOG_BUFFER_HEX(TAG, h->rx_buf + ARTNET_DMX_START, h->rx_length);
    }
}

/* ------------------------------------------------------------------ */
/* transmit                                                           */
/* ------------------------------------------------------------------ */

esp_err_t artnet_set_host(artnet_handle_t h, const char *host)
{
    struct sockaddr_in dest;
    esp_err_t err;

    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = artnet_resolve(host, &dest);
    if (err != ESP_OK) {
        return err;
    }

    h->dest = dest;
    h->dest_valid = true;

    return ESP_OK;
}

void artnet_set_universe(artnet_handle_t h, uint16_t universe)
{
    if (h != NULL) {
        h->tx_universe = universe;
    }
}

void artnet_set_physical(artnet_handle_t h, uint8_t physical)
{
    if (h != NULL) {
        h->physical = physical;
    }
}

void artnet_set_length(artnet_handle_t h, uint16_t length)
{
    if (h == NULL) {
        return;
    }
    if (length > ARTNET_MAX_DMX) {
        length = ARTNET_MAX_DMX;
    }
    if (length % 2 != 0) {
        /* The spec wants an even count. The caller declared the extra channel
         * as not theirs, so it goes out as zero rather than as whatever an
         * earlier frame left in the buffer. */
        h->tx_buf[ARTNET_DMX_START + length] = 0;
        length++;
    }
    h->tx_length = length;
}

uint16_t artnet_get_length(artnet_handle_t h)
{
    return h != NULL ? h->tx_length : 0;
}

esp_err_t artnet_set_byte(artnet_handle_t h, uint16_t pos, uint8_t value)
{
    if (h == NULL || pos >= ARTNET_MAX_DMX) {
        return ESP_ERR_INVALID_ARG;
    }
    h->tx_buf[ARTNET_DMX_START + pos] = value;

    return ESP_OK;
}

esp_err_t artnet_set_buffer(artnet_handle_t h, uint16_t offset, const uint8_t *data, uint16_t len)
{
    if (h == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (offset > ARTNET_MAX_DMX || len > (uint16_t)(ARTNET_MAX_DMX - offset)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(h->tx_buf + ARTNET_DMX_START + offset, data, len);

    return ESP_OK;
}

uint8_t *artnet_get_tx_dmx(artnet_handle_t h)
{
    return h != NULL ? h->tx_buf + ARTNET_DMX_START : NULL;
}

esp_err_t artnet_write(artnet_handle_t h)
{
    if (h == NULL || h->sock < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!h->dest_valid) {
        /* The return code says it; a log line per frame would only cost. */
        ESP_LOGD(TAG, "no transmit target, call artnet_set_host() first");
        return ESP_ERR_INVALID_STATE;
    }

    return artnet_send(h, &h->dest);
}

esp_err_t artnet_write_ip(artnet_handle_t h, uint32_t ipv4)
{
    struct sockaddr_in dest;

    if (h == NULL || h->sock < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    artnet_addr_from_ip(ipv4, &dest);

    return artnet_send(h, &dest);
}

esp_err_t artnet_write_to(artnet_handle_t h, const char *host)
{
    struct sockaddr_in dest;
    esp_err_t err;

    if (h == NULL || h->sock < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = artnet_resolve(host, &dest);
    if (err != ESP_OK) {
        return err;
    }

    return artnet_send(h, &dest);
}
