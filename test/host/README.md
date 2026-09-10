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
| round trip | transmit to own receiver, payload, even-length rounding, zeroed pad, sequence advance |
| wire format | byte-exact ArtDmx header via a `tx_only` node: ID, little-endian opcode and universe, big-endian version and length; `tx_only` really does not bind 6454 |
| odd length padding | `set_length(5)` reports 6 and puts a zero, not a stale channel, on the wire; clamp at 512 |
| truncated ArtDmx | short packets rejected; opcode, universe and length all untouched |
| length clamp | a packet claiming 512 bytes while carrying 2 reports 2 |
| zero-length ArtDmx | an 18-byte keep-alive still reaches the callback with length 0 |
| foreign packet | non Art-Net traffic on port 6454 ignored |
| ArtPoll and ArtPollReply | reported by op-code, kept from the DMX callback; the 239-byte reply is checked field by field (own IP, port, net/sub-net switches, names, node report counter, port count, SwOut, MAC, Status2), plus `answer_poll = false`, the unsolicited send, and the sub-net clamp |
| non-DMX clears DMX state | an ArtPollReply after a DMX frame leaves `rx_length` at 0 instead of describing bytes that are gone |
| buffers independent | a received frame never alters the staged transmit frame or universe |
| tx buffer api | channel 511 accepted, 512 and beyond rejected; `set_buffer` copies without touching the length and rejects overruns |
| timeouts | non-blocking poll, bounded wait, the cached `SO_RCVTIMEO` path, odd millisecond values |
| sequence wrap | 255 wraps to 1 on packet 256, never to 0; a lost round trip fails fast |
| write_ip / write_to | raw-address send, resolver send, refusal without a host |
| task guards | invalid `core_id` rejected before FreeRTOS can assert, failed start leaves the handle usable, `deinit(NULL)` |

The suite is mutation-checked: re-introducing each of the upstream bugs listed
in [the component README](../../components/artnet/README.md#behaviour-differences-from-the-upstream-arduino-library),
including the shared transmit/receive buffer, and each of the fixes above
(stale `rx_length`, unzeroed padding, unchecked `core_id`, `set_buffer` changing
the length, skipped zero-length callback) makes at least one check fail.

Note that FreeRTOS task creation is stubbed out here, so `artnet_start_task()`
is not covered. The synchronous `artnet_read()` path that the task wraps is.
