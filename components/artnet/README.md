# artnet - Art-Net for ESP-IDF

An ESP-IDF component that receives and transmits Art-Net (DMX over UDP) frames
and answers discovery polls. Plain C over lwIP BSD sockets, `esp_err_t` returns,
one handle per node, no dependencies beyond ESP-IDF.

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
cfg.node.ip = got_ip_event->ip_info.ip.addr;     /* own address, network order */
cfg.node.short_name = "Bar strip";              /* up to 17 characters */
cfg.node.long_name = "Left bar, 240 pixels";    /* up to 63 */
cfg.node.first_universe = 0;                    /* port address of output 1 */
cfg.node.num_ports = 2;                         /* outputs 1..4, consecutive universes */
esp_wifi_get_mac(WIFI_IF_STA, cfg.node.mac);    /* optional, informational */
```

`cfg.node.ip` is required for discovery to be useful: the controller lists the
node under that address. lwIP cannot tell a socket bound to `INADDR_ANY` which
address it answers from, so the component does not guess; take the value from
your `IP_EVENT_STA_GOT_IP` (or `_ETH_`) handler, where `ip_info.ip.addr` is
already in the right form, and call `artnet_set_node_ip()` if DHCP later hands
out a different one. A reply sent with no address goes out carrying `0.0.0.0`
and logs a warning once.

Defaults are `"ArtnetWifi"`, universe 0, one port. Set `cfg.node.answer_poll =
false` to stay silent. One reply describes one sub-net of 16 universes, so
`num_ports` is clamped to stay inside it.

The reply is unicast to the poller. The specification's default is the
directed broadcast address, with unicast only when the poller asks for it in
`TalkToMe`; mainstream controllers accept the unicast, and it needs no netmask
knowledge. If yours does not, `artnet_send_poll_reply(h, broadcast_ip)` after
each `ARTNET_OP_POLL` from `artnet_read()` gives you the broadcast behaviour.

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
| Discovery | `cfg.node` (identity), `artnet_send_poll_reply`, `artnet_set_node_ip` |
| Transmit  | `artnet_set_host`, `artnet_set_universe`, `artnet_set_physical`, `artnet_set_length`, `artnet_set_byte`, `artnet_set_buffer`, `artnet_get_tx_dmx`, `artnet_write`, `artnet_write_ip`, `artnet_write_to` |

## Not implemented

`ArtSync` is reported through the op-code but not acted on: frames are handed
to the callback as they arrive rather than held until the sync packet. RDM,
`ArtAddress` (remote reconfiguration) and `ArtTimeCode` are not handled.
