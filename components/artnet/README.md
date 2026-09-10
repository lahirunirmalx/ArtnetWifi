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

The manifest requires ESP-IDF 4.4 or newer, so the Component Manager route
needs 4.4+. The code itself also builds on 4.3.2 through `EXTRA_COMPONENT_DIRS`,
which skips the manifest check. Build-verified on IDF 4.3.2, 5.3.1 and 5.5.4,
on ESP32 (Xtensa) and ESP32-C3 (RISC-V).

## Receiving

```c
#include "artnet.h"

static void on_dmx(const artnet_dmx_t *frame, void *user_ctx)
{
    /* frame->data is valid for the duration of this call only. */
    if (frame->length == 0) {
        return; /* keep-alive frame, nothing to show */
    }
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

tcfg.priority = 6;                      /* above the rendering work */
tcfg.core_id = 1;                       /* dual-core targets only: pin to APP_CPU */
ESP_ERROR_CHECK(artnet_start_task(artnet, &tcfg));
```

A `core_id` the target does not have returns `ESP_ERR_INVALID_ARG` rather than
tripping the FreeRTOS assert. Do not call `artnet_stop_task()` or
`artnet_deinit()` from inside the callback: they run on the receive task and
would wait for themselves, so they return `ESP_ERR_INVALID_STATE` there.

Once the task is running it owns the receive path. Consume frames inside the
callback; the `artnet_get_*()` accessors and `artnet_read()` are for the
synchronous style only.

## Transmitting

