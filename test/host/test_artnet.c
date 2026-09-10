/* Host-side functional tests for the artnet component. Runs over real UDP on
 * 127.0.0.1, so the socket path, the parser and the packet builder are all
 * exercised for real. */

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "artnet.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        checks++;                                            \
        if (!(cond)) {                                       \
            failures++;                                      \
            printf("  FAIL %s:%d: ", __func__, __LINE__);    \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)

/* A second listening port so a node can receive injected packets while a
 * separate sink socket owns the real Art-Net port. */
#define ALT_PORT (ARTNET_PORT + 1)

#define OP_POLL_REPLY 0x2100

/* Last frame seen by the callback. */
static struct {
    int      calls;
    uint16_t universe;
    uint16_t length;
    uint8_t  sequence;
    uint8_t  data[512];
} g_rx;

static void on_dmx(const artnet_dmx_t *f, void *ctx)
{
    (void)ctx;
    g_rx.calls++;
    g_rx.universe = f->universe;
    g_rx.length = f->length;
    g_rx.sequence = f->sequence;
    memcpy(g_rx.data, f->data, f->length);
}

/* ---- helpers ------------------------------------------------------- */

static artnet_handle_t make_node(const char *host, uint16_t port, bool tx_only)
{
    artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
    artnet_handle_t h = NULL;

    cfg.host = host;
    cfg.port = port;
    cfg.tx_only = tx_only;
    cfg.dmx_cb = on_dmx;
    memset(&g_rx, 0, sizeof(g_rx));
    if (artnet_init(&cfg, &h) != ESP_OK) {
        printf("  FATAL: artnet_init failed\n");
        return NULL;
    }
    return h;
}

/* A plain socket on the Art-Net port that captures what a node transmits. */
static int make_sink(void)
{
    struct sockaddr_in addr;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ARTNET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("  FATAL: sink bind failed\n");
    }
    return s;
}

/* Inject a raw datagram into a port on loopback from a throwaway socket. */
static void inject(uint16_t port, const uint8_t *buf, size_t len)
{
    struct sockaddr_in to;
    int s = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = inet_addr("127.0.0.1");
    sendto(s, buf, len, 0, (struct sockaddr *)&to, sizeof(to));
    close(s);
}

/* Encode an Art-Net header. For ArtDmx the universe and length fields are
 * meaningful, for other op-codes the bytes are simply zero. */
static size_t build_header(uint8_t *b, uint16_t opcode, uint16_t universe, uint16_t claimed_len)
{
    memset(b, 0, ARTNET_DMX_START);
    memcpy(b, ARTNET_ID, 8);
    b[8] = (uint8_t)(opcode & 0xff);
    b[9] = (uint8_t)(opcode >> 8);
    b[10] = (uint8_t)(ARTNET_PROTOCOL_VER >> 8);
    b[11] = (uint8_t)(ARTNET_PROTOCOL_VER & 0xff);
    b[12] = 7;                            /* sequence */
    b[13] = 0;                            /* physical */
    b[14] = (uint8_t)(universe & 0xff);
    b[15] = (uint8_t)(universe >> 8);
    b[16] = (uint8_t)(claimed_len >> 8);  /* length, big endian */
    b[17] = (uint8_t)(claimed_len & 0xff);
    return ARTNET_DMX_START;
}

/* ---- tests --------------------------------------------------------- */

