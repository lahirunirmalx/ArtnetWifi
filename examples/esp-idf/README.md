# ESP-IDF examples

Projects for the `artnet` component in
[`components/artnet`](../../components/artnet), buildable with `idf.py`
straight from this repository.

| Example | Description | Upstream sketch |
|---------|-------------|-----------------|
| [`artnet_receive`](artnet_receive) | Joins Wi-Fi, listens on UDP 6454 and logs every ArtDmx frame | `ArtnetWifiDebug` |
| [`artnet_transmit`](artnet_transmit) | Sends one universe, ramping an RGB lamp up to white | `ArtnetWifiTransmit` |

`common/wifi_connect` is a small shared component that brings up a Wi-Fi
station and blocks until an IP address is assigned. It exists only so the
examples stay readable; it is not part of the `artnet` component.

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
I (5432) example: DMX: Univ: 0, Seq: 12, Data (48): 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ...
```