```c
artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
artnet_handle_t artnet;

cfg.host = "2.255.255.255";             /* broadcast is enabled by default */
cfg.tx_only = true;                     /* not receiving: do not bind port 6454 */
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

`artnet_set_length()` is the only call that changes the frame length, and it
keeps it even as Art-Net requires: `artnet_set_length(h, 5)` sends 6 channels
with the sixth zeroed, and `artnet_get_length()` reports 6. `artnet_set_byte()`
and `artnet_set_buffer()` only write channels.

To answer a controller directly, `artnet_write_ip(h, frame->sender_ip)` sends
to a raw address with no lookup, which is cheap enough for every frame.
`artnet_write_to(h, "name")` resolves on every call and is meant for the
occasional packet.

`tx_only` matters on a busy LAN. Every controller broadcasts ArtDmx and ArtPoll
on port 6454; a node bound to that port which never reads keeps up to
`CONFIG_LWIP_UDP_RECVMBOX_SIZE` of those datagrams, and the Wi-Fi RX buffers
behind them, parked forever. A `tx_only` handle takes an ephemeral port instead.

## Discovery (ArtPollReply)

Controllers find nodes by broadcasting `ArtPoll`; a node that does not answer
has to be entered by IP. This component answers every `ArtPoll` with an
`ArtPollReply` unicast to the poller from port 6454, so it shows up in QLC+,
Resolume, MadMapper and friends under the name and universes you give it:

```c
cfg.node.short_name = "Bar strip";              /* up to 17 characters */
cfg.node.long_name = "Left bar, 240 pixels";    /* up to 63 */
cfg.node.first_universe = 0;                    /* port address of output 1 */
cfg.node.num_ports = 2;                         /* outputs 1..4, consecutive universes */
esp_wifi_get_mac(WIFI_IF_STA, cfg.node.mac);    /* optional, informational */
```

Defaults are `"ArtnetWifi"`, universe 0, one port. Set `cfg.node.answer_poll =
false` to stay silent. The reply carries the node's own IP, found by asking
the stack which address routes to the poller, so it works on Wi-Fi and
Ethernet without an `esp_netif` dependency. One reply describes one sub-net
of 16 universes, so `num_ports` is clamped to stay inside it.

The specification also asks nodes to announce themselves on power-up;
`artnet_send_poll_reply(h, broadcast_ip)` does that once your link is up.

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

`artnet_init()` logs a one-line reminder at boot while the mailbox is still at
its default of 6, because the failure it prevents is otherwise silent.

Also give the receive path room to drain the burst: run it with
`artnet_start_task()` at a priority above your rendering work, and keep the
callback short. Copying the frame into your own buffer and signalling another
task is the usual pattern; the
[`artnet_multi_universe`](https://github.com/lahirunirmalx/ArtnetWifi/blob/master/examples/esp-idf/artnet_multi_universe) example
shows it end to end with a length-1 queue.

## Performance notes

- One `recvfrom()` per received packet. The receive timeout is applied with
  `SO_RCVTIMEO` and cached on the handle, so a steady poll interval costs no
  further syscalls, and `artnet_read(h, 0, ...)` uses `MSG_DONTWAIT` with no
  setsockopt at all.
- The constant part of the ArtDmx header, bytes 0 to 11, is written once at
  `artnet_init()`. `artnet_write()` only updates sequence, physical, universe
  and length.
- The transmit target is resolved once, at `artnet_init()` or
  `artnet_set_host()`, never per packet. `artnet_write_ip()` skips resolution
  entirely.
- Transmit failures are logged once per outage, not once per packet. During a
  Wi-Fi drop every `sendto()` fails, and a log line per frame would hold the
  transmit path on the UART for longer than the frame itself.
- No dynamic allocation after `artnet_init()`. Both buffers live in the handle,
  which is a single ~1.1 kB allocation plus one semaphore.

## API summary

| Area      | Function |
|-----------|----------|
| Lifecycle | `artnet_init`, `artnet_deinit` |
| Receive   | `artnet_read`, `artnet_start_task`, `artnet_stop_task` |
| Rx state  | `artnet_get_opcode`, `artnet_get_universe`, `artnet_get_rx_length`, `artnet_get_sequence`, `artnet_get_dmx`, `artnet_get_sender_ip`, `artnet_log_packet` |
| Discovery | `cfg.node` (identity), `artnet_send_poll_reply` |
| Transmit  | `artnet_set_host`, `artnet_set_universe`, `artnet_set_physical`, `artnet_set_length`, `artnet_set_byte`, `artnet_set_buffer`, `artnet_get_tx_dmx`, `artnet_write`, `artnet_write_ip`, `artnet_write_to` |

## Coming from the upstream Arduino `ArtnetWifi` class

| Arduino (upstream) | ESP-IDF (here) |
|--------------------|----------------|
| `ArtnetWifi artnet; artnet.begin(host)` | `artnet_init(&cfg, &handle)` with `cfg.host = host` |
| `artnet.stop()` | `artnet_deinit(handle)` |
| `artnet.read()` | `artnet_read(handle, timeout_ms, &opcode)` |
| `artnet.setArtDmxCallback(fn)` | `cfg.dmx_cb = fn` (plus `cfg.user_ctx`) |
| `artnet.setArtDmxFunc(lambda)` | `cfg.user_ctx` carries the context instead |
| `artnet.getDmxFrame()` | `artnet_get_dmx(handle)` (receive, only meaningful when `artnet_get_opcode() == ARTNET_OP_DMX`) / `artnet_get_tx_dmx(handle)` (transmit) |
| `artnet.getLength()` | `artnet_get_rx_length(handle)` for a received frame. `artnet_get_length(handle)` is the *transmit* length and is not what `getLength()` returned after `read()`. |
| `artnet.getSequence()` | `artnet_get_sequence(handle)`, receive side only. The outgoing sequence counter is not readable. |
| `artnet.getUniverse()` / `setUniverse(u)` | `artnet_get_universe(handle)` (receive) / `artnet_set_universe(handle, u)` (transmit) |
| `artnet.setByte(pos, val)` | `artnet_set_byte(handle, pos, val)` |
| `artnet.setLength(n)` | `artnet_set_length(handle, n)`, rounded up to even |
| `artnet.write()` / `write(ip)` | `artnet_write(handle)` / `artnet_write_ip(handle, ip)` / `artnet_write_to(handle, host)` |
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
6. After a non-DMX packet (ArtPoll, ArtPollReply, ArtSync) the receive length
   reads as 0. The Arduino getters kept returning the previous DMX frame's
   universe and length while `getDmxFrame()` pointed at the new packet's bytes.
7. An odd transmit length is rounded up at `setLength()` time and the padding
   channel is zeroed. The Arduino version rounded up in `write()` and sent
   whatever stale byte followed the declared data.

Kept identical on purpose: the callback fires for every valid ArtDmx frame,
including zero-length ones that some controllers use as keep-alives.

Added over upstream: `ArtPoll` is answered with an `ArtPollReply`, so the node
is discoverable. The upstream library only reported the op-code.

## Not implemented

`ArtSync` is reported through the op-code but not acted on: frames are handed
to the callback as they arrive rather than held until the sync packet. RDM,
`ArtAddress` (remote reconfiguration) and `ArtTimeCode` are not handled.