/* 1. Round trip: our own transmitter feeding our own receiver. */
static void test_round_trip(void)
{
    artnet_handle_t h = make_node("127.0.0.1", ARTNET_PORT, false);
    uint16_t opcode = 0;

    artnet_set_universe(h, 258);
    artnet_set_length(h, 5);
    for (uint16_t i = 0; i < 5; i++) {
        artnet_set_byte(h, i, (uint8_t)(0xA0 + i));
    }

    CHECK(artnet_write(h) == ESP_OK, "write failed");
    CHECK(artnet_read(h, 500, &opcode) == ESP_OK, "read failed");
    CHECK(opcode == ARTNET_OP_DMX, "opcode 0x%04x", opcode);
    CHECK(g_rx.calls == 1, "callback fired %d times", g_rx.calls);
    CHECK(g_rx.universe == 258, "universe %u", g_rx.universe);
    CHECK(g_rx.length == 6, "length %u, expected 5 rounded up to even", g_rx.length);
    CHECK(g_rx.data[0] == 0xA0 && g_rx.data[4] == 0xA4, "payload mismatch");
    CHECK(g_rx.data[5] == 0, "padding channel was %u, expected 0", g_rx.data[5]);
    CHECK(g_rx.sequence == 1, "first sequence was %u, expected 1", g_rx.sequence);

    /* Sequence must advance on each packet. */
    artnet_write(h);
    artnet_read(h, 500, NULL);
    CHECK(g_rx.sequence == 2, "second sequence was %u", g_rx.sequence);

    CHECK(artnet_deinit(h) == ESP_OK, "deinit failed");
}

/* 2. Exact wire format of a transmitted ArtDmx packet, via a tx_only node. */
static void test_wire_format(void)
{
    artnet_handle_t h = make_node("127.0.0.1", 0, true);
    int sink = make_sink();
    uint8_t buf[600];
    int n;

    artnet_set_universe(h, 0x0123);
    artnet_set_physical(h, 3);
    artnet_set_length(h, 3);
    artnet_write(h);

    n = (int)recv(sink, buf, sizeof(buf), 0);
    CHECK(n == 18 + 4, "sent %d bytes, expected 22", n);
    CHECK(memcmp(buf, "Art-Net", 8) == 0, "bad Art-Net ID");
    CHECK(buf[8] == 0x00 && buf[9] == 0x50, "opcode not little endian 0x5000");
    CHECK(buf[10] == 0x00 && buf[11] == 14, "protocol version not big endian 14");
    CHECK(buf[13] == 3, "physical %u", buf[13]);
    CHECK(buf[14] == 0x23 && buf[15] == 0x01, "universe not little endian");
    CHECK(buf[16] == 0x00 && buf[17] == 0x04, "length not big endian, even rounded");

    /* A tx_only node must not have taken the Art-Net port: the sink got the
     * packet, and nothing is waiting for the node itself. */
    CHECK(artnet_read(h, 0, NULL) == ESP_ERR_TIMEOUT, "tx_only node received its own packet");

    close(sink);
    artnet_deinit(h);
}

/* 3. An odd length pads with a zero, never with a stale channel. */
static void test_odd_length_padding(void)
{
    artnet_handle_t h = make_node("127.0.0.1", 0, true);
    int sink = make_sink();
    uint8_t buf[600];
    int n;

    /* Stage a full frame, then shrink to 5 channels. Channel 6 held 0xEE. */
    for (uint16_t i = 0; i < 512; i++) {
        artnet_set_byte(h, i, 0xEE);
    }
    artnet_set_length(h, 5);
    CHECK(artnet_get_length(h) == 6, "get_length %u, expected 6", artnet_get_length(h));

    artnet_write(h);
    n = (int)recv(sink, buf, sizeof(buf), 0);
    CHECK(n == 18 + 6, "sent %d bytes, expected 24", n);
    CHECK(buf[18 + 4] == 0xEE, "channel 5 lost");
    CHECK(buf[18 + 5] == 0x00, "padding channel carried stale 0x%02x", buf[18 + 5]);

    artnet_set_length(h, 512);
    CHECK(artnet_get_length(h) == 512, "512 stays 512");
    artnet_set_length(h, 600);
    CHECK(artnet_get_length(h) == 512, "clamp to 512 failed: %u", artnet_get_length(h));

    close(sink);
    artnet_deinit(h);
}

