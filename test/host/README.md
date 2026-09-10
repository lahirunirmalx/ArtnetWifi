# Host functional tests

The `artnet` component is plain C over BSD sockets, so it builds and runs on a
Linux host against the small shims in [`shims/`](shims) (`esp_err_t`,
`ESP_LOG*`, and the handful of FreeRTOS types the header names). No ESP-IDF,
no hardware.

```
cd test/host
make
```

The tests exchange real UDP datagrams on 127.0.0.1, so the socket path, the
parser and the packet builder are all exercised for real rather than mocked.

| Test | Covers |
|------|--------|
| round trip | transmit to own receiver, payload, even-length rounding, sequence advance |
| wire format | byte-exact ArtDmx header: ID, little-endian opcode and universe, big-endian version and length |
| truncated ArtDmx | short packets rejected without leaving stale universe or length behind |
| length clamp | a packet claiming 512 bytes while carrying 2 reports 2 |
| foreign packet | non Art-Net traffic on port 6454 ignored |
| ArtPoll | reported by op-code, does not reach the DMX callback |
| set_byte bounds | channel 511 accepted, 512 and beyond rejected; `set_buffer` overrun rejected |
| timeouts | non-blocking poll, bounded wait, and the cached `SO_RCVTIMEO` path |
| sequence wrap | 255 wraps to 1, never to 0 |

The suite is mutation-checked: re-introducing any of the upstream bugs listed
in [the component README](../../components/artnet/README.md#behaviour-differences-from-the-upstream-arduino-library)
makes it fail.

Note that FreeRTOS task creation is stubbed out here, so `artnet_start_task()`
is not covered. The synchronous `artnet_read()` path that the task wraps is.
