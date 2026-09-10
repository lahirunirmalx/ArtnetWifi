# ESP-IDF examples

Projects for the `artnet` component in
[`components/artnet`](../../components/artnet), buildable with `idf.py`
straight from this repository.

| Example | Description | Upstream sketch |
|---------|-------------|-----------------|
| [`artnet_receive`](artnet_receive) | Joins Wi-Fi, listens on UDP 6454, counts every ArtDmx frame and logs a sample of them (rate limited so the UART never stalls the receive path) | `ArtnetWifiDebug` |
| [`artnet_transmit`](artnet_transmit) | Sends one universe, ramping an RGB lamp up to white; a `tx_only` node | `ArtnetWifiTransmit` |
| [`artnet_multi_universe`](artnet_multi_universe) | Assembles a 240 LED strip from two universes in the receive task and hands complete frames to a render task through a length-1 queue. The LED driver is a stub with a comment where `led_strip` plugs in | `ArtnetWifiFastLED`, `ArtnetWifiNeoPixel` |

`common/wifi_connect` is a small shared component that brings up a Wi-Fi
station and blocks until an IP address is assigned, retrying for as long as it
takes. It exists only so the examples stay readable and need no menuconfig; it
is not part of the `artnet` component. ESP-IDF's own
`protocol_examples_common` does the same job with more options.

## Building

```
. $IDF_PATH/export.sh
cd examples/esp-idf/artnet_receive
idf.py set-target esp32          # or esp32s3, esp32c3, ...
idf.py build flash monitor
```

Edit `WIFI_SSID` and `WIFI_PASS` at the top of `main/main.c` first. The
transmit example also needs `ARTNET_HOST` set to your controller or to the
broadcast address of your subnet, for example `2.255.255.255`.

Each example's `CMakeLists.txt` points `EXTRA_COMPONENT_DIRS` at this
repository, so nothing needs to be installed.

Expected output from `artnet_receive`:

```
I (5432) example: DMX: Univ: 0, Seq: 12, Len: 48, frame 41 (7 not shown)
I (5432) example: 11 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```