/* 4. A truncated ArtDmx must be rejected without disturbing prior state. */
static void test_truncated_dmx(void)
{
    artnet_handle_t h = make_node("127.0.0.1", ARTNET_PORT, false);
    uint8_t buf[64];
    uint16_t opcode = 0;

    /* Seed a good packet first so there is state that could go stale. */
    artnet_set_universe(h, 42);
    artnet_set_length(h, 4);
    artnet_write(h);
    artnet_read(h, 500, NULL);
    CHECK(artnet_get_universe(h) == 42, "setup universe %u", artnet_get_universe(h));

    /* Now a 12 byte ArtDmx: enough for the ID and opcode, nothing else. */
    build_header(buf, ARTNET_OP_DMX, 999, 512);
    inject(ARTNET_PORT, buf, 12);
    CHECK(artnet_read(h, 500, &opcode) == ESP_ERR_INVALID_RESPONSE,
          "truncated packet was not rejected");
    CHECK(g_rx.calls == 1, "callback fired on a truncated packet");
    CHECK(artnet_get_opcode(h) == ARTNET_OP_DMX, "opcode changed to 0x%04x", artnet_get_opcode(h));
    CHECK(artnet_get_universe(h) == 42, "universe leaked: %u", artnet_get_universe(h));
    CHECK(artnet_get_rx_length(h) == 4, "length leaked: %u", artnet_get_rx_length(h));

    artnet_deinit(h);
}

/* 5. A packet claiming more DMX data than it carries must be clamped. */
static void test_length_clamp(void)
{
    artnet_handle_t h = make_node("127.0.0.1", ARTNET_PORT, false);
    uint8_t buf[64];

    build_header(buf, ARTNET_OP_DMX, 7, 512);   /* claims 512 ... */
    memset(buf + 18, 0x5A, 2);
    inject(ARTNET_PORT, buf, 20);               /* ... but carries 2 */

    CHECK(artnet_read(h, 500, NULL) == ESP_OK, "read failed");
    CHECK(g_rx.calls == 1, "callback fired %d times", g_rx.calls);
    CHECK(g_rx.length == 2, "length %u, expected clamp to 2", g_rx.length);
    CHECK(artnet_get_rx_length(h) == 2, "accessor length %u", artnet_get_rx_length(h));

    artnet_deinit(h);
}

/* 6. A zero-length ArtDmx (keep-alive) still reaches the callback. */
static void test_zero_length_dmx(void)
{
    artnet_handle_t h = make_node("127.0.0.1", ARTNET_PORT, false);
    uint8_t buf[18];

    build_header(buf, ARTNET_OP_DMX, 3, 0);
    inject(ARTNET_PORT, buf, sizeof(buf));

    CHECK(artnet_read(h, 500, NULL) == ESP_OK, "read failed");
    CHECK(g_rx.calls == 1, "callback fired %d times, expected 1", g_rx.calls);
    CHECK(g_rx.length == 0, "length %u, expected 0", g_rx.length);
    CHECK(g_rx.universe == 3, "universe %u", g_rx.universe);

    artnet_deinit(h);
}

/* 7. Non Art-Net traffic on the port must be ignored. */
static void test_foreign_packet(void)
{
    artnet_handle_t h = make_node("127.0.0.1", ARTNET_PORT, false);
    const uint8_t junk[32] = "not an art-net packet at all";

    inject(ARTNET_PORT, junk, sizeof(junk));
    CHECK(artnet_read(h, 500, NULL) == ESP_ERR_INVALID_RESPONSE, "junk not rejected");
    CHECK(g_rx.calls == 0, "callback fired on junk");

    artnet_deinit(h);
}

/* 8. ArtPoll: reported by opcode, kept away from the DMX callback, and
 *    answered with an ArtPollReply unicast to the poller on port 6454. */
