# ArtnetWifi for ESP-IDF

An Art-Net node component for ESP-IDF. Drop it into a project and an ESP32
receives DMX universes from any lighting controller over Wi-Fi or Ethernet,
sends them back out, and shows up in the controller's device list by name.

Written in plain C on lwIP sockets and FreeRTOS, with an `esp_err_t` API and
no dependencies beyond ESP-IDF itself.

## What Art-Net is

Art-Net carries DMX512 lighting data over UDP, port 6454. Data is organised in
**universes** of 512 channels, one byte each; a controller (QLC+, Resolume,
MadMapper, grandMA, ...) sends each universe as one packet, up to about 44
times a second. A **node** listens for the universes it cares about and drives
fixtures with them. Controllers find nodes by broadcasting `ArtPoll`, and each
node answers with a description of itself.

This component does the node's share of that conversation. Turning channel
values into light (LED strips, PWM dimmers, DMX line drivers) is left to your
application.

## Features

- **Receive** DMX frames through a callback, a blocking read, or a background
  FreeRTOS task, with per-packet validation of everything read off the wire.
- **Transmit** frames to a fixed host, a broadcast address, or straight back to
  a sender, with the packet header built once and only the changing fields
  touched per frame.
- **Discovery**: answers `ArtPoll` so controllers list the node with the name
  and universes you configure.
- **Multi-universe ready**: independent receive and transmit buffers, a receive
  task that can outrank rendering work, and documentation of the one lwIP
  setting that decides whether bursts of universes survive.
- **Testable without hardware**: the component compiles on Linux and the test
  suite exchanges real UDP datagrams on loopback.

## Quick start

```c
#include "artnet.h"

static void on_dmx(const artnet_dmx_t *frame, void *user_ctx)
{
    /* Runs on the receive path: copy what you need and return quickly. */
    if (frame->length == 0) {
        return; /* keep-alive frame */
    }
    ESP_LOGI("app", "universe %u, %u channels, first byte %u",
             frame->universe, frame->length, frame->data[0]);
}

void app_main(void)
{
    artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
    artnet_handle_t artnet;

    /* Bring up Wi-Fi or Ethernet first, then: */
    cfg.dmx_cb = on_dmx;
    cfg.node.short_name = "My node";
    cfg.node.ip = my_ipv4_from_got_ip_event;   /* what controllers list you under */

    ESP_ERROR_CHECK(artnet_init(&cfg, &artnet));
    ESP_ERROR_CHECK(artnet_start_task(artnet, NULL));
}
```

The full API, with receive, transmit, discovery and tuning walkthroughs, is in
the **[component reference](components/artnet/README.md)**.

## Installation

Any of the three usual ESP-IDF routes works.

**`EXTRA_COMPONENT_DIRS`** - clone this repository anywhere and point at it:

```cmake
# <your-project>/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "/path/to/ArtnetWifi/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_app)
```

**Copy** `components/artnet` into your project's `components/` directory.

**IDF Component Manager** - in `main/idf_component.yml`:

```yaml
dependencies:
  artnet:
    git: https://github.com/lahirunirmalx/ArtnetWifi.git
    path: components/artnet
    version: "^2.0.0"
```

Then declare the dependency where you use it:

```cmake
idf_component_register(SRCS "main.c" REQUIRES artnet)
```

## Examples

Three projects under [`examples/esp-idf`](examples/esp-idf), each buildable
with `idf.py` straight from a clone:

| Example | What it shows |
|---------|---------------|
| `artnet_receive` | Join Wi-Fi, listen, log a sample of incoming frames |
| `artnet_transmit` | Ramp an RGB fixture up to white on one universe |
| `artnet_multi_universe` | Assemble a 240 LED strip from two universes and hand complete frames to a render task |

```
. $IDF_PATH/export.sh
cd examples/esp-idf/artnet_receive
idf.py set-target esp32
idf.py build flash monitor
```

Set `WIFI_SSID` and `WIFI_PASS` at the top of `main/main.c` first.

## How it works

One handle owns one UDP socket bound to port 6454. Receiving is a single
`recvfrom()` into a fixed buffer, a header check, and a callback; transmitting
fills a separate fixed buffer and sends it. Nothing is allocated after
`artnet_init()`.

The callback runs on whichever task called `artnet_read()`, or on the receive
task if you started one. That task also has to drain the next packet, so the
callback should copy the channels it needs into your own buffer and signal a
rendering task, then return. The multi-universe example is the template for
that pattern.

### Universes and LED counts

A universe carries 512 channels. RGB pixels take 3 each, so one universe drives
170 LEDs with 2 channels left over; most controllers therefore send 510
channels per universe for LED strips and start the next strip segment in the
next universe.

**Example:** 240 LEDs, 720 channels, 2 universes

**Universe 1**

|Byte |  1|  2|  3|...|508|509|510|511|512|
|:----|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
|Color|  R|  G|  B|...|  R|  G|  B| x | x |
|LED  |  1|  1|  1|...|170|170|170|   |   |

**Universe 2**

|Byte |  1|  2|  3|...|208|209|210|
|:----|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
|Color|  R|  G|  B|...|  R|  G|  B|
|LED  |171|171|171|...|240|240|240|

## Requirements

- ESP-IDF 4.4 or newer (the manifest's floor; the code also builds on 4.3 via
  `EXTRA_COMPONENT_DIRS`). Build-verified on 4.3.2, 5.3.1 and 5.5.4.
- Any ESP-IDF target with a network interface: ESP32, ESP32-S2/S3,
  ESP32-C3/C6/H2, over Wi-Fi or Ethernet. Verified on ESP32 and ESP32-C3.
- For rigs with more than six universes, raise `CONFIG_LWIP_UDP_RECVMBOX_SIZE`
  in your `sdkconfig.defaults`; see
  [multi-universe setups](components/artnet/README.md#multi-universe-setups).

## Tests

```
cd test/host && make
```

Builds the component against small shims on Linux and runs the suite over
loopback UDP. See [test/host](test/host) for what is covered.

## Credits and license

Based on [rstephan/ArtnetWifi](https://github.com/rstephan/ArtnetWifi) and
[natcl/Artnet](https://github.com/natcl/Artnet). MIT licensed, see
[LICENSE](LICENSE). Release history is in [CHANGELOG.md](CHANGELOG.md).

Art-Net(tm) is a trademark of Artistic Licence Holdings Ltd. The Art-Net
protocol and associated documentation is copyright Artistic Licence Holdings
Ltd. Specification: [art-net.pdf](http://www.artisticlicence.com/WebSiteMaster/User%20Guides/art-net.pdf).
