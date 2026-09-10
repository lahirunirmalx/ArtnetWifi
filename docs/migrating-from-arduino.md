# Migrating from the Arduino ArtnetWifi library

Up to release 1.6.3 this repository was an Arduino library. From 2.0.0 it is an
ESP-IDF component with a different API. If your Arduino or PlatformIO build
pulled this repository through `lib_deps` or a submodule, pin it to tag
[`1.6.3`](https://github.com/lahirunirmalx/ArtnetWifi/tree/1.6.3) or switch to
upstream [rstephan/ArtnetWifi](https://github.com/rstephan/ArtnetWifi).

If you are porting a sketch to ESP-IDF, this page maps the old calls to the new
ones and lists the places where the behaviour deliberately differs.

## API mapping

| Arduino `ArtnetWifi` | ESP-IDF `artnet` |
|----------------------|------------------|
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

## Behaviour differences

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

