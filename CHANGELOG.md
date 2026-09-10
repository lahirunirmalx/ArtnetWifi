# Changelog

## 2.0.0 - unreleased

This release turns the repository into an ESP-IDF component. It is a breaking
change for Arduino users.

### Removed

- The Arduino library: `src/ArtnetWifi.*`, `library.properties`,
  `keywords.txt` and the five `.ino` sketches. Arduino and PlatformIO
  `lib_deps` users should pin to tag `1.6.3`, the last Arduino release, or use
  upstream [rstephan/ArtnetWifi](https://github.com/rstephan/ArtnetWifi).
  Porting a sketch: see [docs/migrating-from-arduino.md](docs/migrating-from-arduino.md).

### Added

- `components/artnet`: a pure ESP-IDF C component over lwIP BSD sockets with an
  `esp_err_t` API, an optional FreeRTOS receive task, separate transmit and
  receive buffers, and a `tx_only` mode for nodes that never listen.
- ArtPoll is answered with an ArtPollReply, so controllers discover the node by
  name and universe. Identity comes from `cfg.node`; the node's own address is
  supplied by the application (`cfg.node.ip`, `artnet_set_node_ip()`).
- `artnet_write_ip()` sends to a raw address with no resolution, the cheap way
  to answer a controller per frame.
- Examples: `artnet_receive`, `artnet_transmit` and `artnet_multi_universe`
  (a 240 LED, two universe strip handed to a render task), built with `idf.py`.
- Host test suite under `test/host`: the component runs on Linux over loopback
  UDP, 121 checks, mutation-checked against every bug listed below.

### Fixed, relative to the upstream Arduino library

- Datagrams shorter than the header are rejected instead of parsed.
- The advertised DMX length is clamped to the bytes actually received and to 512.
- Channel index 512 is rejected; upstream wrote one byte past the DMX area.
- A received packet no longer overwrites data staged for transmission.
- After a non-DMX packet the DMX accessors report length 0 instead of
  describing a frame that is no longer in the buffer.
- An odd transmit length is rounded up at `set_length()` time with the padding
  channel zeroed, rather than sending a stale byte.

### Requirements

- ESP-IDF 4.4 or newer per the manifest; also builds on 4.3.2 through
  `EXTRA_COMPONENT_DIRS`. Build-verified on 4.3.2, 5.3.1 and 5.5.4, on ESP32
  and ESP32-C3. Not yet exercised against a controller on hardware.

## 1.6.3 and earlier

Arduino releases, see the upstream project
[rstephan/ArtnetWifi](https://github.com/rstephan/ArtnetWifi/releases).
