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
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/semphr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "artnet";

/* "Art-Net" plus the terminating zero, all 8 bytes are on the wire. */
static const char artnet_id[8] = ARTNET_ID;

/* Smallest packet that still carries ID, op-code and protocol version. */
#define ARTNET_MIN_PACKET 12

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
    uint16_t           tx_length;
    uint16_t           tx_universe;
    uint8_t            physical;
    uint8_t            sequence;

    /* Optional receive task. */
    TaskHandle_t      task;
    volatile bool     task_run;
    SemaphoreHandle_t task_done;
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t artnet_resolve(const char *host, struct sockaddr_in *out)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int rc;

    if (host == NULL || host[0] == '\0' || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(ARTNET_PORT);

    /* Fast path for the common dotted quad, avoids a DNS round trip. */
    if (inet_pton(AF_INET, host, &out->sin_addr) == 1) {
        return ESP_OK;
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

    out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);

    return ESP_OK;
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
static uint16_t artnet_make_packet(artnet_handle_t h)
{
    uint16_t len;

    memcpy(h->tx_buf, artnet_id, sizeof(artnet_id));
    h->tx_buf[8] = (uint8_t)(ARTNET_OP_DMX & 0xff);
    h->tx_buf[9] = (uint8_t)(ARTNET_OP_DMX >> 8);
    h->tx_buf[10] = (uint8_t)(ARTNET_PROTOCOL_VER >> 8);
    h->tx_buf[11] = (uint8_t)(ARTNET_PROTOCOL_VER & 0xff);
    h->tx_buf[12] = h->sequence;
    h->sequence++;
    if (h->sequence == 0) {
        /* 0 means "sequencing disabled", so it is skipped on wrap. */
        h->sequence = 1;
    }
    h->tx_buf[13] = h->physical;
    h->tx_buf[14] = (uint8_t)(h->tx_universe & 0xff);
    h->tx_buf[15] = (uint8_t)(h->tx_universe >> 8);

    len = h->tx_length + (h->tx_length % 2); /* the spec wants an even length */
    if (len > ARTNET_MAX_DMX) {
        len = ARTNET_MAX_DMX;
    }
    h->tx_buf[16] = (uint8_t)(len >> 8);
    h->tx_buf[17] = (uint8_t)(len & 0xff);

    return len;
}

static esp_err_t artnet_send(artnet_handle_t h, const struct sockaddr_in *dest)
{
    uint16_t len = artnet_make_packet(h);
    int sent;

    sent = sendto(h->sock, h->tx_buf, ARTNET_DMX_START + len, 0,
                  (const struct sockaddr *)dest, sizeof(*dest));
    if (sent < 0) {
        ESP_LOGE(TAG, "sendto failed: errno %d", errno);
        return ESP_FAIL;
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

    if (opcode == ARTNET_OP_DMX) {
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

        if (h->dmx_cb != NULL && length > 0) {
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

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

esp_err_t artnet_init(const artnet_config_t *config, artnet_handle_t *out_handle)
{
    artnet_config_t defaults = ARTNET_CONFIG_DEFAULT();
    struct sockaddr_in bind_addr;
    artnet_handle_t h;
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

    h->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h->sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        free(h);
        return ESP_FAIL;
    }

    setsockopt(h->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (config->enable_broadcast) {
        setsockopt(h->sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(config->port != 0 ? config->port : ARTNET_PORT);

    if (bind(h->sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind to port %u failed: errno %d", ntohs(bind_addr.sin_port), errno);
        close(h->sock);
        free(h);
        return ESP_FAIL;
    }

    if (config->host != NULL) {
        if (artnet_resolve(config->host, &h->dest) == ESP_OK) {
            h->dest_valid = true;
        } else {
            /* Not fatal, the application can retry with artnet_set_host(). */
            ESP_LOGW(TAG, "transmit target '%s' unresolved for now", config->host);
        }
    }

    ESP_LOGI(TAG, "listening on UDP port %u", ntohs(bind_addr.sin_port));
    *out_handle = h;

    return ESP_OK;
}

void artnet_deinit(artnet_handle_t h)
{
    if (h == NULL) {
        return;
    }

    artnet_stop_task(h);

    if (h->sock >= 0) {
        close(h->sock);
        h->sock = -1;
    }
    free(h);
}

/* ------------------------------------------------------------------ */
/* receive                                                            */
/* ------------------------------------------------------------------ */

esp_err_t artnet_read(artnet_handle_t h, uint32_t timeout_ms, uint16_t *out_opcode)
{
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    struct timeval tv;
    fd_set read_set;
    uint16_t opcode;
    int rc;
    int n;

    if (h == NULL || h->sock < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (h->task != NULL && xTaskGetCurrentTaskHandle() != h->task) {
        /* The receive task owns rx_buf, a second reader would corrupt it. */
        return ESP_ERR_INVALID_STATE;
    }

    FD_ZERO(&read_set);
    FD_SET(h->sock, &read_set);
    tv.tv_sec = (time_t)(timeout_ms / 1000U);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U);

    rc = select(h->sock + 1, &read_set, NULL, NULL,
                timeout_ms == UINT32_MAX ? NULL : &tv);
    if (rc == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (rc < 0) {
        if (errno == EINTR) {
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGE(TAG, "select failed: errno %d", errno);
        return ESP_FAIL;
    }

    n = recvfrom(h->sock, h->rx_buf, sizeof(h->rx_buf), 0,
                 (struct sockaddr *)&from, &from_len);
    if (n < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
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
        artnet_read(h, 100, NULL);
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

    if (h->task_done == NULL) {
        h->task_done = xSemaphoreCreateBinary();
        if (h->task_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    h->task_run = true;
    ok = xTaskCreatePinnedToCore(artnet_task,
                                 cfg->name != NULL ? cfg->name : "artnet_rx",
                                 cfg->stack_size != 0 ? cfg->stack_size : 4096,
                                 h,
                                 cfg->priority != 0 ? cfg->priority : 5,
                                 &h->task,
                                 cfg->core_id);
    if (ok != pdPASS) {
        h->task_run = false;
        h->task = NULL;
        vSemaphoreDelete(h->task_done);
        h->task_done = NULL;
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

    h->task_run = false;
    xSemaphoreTake(h->task_done, portMAX_DELAY);
    h->task = NULL;

    vSemaphoreDelete(h->task_done);
    h->task_done = NULL;

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
    if (h != NULL) {
        h->tx_length = length > ARTNET_MAX_DMX ? ARTNET_MAX_DMX : length;
    }
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
    h->tx_length = (uint16_t)(offset + len);

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
        ESP_LOGE(TAG, "no transmit target, call artnet_set_host() first");
        return ESP_ERR_INVALID_STATE;
    }

    return artnet_send(h, &h->dest);
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
