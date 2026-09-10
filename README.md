# ArtnetWifi for ESP-IDF

An Art-Net (DMX over UDP) node for **ESP-IDF**. No Arduino core, no `WiFi.h`,
no `String`, no `IPAddress`. It talks straight to lwIP BSD sockets, returns
`esp_err_t`, and logs through `ESP_LOG*`.

Works on any ESP-IDF target with a network interface: ESP32, ESP32-S2/S3,
ESP32-C3/C6/H2, over Wi-Fi or Ethernet. Requires ESP-IDF 4.4 or newer.

```c
#include "artnet.h"

static void on_dmx(const artnet_dmx_t *frame, void *user_ctx)
{
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
    ESP_ERROR_CHECK(artnet_start_task(artnet, NULL));
}
```

## Installation

Pick whichever suits your project:

**`EXTRA_COMPONENT_DIRS`** - clone this repository anywhere:

```cmake
# <your-project>/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "/path/to/ArtnetWifi/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_app)
```

**Copy** `components/artnet` into your project's `components/` directory.
ESP-IDF picks it up with no further configuration.

**IDF Component Manager** - in `main/idf_component.yml`:

```yaml
dependencies:
  artnet:
    git: https://github.com/lahirunirmalx/ArtnetWifi.git
    path: components/artnet
    version: "*"
```

Then declare the dependency where you use it:

```cmake
idf_component_register(SRCS "main.c" REQUIRES artnet)
```

## Documentation

- **[Component reference](components/artnet/README.md)** - full API, receive and
  transmit walkthroughs, multi-universe tuning.
- **[Examples](examples/esp-idf)** - `artnet_receive`, `artnet_transmit` and
  `artnet_multi_universe` (a 240 LED, two universe strip assembled and handed
  to a render task), buildable with `idf.py` straight from this repository.
- **[Host tests](test/host)** - the protocol code compiles and runs on Linux
  over real loopback UDP, no hardware needed: `cd test/host && make`.

```
. $IDF_PATH/export.sh
cd examples/esp-idf/artnet_receive
idf.py set-target esp32
idf.py build flash monitor
```

## Universes and LED counts

An Art-Net frame carries at most 512 bytes. Divided by 3 colors that is 170.66
LEDs, so a single universe usually drives 170 LEDs and the last 2 bytes go
unused.

**Example:** 240 LEDs, 720 bytes, 2 universes

**Universe "1"**

|Byte |  1|  2|  3|...|508|509|510|511|512|
|:----|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
|Color|  R|  G|  B|...|  R|  G|  B| x | x |
|LED  |  1|  1|  1|...|170|170|170|   |   |

**Universe "2"**

|Byte |  1|  2|  3|...|208|209|210|
|:----|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
|Color|  R|  G|  B|...|  R|  G|  B|
|LED  |171|171|171|...|240|240|240|

*You only have to send 510 bytes of DMX data per frame. Extra bytes at the end
are ignored.*

## Credits

This is a from-scratch ESP-IDF rewrite of the Arduino library
[rstephan/ArtnetWifi](https://github.com/rstephan/ArtnetWifi), which in turn is
based on [natcl/Artnet](https://github.com/natcl/Artnet). The protocol handling
follows their work; the transport, API and threading model are new. See
[the migration table](components/artnet/README.md#coming-from-the-upstream-arduino-artnetwifi-class)
if you are porting a sketch, and
[the behaviour notes](components/artnet/README.md#behaviour-differences-from-the-upstream-arduino-library)
for the bugs fixed along the way.

**Still on Arduino?** This repository stopped being an Arduino library after
tag [`1.6.3`](https://github.com/lahirunirmalx/ArtnetWifi/tree/1.6.3). Pin your
`lib_deps` or submodule to that tag, or use upstream
[rstephan/ArtnetWifi](https://github.com/rstephan/ArtnetWifi) directly.

MIT licensed, see [LICENSE](LICENSE).

# Art-Net

Art-Net(tm) is a trademark of Artistic Licence Holdings Ltd. The Art-Net protocol and associated documentation is copyright Artistic Licence Holdings Ltd.

[Art-Net](http://www.artisticlicence.com/WebSiteMaster/User%20Guides/art-net.pdf)
