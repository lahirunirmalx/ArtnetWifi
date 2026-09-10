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
    uint16_t       length;    /* number of valid bytes in data, 0..512 */
    uint8_t        sequence;  /* 0 = sequencing disabled by the sender */
    const uint8_t *data;      /* DMX channel data */
    uint32_t       sender_ip; /* IPv4 address of the sender, network order */
} artnet_dmx_t;

/*
 * Called for every valid ArtDmx packet, including zero-length ones, which some
 * controllers send as keep-alives. Runs in the context of the caller of
 * artnet_read(), or in the receive task when artnet_start_task() is used.
 * Keep it short and do not block. Do not call artnet_stop_task() or
 * artnet_deinit() from inside it, they return ESP_ERR_INVALID_STATE there.
 */
typedef void (*artnet_dmx_cb_t)(const artnet_dmx_t *frame, void *user_ctx);

typedef struct {
    uint16_t        port;             /* listen port, 0 selects ARTNET_PORT */
    const char      *host;            /* default transmit target, NULL for receive only */
    bool            enable_broadcast; /* allow sending to a broadcast address */
    bool            tx_only;          /* do not listen on the Art-Net port, see below */
    artnet_dmx_cb_t dmx_cb;           /* optional ArtDmx callback */
    void            *user_ctx;        /* passed back to dmx_cb */
} artnet_config_t;

/*
 * tx_only: a node that only transmits should set this. The socket is then
 * bound to an ephemeral port instead of 6454, so the Art-Net traffic every
 * controller broadcasts on the LAN is never queued for a reader that does not
 * exist. Without it, lwIP pins up to CONFIG_LWIP_UDP_RECVMBOX_SIZE received
 * datagrams, and the Wi-Fi RX buffers behind them, for the life of the handle.
 * artnet_read() still works on a tx_only handle but only sees unicast replies.
 */

#define ARTNET_CONFIG_DEFAULT()      \
    {                                \
        .port = ARTNET_PORT,         \
        .host = NULL,                \
        .enable_broadcast = true,    \
        .tx_only = false,            \
        .dmx_cb = NULL,              \
        .user_ctx = NULL,            \
    }

/*
 * Task options for artnet_start_task(). Start from ARTNET_TASK_CONFIG_DEFAULT()
 * and override what you need. A zero stack_size or priority and a NULL name
 * fall back to the default value; core_id has no such fallback, 0 means core 0.
 * core_id must be tskNO_AFFINITY or a valid core index for the target.
 */
typedef struct {
    uint32_t     stack_size;
    UBaseType_t  priority;
    BaseType_t   core_id;
    const char   *name;
} artnet_task_config_t;

#define ARTNET_TASK_CONFIG_DEFAULT() \
    {                                \
        .stack_size = 4096,          \
        .priority = 5,               \
        .core_id = tskNO_AFFINITY,   \
        .name = "artnet_rx",         \
    }

/*
 * Create an instance and open the UDP socket. config may be NULL for defaults.
 * Returns ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM or ESP_FAIL on failure.
 */
esp_err_t artnet_init(const artnet_config_t *config, artnet_handle_t *out_handle);

/*
 * Stop the receive task if running, close the socket and free the instance.
 * Returns ESP_ERR_INVALID_STATE when called from the receive task itself
 * (that is, from inside the DMX callback); nothing is freed in that case.
 */
esp_err_t artnet_deinit(artnet_handle_t handle);

/*
 * Receive and dispatch at most one packet.
 *
 * A handle is not thread-safe. Once artnet_start_task() is running, that task
 * owns the receive path: do not call artnet_read() or the artnet_get_*()
 * accessors from another task, consume the data inside the callback instead.
 * Calling artnet_read() from another task returns ESP_ERR_INVALID_STATE.
 *
 * timeout_ms: 0 polls without blocking, UINT32_MAX blocks forever. Other values
 * are rounded up to a whole FreeRTOS tick and clamped to INT32_MAX.
 *
 * On success *out_opcode holds the op-code of the packet (may be NULL).
 * Returns ESP_OK, ESP_ERR_TIMEOUT when nothing arrived, ESP_ERR_INVALID_RESPONSE
 * for a packet that is not Art-Net, or ESP_FAIL on a socket error.
 */
