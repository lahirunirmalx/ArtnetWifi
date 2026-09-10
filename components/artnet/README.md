# artnet - Art-Net for ESP-IDF

A pure ESP-IDF component that sends and receives Art-Net (DMX over UDP) frames.
No Arduino core, no `WiFi.h`, no `String`, no `IPAddress`. It talks straight to
lwIP BSD sockets and returns `esp_err_t`.

This is a rewrite of the upstream Arduino library
[rstephan/ArtnetWifi](https://github.com/rstephan/ArtnetWifi). The Art-Net wire
format and the behaviour are the same, the API is not: it is a C handle-based
API that fits ESP-IDF conventions.

## Adding it to a project

**Option 1 - `EXTRA_COMPONENT_DIRS`** (clone the repo anywhere):

```cmake
# <your-project>/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "/path/to/ArtnetWifi/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_app)
```

**Option 2 - copy** `components/artnet` into your project's `components/`
directory. ESP-IDF picks it up with no further configuration.

**Option 3 - IDF Component Manager**, in `main/idf_component.yml`:

```yaml
dependencies:
  artnet:
    git: https://github.com/lahirunirmalx/ArtnetWifi.git
    path: components/artnet
    version: "*"
```

Then declare the dependency in the component that uses it:

```cmake
idf_component_register(SRCS "main.c" REQUIRES artnet)
```

Requires ESP-IDF 4.4 or newer. Verified building against IDF 4.3.2 and 5.3.1 on
ESP32 (Xtensa) and ESP32-C3 (RISC-V).

## Receiving

```c
#include "artnet.h"

static void on_dmx(const artnet_dmx_t *frame, void *user_ctx)
{
    /* frame->data is valid for the duration of this call only. */
    ESP_LOGI("app", "universe %u, %u channels, first byte %u",
             frame->universe, frame->length, frame->data[0]);
}

void app_main(void)
{
    artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
    artnet_handle_t artnet;

    /* Bring up Wi-Fi or Ethernet yourself first. */

    cfg.dmx_cb = on_dmx;
    ESP_ERROR_CHECK(artnet_init(&cfg, &artnet));

    while (true) {
        artnet_read(artnet, 1000, NULL);   /* blocks up to 1000 ms */
    }
}
```

To keep `app_main` free, run the receive loop in its own task instead:

```c
artnet_task_config_t tcfg = ARTNET_TASK_CONFIG_DEFAULT();

tcfg.core_id = 1;                       /* pin the RX task to APP_CPU */
ESP_ERROR_CHECK(artnet_start_task(artnet, &tcfg));
```

## Transmitting

```c
artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
artnet_handle_t artnet;

cfg.host = "2.255.255.255";             /* broadcast is enabled by default */
ESP_ERROR_CHECK(artnet_init(&cfg, &artnet));

artnet_set_universe(artnet, 0);
artnet_set_length(artnet, 3);
artnet_set_byte(artnet, 0, 255);        /* R */
artnet_set_byte(artnet, 1, 128);        /* G */
artnet_set_byte(artnet, 2, 0);          /* B */
artnet_write(artnet);
```

The transmit buffer is separate from the receive buffer, so one handle can send
and receive at the same time. The host name is resolved once, at
`artnet_init()` or `artnet_set_host()`, not on every packet.

## Multi-universe setups

An Art-Net controller sends one UDP packet per universe, back to back, so a
frame for an N universe rig arrives as a burst of N packets within about a
millisecond. lwIP queues them in a per-socket mailbox whose depth is
`CONFIG_LWIP_UDP_RECVMBOX_SIZE`, **6 by default**. Anything past that is
dropped before this component ever sees it, which shows up as missing or
flickering universes.

Raise it in your project's `sdkconfig.defaults`:

```
CONFIG_LWIP_UDP_RECVMBOX_SIZE=32
```

The valid range is 6 to 64. Pick at least your universe count. Note that
`CONFIG_LWIP_SO_RCVBUF` is not the knob for this: it adds a byte ceiling that
causes *more* drops, it does not add queue capacity.

Also give the receive path room to drain the burst: run it with
`artnet_start_task()` at a priority above your rendering work, and keep the
callback short. Copying the frame into your own buffer and signalling another
task is the usual pattern.

## Performance notes

- One `recvfrom()` per received packet. The receive timeout is applied with
  `SO_RCVTIMEO` and cached on the handle, so a steady poll interval costs no
  further syscalls, and `artnet_read(h, 0, ...)` uses `MSG_DONTWAIT` with no
  setsockopt at all.
- The constant part of the ArtDmx header, bytes 0 to 11, is written once at
  `artnet_init()`. `artnet_write()` only updates sequence, physical, universe
  and length.
- The transmit target is resolved once, at `artnet_init()` or
  `artnet_set_host()`, never per packet.
- No dynamic allocation after `artnet_init()`. Both buffers live in the handle,
  which is a single ~1.1 kB allocation.

## API summary

| Area      | Function |
|-----------|----------|
| Lifecycle | `artnet_init`, `artnet_deinit` |
| Receive   | `artnet_read`, `artnet_start_task`, `artnet_stop_task` |
| Rx state  | `artnet_get_opcode`, `artnet_get_universe`, `artnet_get_rx_length`, `artnet_get_sequence`, `artnet_get_dmx`, `artnet_get_sender_ip`, `artnet_log_packet` |
| Transmit  | `artnet_set_host`, `artnet_set_universe`, `artnet_set_physical`, `artnet_set_length`, `artnet_set_byte`, `artnet_set_buffer`, `artnet_get_tx_dmx`, `artnet_write`, `artnet_write_to` |

## Coming from the upstream Arduino `ArtnetWifi` class

| Arduino (upstream) | ESP-IDF (here) |
|--------------------|----------------|
| `ArtnetWifi artnet; artnet.begin(host)` | `artnet_init(&cfg, &handle)` with `cfg.host = host` |
| `artnet.stop()` | `artnet_deinit(handle)` |
| `artnet.read()` | `artnet_read(handle, timeout_ms, &opcode)` |
| `artnet.setArtDmxCallback(fn)` | `cfg.dmx_cb = fn` (plus `cfg.user_ctx`) |
| `artnet.setArtDmxFunc(lambda)` | `cfg.user_ctx` carries the context instead |
| `artnet.getDmxFrame()` | `artnet_get_dmx(handle)` (receive) / `artnet_get_tx_dmx(handle)` (transmit) |
| `artnet.setByte(pos, val)` | `artnet_set_byte(handle, pos, val)` |
| `artnet.write()` / `write(ip)` | `artnet_write(handle)` / `artnet_write_to(handle, host)` |
| `artnet.getSenderIp()` | `artnet_get_sender_ip(handle)` (IPv4, network byte order) |
| `printPacketHeader/Content()` | `artnet_log_packet(handle, with_data)` |

## Behaviour differences from the upstream Arduino library

These are deliberate fixes, not oversights:

1. Short packets are rejected before the header is read. The Arduino version
   parses bytes 12 to 17 of any packet that carries a valid `Art-Net` ID,
   regardless of how short the datagram actually was.
2. The DMX length from the wire is clamped to what really arrived and to 512.
   The Arduino version passes the advertised length to the callback unchecked,
   so a malformed packet makes the callback read past the received data.
3. `artnet_set_byte()` rejects `pos == 512`. The Arduino `setByte()` uses
   `pos > 512`, so channel 512 writes one byte past the DMX area.
4. Transmit and receive use separate buffers. In the Arduino version a received
   packet overwrites the data staged for transmission.
5. The transmit target is resolved once instead of on every `write()`.

## Not implemented

`ArtPoll` is reported through the op-code but no `ArtPollReply` is sent, so the
node is not discoverable by Art-Net controllers. This matches the upstream
Arduino library. `ArtSync` is likewise reported but not acted on.