static void test_artpoll(void)
{
    artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
    artnet_handle_t h = NULL;
    int sink = make_sink();          /* plays the controller, owns port 6454 */
    uint8_t buf[300];
    uint16_t opcode = 0;
    int n;

    cfg.port = ALT_PORT;
    cfg.dmx_cb = on_dmx;
    cfg.node.short_name = "TestNode";
    cfg.node.long_name = "Host test Art-Net node";
    cfg.node.first_universe = 0x0123;   /* net 1, sub-net 2, universe 3 */
    cfg.node.num_ports = 2;
    memcpy(cfg.node.mac, "\x02\x11\x22\x33\x44\x55", 6);
    memset(&g_rx, 0, sizeof(g_rx));
    CHECK(artnet_init(&cfg, &h) == ESP_OK, "init failed");

    /* A real ArtPoll is 14 bytes: header through TalkToMe and Priority. */
    build_header(buf, ARTNET_OP_POLL, 0, 0);
    inject(ALT_PORT, buf, 14);

    CHECK(artnet_read(h, 500, &opcode) == ESP_OK, "ArtPoll read failed");
    CHECK(opcode == ARTNET_OP_POLL, "opcode 0x%04x", opcode);
    CHECK(g_rx.calls == 0, "DMX callback fired on ArtPoll");

    n = (int)recv(sink, buf, sizeof(buf), 0);
    CHECK(n == ARTNET_POLL_REPLY_LEN, "reply is %d bytes, expected %d", n, ARTNET_POLL_REPLY_LEN);
    if (n == ARTNET_POLL_REPLY_LEN) {
        uint32_t ip;

        memcpy(&ip, buf + 10, 4);
        CHECK(memcmp(buf, "Art-Net", 8) == 0, "reply ID");
        CHECK(buf[8] == 0x00 && buf[9] == 0x21, "reply opcode not 0x2100 LE");
        CHECK(ip == inet_addr("127.0.0.1"), "reply IP not the node's own address");
        CHECK(buf[14] == 0x36 && buf[15] == 0x19, "reply port not 6454 LE");
        CHECK(buf[18] == 0x01, "NetSwitch %u", buf[18]);
        CHECK(buf[19] == 0x02, "SubSwitch %u", buf[19]);
        CHECK(strcmp((const char *)buf + 26, "TestNode") == 0, "short name '%s'", buf + 26);
        CHECK(strcmp((const char *)buf + 44, "Host test Art-Net node") == 0, "long name");
        CHECK(strncmp((const char *)buf + 108, "#0001 [0001]", 12) == 0, "node report '%.20s'", buf + 108);
        CHECK(buf[172] == 0 && buf[173] == 2, "NumPorts %u/%u", buf[172], buf[173]);
        CHECK(buf[174] == 0x80 && buf[175] == 0x80 && buf[176] == 0x00, "PortTypes");
        CHECK(buf[190] == 3 && buf[191] == 4, "SwOut %u %u", buf[190], buf[191]);
        CHECK(buf[200] == 0x00, "Style not StNode");
        CHECK(memcmp(buf + 201, "\x02\x11\x22\x33\x44\x55", 6) == 0, "MAC");
        CHECK(buf[211] == 1, "BindIndex %u", buf[211]);
        CHECK(buf[212] & 0x08, "Status2 lacks 15-bit addressing bit");
        CHECK(buf[182] == 0x00, "GoodOutput claims data before any DMX arrived");
    }

    /* After a DMX frame the outputs report as active and the counter moves. */
    build_header(buf, ARTNET_OP_DMX, 0x0123, 2);
    inject(ALT_PORT, buf, 20);
    artnet_read(h, 500, NULL);
    CHECK(artnet_send_poll_reply(h, inet_addr("127.0.0.1")) == ESP_OK, "unsolicited reply failed");
    n = (int)recv(sink, buf, sizeof(buf), 0);
    CHECK(n == ARTNET_POLL_REPLY_LEN, "unsolicited reply %d bytes", n);
    CHECK(n > 182 && buf[182] == 0x80, "GoodOutput not active after DMX");
    CHECK(n > 120 && strncmp((const char *)buf + 108, "#0001 [0002]", 12) == 0, "poll counter '%.14s'", buf + 108);

    artnet_deinit(h);

    /* answer_poll = false must stay silent. */
    cfg.node.answer_poll = false;
    CHECK(artnet_init(&cfg, &h) == ESP_OK, "init (no reply) failed");
    build_header(buf, ARTNET_OP_POLL, 0, 0);
    inject(ALT_PORT, buf, 14);
    CHECK(artnet_read(h, 500, &opcode) == ESP_OK && opcode == ARTNET_OP_POLL, "poll not seen");
    {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(sink, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        n = (int)recv(sink, buf, sizeof(buf), 0);
        CHECK(n < 0, "reply sent although answer_poll is false");
    }
    artnet_deinit(h);

    /* Ports must stay inside one sub-net: universe 14 leaves room for 2. */
    cfg.node.answer_poll = true;
    cfg.node.first_universe = 14;
    cfg.node.num_ports = 4;
    CHECK(artnet_init(&cfg, &h) == ESP_OK, "init (clamp) failed");
    artnet_send_poll_reply(h, inet_addr("127.0.0.1"));
    n = (int)recv(sink, buf, sizeof(buf), 0);
    CHECK(n == ARTNET_POLL_REPLY_LEN && buf[173] == 2, "num_ports not clamped: %u", buf[173]);
    CHECK(n > 191 && buf[190] == 14 && buf[191] == 15, "SwOut after clamp %u %u", buf[190], buf[191]);
    artnet_deinit(h);

    close(sink);
}

/* 9. A non-DMX packet after a DMX frame must not leave DMX accessors
 *    describing data that is no longer in the buffer. */
static void test_non_dmx_clears_dmx_state(void)
{
    artnet_handle_t h = make_node("127.0.0.1", ARTNET_PORT, false);
    uint8_t buf[300];

    /* A full DMX frame of 0xAA on universe 5. */
    build_header(buf, ARTNET_OP_DMX, 5, 200);
    memset(buf + 18, 0xAA, 200);
    inject(ARTNET_PORT, buf, 218);
    CHECK(artnet_read(h, 500, NULL) == ESP_OK, "dmx read failed");
    CHECK(artnet_get_rx_length(h) == 200, "setup length %u", artnet_get_rx_length(h));

    /* Then an ArtPollReply, which real controllers broadcast on the port. */
    build_header(buf, OP_POLL_REPLY, 0, 0);
    memset(buf + 18, 0x11, 221);
    inject(ARTNET_PORT, buf, 239);
    CHECK(artnet_read(h, 500, NULL) == ESP_OK, "poll reply read failed");
    CHECK(artnet_get_opcode(h) == OP_POLL_REPLY, "opcode 0x%04x", artnet_get_opcode(h));
    CHECK(artnet_get_rx_length(h) == 0,
          "rx_length %u after a non-DMX packet, expected 0", artnet_get_rx_length(h));
    CHECK(g_rx.calls == 1, "DMX callback fired on ArtPollReply");

    artnet_deinit(h);
}

/* 10. Receiving must never touch the transmit buffer. */
static void test_buffers_independent(void)
{
    artnet_handle_t h = make_node("127.0.0.1", ALT_PORT, false);
    int sink = make_sink();
    uint8_t buf[600];
    int n;

    /* Stage a transmit frame of 0x33. */
    artnet_set_length(h, 8);
    for (uint16_t i = 0; i < 8; i++) {
        artnet_set_byte(h, i, 0x33);
    }

    /* A foreign frame of 0x77 arrives and is consumed. */
    build_header(buf, ARTNET_OP_DMX, 9, 8);
    memset(buf + 18, 0x77, 8);
    inject(ALT_PORT, buf, 26);
    CHECK(artnet_read(h, 500, NULL) == ESP_OK, "rx failed");
    CHECK(g_rx.data[0] == 0x77, "rx payload wrong");

    /* The staged frame must go out untouched. */
    artnet_write(h);
    n = (int)recv(sink, buf, sizeof(buf), 0);
    CHECK(n == 18 + 8, "sent %d bytes", n);
    CHECK(buf[18] == 0x33 && buf[18 + 7] == 0x33,
          "transmit buffer corrupted by receive: 0x%02x", buf[18]);
    CHECK(buf[14] == 0 && buf[15] == 0, "tx universe changed by receive");

    close(sink);
    artnet_deinit(h);
}

/* 11. Channel index bounds and set_buffer semantics. */
static void test_tx_buffer_api(void)
{
    artnet_handle_t h = make_node("127.0.0.1", 0, true);

    CHECK(artnet_set_byte(h, 0, 1) == ESP_OK, "index 0 rejected");
    CHECK(artnet_set_byte(h, 511, 1) == ESP_OK, "index 511 rejected");
    CHECK(artnet_set_byte(h, 512, 1) == ESP_ERR_INVALID_ARG, "index 512 accepted");
    CHECK(artnet_set_byte(h, 65535, 1) == ESP_ERR_INVALID_ARG, "index 65535 accepted");

    artnet_set_length(h, 510);
    CHECK(artnet_set_buffer(h, 0, (const uint8_t *)"abc", 3) == ESP_OK, "buffer 0+3 rejected");
    CHECK(artnet_get_length(h) == 510,
          "set_buffer changed the length to %u, it must only copy", artnet_get_length(h));
    CHECK(artnet_get_tx_dmx(h)[1] == 'b', "set_buffer did not copy");
    CHECK(artnet_set_buffer(h, 509, (const uint8_t *)"abc", 3) == ESP_OK, "buffer 509+3 rejected");
    CHECK(artnet_set_buffer(h, 510, (const uint8_t *)"abc", 3) == ESP_ERR_INVALID_SIZE,
          "buffer 510+3 accepted, that overruns");
    CHECK(artnet_set_buffer(h, 512, (const uint8_t *)"a", 1) == ESP_ERR_INVALID_SIZE,
          "buffer 512+1 accepted");
    CHECK(artnet_set_buffer(h, 512, (const uint8_t *)"a", 0) == ESP_OK,
          "zero-length copy at the end rejected");

    artnet_deinit(h);
}

/* 12. Timeout paths: non-blocking poll and a bounded wait on an idle socket. */
static void test_timeouts(void)
{
    artnet_handle_t h = make_node("127.0.0.1", ARTNET_PORT, false);

    CHECK(artnet_read(h, 0, NULL) == ESP_ERR_TIMEOUT, "non-blocking poll did not time out");
    CHECK(artnet_read(h, 50, NULL) == ESP_ERR_TIMEOUT, "50 ms wait did not time out");
    /* Back to back, to exercise the cached SO_RCVTIMEO path. */
    CHECK(artnet_read(h, 50, NULL) == ESP_ERR_TIMEOUT, "cached timeout path broke");
    CHECK(artnet_read(h, 0, NULL) == ESP_ERR_TIMEOUT, "poll after timed wait broke");
    /* Odd millisecond values exercise the round-up-to-tick path; on the host
     * a tick is 1 ms so this is only a smoke test of the arithmetic. */
    CHECK(artnet_read(h, 7, NULL) == ESP_ERR_TIMEOUT, "7 ms wait broke");

    artnet_deinit(h);
}

/* 13. Sequence must wrap 255 -> 1, never 0 (0 means sequencing disabled). */
static void test_sequence_wrap(void)
{
    artnet_handle_t h = make_node("127.0.0.1", ARTNET_PORT, false);
    int saw_zero = 0;
    int saw_wrap = 0;

    artnet_set_length(h, 2);
    for (int i = 0; i < 256; i++) {
        artnet_write(h);
        if (artnet_read(h, 100, NULL) != ESP_OK) {
            CHECK(0, "round trip %d lost", i);
            break;
        }
        if (g_rx.sequence == 0) {
            saw_zero = 1;
        }
        if (i == 255 && g_rx.sequence == 1) {
            saw_wrap = 1;
        }
    }
    CHECK(saw_zero == 0, "sequence 0 was transmitted");
    CHECK(saw_wrap == 1, "sequence did not wrap to 1 on packet 256");

    artnet_deinit(h);
}

/* 14. artnet_write_ip() sends to a raw address, the reply-to-sender path. */
static void test_write_ip(void)
{
    artnet_handle_t h = make_node(NULL, 0, true);
    int sink = make_sink();
    uint8_t buf[600];
    int n;

    /* No host configured: write() must refuse, write_ip() must work. */
    CHECK(artnet_write(h) == ESP_ERR_INVALID_STATE, "write without host accepted");

    artnet_set_length(h, 2);
    artnet_set_byte(h, 0, 0x42);
    CHECK(artnet_write_ip(h, inet_addr("127.0.0.1")) == ESP_OK, "write_ip failed");
    n = (int)recv(sink, buf, sizeof(buf), 0);
    CHECK(n == 20, "write_ip sent %d bytes", n);
    CHECK(n >= 19 && buf[18] == 0x42, "write_ip payload wrong");

    /* write_to() with a literal address takes the resolver path. */
    CHECK(artnet_write_to(h, "127.0.0.1") == ESP_OK, "write_to failed");
    n = (int)recv(sink, buf, sizeof(buf), 0);
    CHECK(n == 20, "write_to sent %d bytes", n);
    CHECK(artnet_write_to(h, "") == ESP_ERR_INVALID_ARG, "empty host accepted");

    close(sink);
    artnet_deinit(h);
}

/* 15. Task lifecycle guards that do not need a real scheduler. */
static void test_task_guards(void)
{
    artnet_handle_t h = make_node("127.0.0.1", 0, true);
    artnet_task_config_t tcfg = ARTNET_TASK_CONFIG_DEFAULT();

    tcfg.core_id = 7;
    CHECK(artnet_start_task(h, &tcfg) == ESP_ERR_INVALID_ARG,
          "core_id 7 accepted, would assert inside FreeRTOS");
    tcfg.core_id = -2;
    CHECK(artnet_start_task(h, &tcfg) == ESP_ERR_INVALID_ARG, "core_id -2 accepted");

    /* The host stub cannot create tasks; the component must report that
     * cleanly and leave the handle usable. */
    CHECK(artnet_start_task(h, NULL) == ESP_ERR_NO_MEM, "stubbed task creation not reported");
    CHECK(artnet_stop_task(h) == ESP_OK, "stop with no task running failed");
    CHECK(artnet_read(h, 0, NULL) == ESP_ERR_TIMEOUT, "handle unusable after failed start");

    CHECK(artnet_deinit(NULL) == ESP_ERR_INVALID_ARG, "deinit(NULL) accepted");
    CHECK(artnet_deinit(h) == ESP_OK, "deinit failed");
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "round trip",                test_round_trip },
        { "wire format",               test_wire_format },
        { "odd length padding",        test_odd_length_padding },
        { "truncated ArtDmx",          test_truncated_dmx },
        { "length clamp",              test_length_clamp },
        { "zero-length ArtDmx",        test_zero_length_dmx },
        { "foreign packet",            test_foreign_packet },
        { "ArtPoll and ArtPollReply",  test_artpoll },
        { "non-DMX clears DMX state",  test_non_dmx_clears_dmx_state },
        { "buffers independent",       test_buffers_independent },
        { "tx buffer api",             test_tx_buffer_api },
        { "timeouts",                  test_timeouts },
        { "sequence wrap",             test_sequence_wrap },
        { "write_ip / write_to",       test_write_ip },
        { "task guards",               test_task_guards },
    };

    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;
        printf("test: %s\n", tests[i].name);
        tests[i].fn();
        if (failures == before) {
            printf("  ok\n");
        }
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