esp_err_t artnet_read(artnet_handle_t handle, uint32_t timeout_ms, uint16_t *out_opcode);

/*
 * Run artnet_read() in a dedicated FreeRTOS task. cfg may be NULL for defaults.
 * Returns ESP_ERR_INVALID_ARG for a core_id the target does not have,
 * ESP_ERR_INVALID_STATE if the task is already running.
 */
esp_err_t artnet_start_task(artnet_handle_t handle, const artnet_task_config_t *cfg);

/*
 * Stop the receive task. Safe to call when no task is running. Takes up to
 * 100 ms, the receive task's poll interval. Returns ESP_ERR_INVALID_STATE when
 * called from the receive task itself.
 */
esp_err_t artnet_stop_task(artnet_handle_t handle);

/*
 * Accessors for the most recently received Art-Net packet.
 *
 * artnet_get_opcode() and artnet_get_sender_ip() describe whatever arrived
 * last. The DMX accessors are only meaningful when artnet_get_opcode() returns
 * ARTNET_OP_DMX: after any other packet (ArtPoll, ArtPollReply, ArtSync, all of
 * which controllers broadcast on the same port) artnet_get_rx_length() returns
 * 0, and artnet_get_dmx() points at that packet's bytes rather than DMX data.
 */
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

/*
 * Resolve and cache a new default target. Accepts a dotted quad, a broadcast
 * address or a hostname. A hostname may block on a DNS lookup, so call this
 * once at setup, not per frame.
 */
esp_err_t artnet_set_host(artnet_handle_t handle, const char *host);

void     artnet_set_universe(artnet_handle_t handle, uint16_t universe);
void     artnet_set_physical(artnet_handle_t handle, uint8_t physical);

/*
 * Number of DMX bytes sent by artnet_write(). Art-Net requires an even count,
 * so an odd length is rounded up and the padding channel is zeroed, which is
 * what artnet_get_length() then reports. Clamped to ARTNET_MAX_DMX. This is
 * the only call that changes the transmit length.
 */
void     artnet_set_length(artnet_handle_t handle, uint16_t length);
uint16_t artnet_get_length(artnet_handle_t handle);

/* Write one channel. pos is 0 based, values >= ARTNET_MAX_DMX are rejected. */
esp_err_t artnet_set_byte(artnet_handle_t handle, uint16_t pos, uint8_t value);

/*
 * Copy len bytes into the transmit buffer at offset. Does not change the
 * transmit length, so a partial update never shrinks the frame.
 */
esp_err_t artnet_set_buffer(artnet_handle_t handle, uint16_t offset, const uint8_t *data, uint16_t len);

/* Direct access to the 512 byte transmit DMX buffer. */
uint8_t *artnet_get_tx_dmx(artnet_handle_t handle);

/* Send one ArtDmx packet to the configured host. */
esp_err_t artnet_write(artnet_handle_t handle);

/*
 * Send one ArtDmx packet to an IPv4 address given in network byte order, the
 * same form artnet_get_sender_ip() and artnet_dmx_t.sender_ip use, so replying
 * to a sender is one call. No resolution, no allocation, fine per frame.
 */
esp_err_t artnet_write_ip(artnet_handle_t handle, uint32_t ipv4);

/*
 * Send one ArtDmx packet to a host name or address without changing the
 * default. Resolves the name on every call; use artnet_set_host() or
 * artnet_write_ip() for anything sent per frame.
 */
esp_err_t artnet_write_to(artnet_handle_t handle, const char *host);

#ifdef __cplusplus
}
#endif

#endif /* ARTNET_H */
