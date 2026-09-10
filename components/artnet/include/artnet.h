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

/*
 * Art-Net for ESP-IDF.
 *
 * Pure ESP-IDF / lwIP implementation. No Arduino core, no WiFi.h, no String.
 * Wi-Fi (or Ethernet) is brought up by the application; this component only
 * owns a UDP socket.
 */

#ifndef ARTNET_H
#define ARTNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UDP port used by Art-Net. */
#define ARTNET_PORT 6454

/* Op-codes (little endian on the wire). */
#define ARTNET_OP_POLL 0x2000
#define ARTNET_OP_DMX  0x5000
#define ARTNET_OP_SYNC 0x5200

/* Packet layout. */
#define ARTNET_ID           "Art-Net"
#define ARTNET_DMX_START    18
#define ARTNET_MAX_DMX      512
#define ARTNET_MAX_PACKET   (ARTNET_DMX_START + ARTNET_MAX_DMX)
#define ARTNET_PROTOCOL_VER 14

/* Opaque instance handle. */
typedef struct artnet_ctx *artnet_handle_t;

/* One received ArtDmx frame. Valid only for the duration of the callback. */
typedef struct {
    uint16_t       universe;  /* 15 bit port address */
    uint16_t       length;    /* number of valid bytes in data, 1..512 */
    uint8_t        sequence;  /* 0 = sequencing disabled by the sender */
    const uint8_t *data;      /* DMX channel data */
    uint32_t       sender_ip; /* IPv4 address of the sender, network order */
} artnet_dmx_t;

/*
 * Called for every valid ArtDmx packet. Runs in the context of the caller of
 * artnet_read(), or in the receive task when artnet_start_task() is used.
 * Keep it short and do not block.
 */
typedef void (*artnet_dmx_cb_t)(const artnet_dmx_t *frame, void *user_ctx);

typedef struct {
    uint16_t        port;             /* listen port, 0 selects ARTNET_PORT */
    const char      *host;            /* default transmit target, NULL for receive only */
    bool            enable_broadcast; /* allow sending to a broadcast address */
    artnet_dmx_cb_t dmx_cb;           /* optional ArtDmx callback */
    void            *user_ctx;        /* passed back to dmx_cb */
} artnet_config_t;

#define ARTNET_CONFIG_DEFAULT()      \
    {                                \
        .port = ARTNET_PORT,         \
        .host = NULL,                \
        .enable_broadcast = true,    \
        .dmx_cb = NULL,              \
        .user_ctx = NULL,            \
    }

/* Task options for artnet_start_task(). */
typedef struct {
    uint32_t     stack_size; /* 0 selects 4096 */
    UBaseType_t  priority;   /* 0 selects 5 */
    BaseType_t   core_id;    /* tskNO_AFFINITY to let the scheduler decide */
    const char   *name;      /* NULL selects "artnet_rx" */
} artnet_task_config_t;

#define ARTNET_TASK_CONFIG_DEFAULT() \
    {                                \
        .stack_size = 4096,          \
        .priority = 5,               \
        .core_id = tskNO_AFFINITY,   \
        .name = "artnet_rx",         \
    }

/*
 * Create an instance and open the UDP socket.
 * Returns ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM or ESP_FAIL on failure.
 */
esp_err_t artnet_init(const artnet_config_t *config, artnet_handle_t *out_handle);

/* Stop the receive task if running, close the socket and free the instance. */
void artnet_deinit(artnet_handle_t handle);

/*
 * Receive and dispatch at most one packet.
 * A handle is not thread-safe. Once artnet_start_task() is running, that task
 * owns the receive path: do not call artnet_read() or the artnet_get_*()
 * accessors from another task, consume the data inside the callback instead.
 * Calling artnet_read() from another task returns ESP_ERR_INVALID_STATE.
 * timeout_ms: 0 polls without blocking, UINT32_MAX blocks forever.
 * On success *out_opcode holds the op-code of the packet (may be NULL).
 * Returns ESP_OK, ESP_ERR_TIMEOUT when nothing arrived, ESP_ERR_INVALID_RESPONSE
 * for a packet that is not Art-Net, or ESP_FAIL on a socket error.
 */
esp_err_t artnet_read(artnet_handle_t handle, uint32_t timeout_ms, uint16_t *out_opcode);

/* Run artnet_read() in a dedicated FreeRTOS task. cfg may be NULL for defaults. */
esp_err_t artnet_start_task(artnet_handle_t handle, const artnet_task_config_t *cfg);

/* Stop the receive task. Safe to call when no task is running. */
esp_err_t artnet_stop_task(artnet_handle_t handle);

/* Accessors for the most recently received packet. */
uint16_t       artnet_get_opcode(artnet_handle_t handle);
uint16_t       artnet_get_universe(artnet_handle_t handle);
uint16_t       artnet_get_rx_length(artnet_handle_t handle);
uint8_t        artnet_get_sequence(artnet_handle_t handle);
const uint8_t *artnet_get_dmx(artnet_handle_t handle);
uint32_t       artnet_get_sender_ip(artnet_handle_t handle);

/* Log the last received packet at ESP_LOG_INFO. */
void artnet_log_packet(artnet_handle_t handle, bool with_data);

/*
 * Transmit side. The transmit buffer is independent of the receive buffer, so
 * a node can send and receive at the same time.
 */

/* Resolve and cache a new default target. Accepts a dotted quad or a hostname. */
esp_err_t artnet_set_host(artnet_handle_t handle, const char *host);

void     artnet_set_universe(artnet_handle_t handle, uint16_t universe);
void     artnet_set_physical(artnet_handle_t handle, uint8_t physical);

/* Number of DMX bytes sent by artnet_write(), clamped to ARTNET_MAX_DMX. */
void     artnet_set_length(artnet_handle_t handle, uint16_t length);
uint16_t artnet_get_length(artnet_handle_t handle);

/* Write one channel. pos is 0 based, values >= ARTNET_MAX_DMX are rejected. */
esp_err_t artnet_set_byte(artnet_handle_t handle, uint16_t pos, uint8_t value);

/* Copy len bytes into the transmit buffer at offset and set the length. */
esp_err_t artnet_set_buffer(artnet_handle_t handle, uint16_t offset, const uint8_t *data, uint16_t len);

/* Direct access to the 512 byte transmit DMX buffer. */
uint8_t *artnet_get_tx_dmx(artnet_handle_t handle);

/* Send one ArtDmx packet to the configured host. */
esp_err_t artnet_write(artnet_handle_t handle);

/* Send one ArtDmx packet to an explicit target without changing the default. */
esp_err_t artnet_write_to(artnet_handle_t handle, const char *host);

#ifdef __cplusplus
}
#endif

#endif /* ARTNET_H */
