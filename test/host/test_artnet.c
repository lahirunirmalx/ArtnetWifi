/* Host-side functional tests for the artnet component. Runs over real UDP on
 * 127.0.0.1, so the socket path, the parser and the packet builder are all
 * exercised for real. */

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
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

static artnet_handle_t make_node(const char *host)
{
    artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
    artnet_handle_t h = NULL;

    cfg.host = host;
    cfg.dmx_cb = on_dmx;
    if (artnet_init(&cfg, &h) != ESP_OK) {
        printf("  FATAL: artnet_init failed\n");
        return NULL;
    }
    return h;
}

/* Inject a raw datagram into the node's port from a throwaway socket. */
static void inject(const uint8_t *buf, size_t len)
{
    struct sockaddr_in to;
    int s = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(ARTNET_PORT);
    to.sin_addr.s_addr = inet_addr("127.0.0.1");
    sendto(s, buf, len, 0, (struct sockaddr *)&to, sizeof(to));
    close(s);
}

static size_t build_dmx_header(uint8_t *b, uint16_t universe, uint16_t claimed_len)
{
    memcpy(b, "Art-Net", 8);
    b[8] = 0x00; b[9] = 0x50;              /* ArtDmx, little endian */
    b[10] = 0x00; b[11] = 14;              /* protocol version, big endian */
    b[12] = 7;                             /* sequence */
    b[13] = 0;                             /* physical */
    b[14] = universe & 0xff;
    b[15] = universe >> 8;
    b[16] = claimed_len >> 8;              /* length, big endian */
    b[17] = claimed_len & 0xff;
    return 18;
}

/* 1. Round trip: our own transmitter feeding our own receiver. */
static void test_round_trip(void)
{
    artnet_handle_t h = make_node("127.0.0.1");
    uint16_t opcode = 0;

    memset(&g_rx, 0, sizeof(g_rx));
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
    CHECK(g_rx.sequence == 1, "first sequence was %u, expected 1", g_rx.sequence);

    /* Sequence must advance on each packet. */
    artnet_write(h);
    artnet_read(h, 500, NULL);
    CHECK(g_rx.sequence == 2, "second sequence was %u", g_rx.sequence);

    artnet_deinit(h);
}

/* 2. Exact wire format of a transmitted ArtDmx packet. */
static void test_wire_format(void)
{
    artnet_config_t cfg = ARTNET_CONFIG_DEFAULT();
    artnet_handle_t h = NULL;
    struct sockaddr_in sink_addr;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    uint8_t buf[600];
    int sink;
    int n;

    /* A plain socket on the Art-Net port captures what the node transmits,
     * while the node itself listens elsewhere so it does not eat its own
     * packet. */
    sink = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&sink_addr, 0, sizeof(sink_addr));
    sink_addr.sin_family = AF_INET;
    sink_addr.sin_port = htons(ARTNET_PORT);
    sink_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    CHECK(bind(sink, (struct sockaddr *)&sink_addr, sizeof(sink_addr)) == 0, "sink bind failed");
    setsockopt(sink, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    cfg.port = ARTNET_PORT + 1;
    cfg.host = "127.0.0.1";
    CHECK(artnet_init(&cfg, &h) == ESP_OK, "init failed");

    artnet_set_universe(h, 0x0123);
    artnet_set_physical(h, 3);
    artnet_set_length(h, 3);
    artnet_write(h);

    n = (int)recv(sink, buf, sizeof(buf), 0);
    close(sink);
    CHECK(n == 18 + 4, "sent %d bytes, expected 22", n);
    CHECK(memcmp(buf, "Art-Net", 8) == 0, "bad Art-Net ID");
    CHECK(buf[8] == 0x00 && buf[9] == 0x50, "opcode not little endian 0x5000");
    CHECK(buf[10] == 0x00 && buf[11] == 14, "protocol version not big endian 14");
    CHECK(buf[13] == 3, "physical %u", buf[13]);
    CHECK(buf[14] == 0x23 && buf[15] == 0x01, "universe not little endian");
    CHECK(buf[16] == 0x00 && buf[17] == 0x04, "length not big endian, even rounded");

    artnet_deinit(h);
}

/* 3. A truncated ArtDmx must be rejected without disturbing prior state. */
static void test_truncated_dmx(void)
{
    artnet_handle_t h = make_node("127.0.0.1");
    uint8_t buf[64];
    uint16_t opcode = 0;

    /* Seed a good packet first so there is state that could go stale. */
    memset(&g_rx, 0, sizeof(g_rx));
    artnet_set_universe(h, 42);
    artnet_set_length(h, 4);
    artnet_write(h);
    artnet_read(h, 500, NULL);
    CHECK(artnet_get_universe(h) == 42, "setup universe %u", artnet_get_universe(h));

    /* Now a 12 byte ArtDmx: enough for the ID and opcode, nothing else. */
    build_dmx_header(buf, 999, 512);
    inject(buf, 12);
    CHECK(artnet_read(h, 500, &opcode) == ESP_ERR_INVALID_RESPONSE,
          "truncated packet was not rejected");
    CHECK(g_rx.calls == 1, "callback fired on a truncated packet");
    CHECK(artnet_get_universe(h) == 42, "stale universe leaked: %u", artnet_get_universe(h));
    CHECK(artnet_get_opcode(h) != ARTNET_OP_DMX || artnet_get_universe(h) == 42,
          "opcode says DMX with no valid DMX state");

    artnet_deinit(h);
}

/* 4. A packet claiming more DMX data than it carries must be clamped. */
static void test_length_clamp(void)
{
    artnet_handle_t h = make_node("127.0.0.1");
    uint8_t buf[64];

    memset(&g_rx, 0, sizeof(g_rx));
    build_dmx_header(buf, 7, 512);   /* claims 512 ... */
    memset(buf + 18, 0x5A, 2);
    inject(buf, 20);                 /* ... but carries 2 */

    CHECK(artnet_read(h, 500, NULL) == ESP_OK, "read failed");
    CHECK(g_rx.calls == 1, "callback fired %d times", g_rx.calls);
    CHECK(g_rx.length == 2, "length %u, expected clamp to 2", g_rx.length);
    CHECK(artnet_get_rx_length(h) == 2, "accessor length %u", artnet_get_rx_length(h));

    artnet_deinit(h);
}

/* 5. Non Art-Net traffic on the port must be ignored. */
static void test_foreign_packet(void)
{
    artnet_handle_t h = make_node("127.0.0.1");
    const uint8_t junk[32] = "not an art-net packet at all";

    memset(&g_rx, 0, sizeof(g_rx));
    inject(junk, sizeof(junk));
    CHECK(artnet_read(h, 500, NULL) == ESP_ERR_INVALID_RESPONSE, "junk not rejected");
    CHECK(g_rx.calls == 0, "callback fired on junk");

    artnet_deinit(h);
}

/* 6. ArtPoll is reported by opcode and does not reach the DMX callback. */
static void test_artpoll(void)
{
    artnet_handle_t h = make_node("127.0.0.1");
    uint8_t buf[14];
    uint16_t opcode = 0;

    memset(&g_rx, 0, sizeof(g_rx));
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "Art-Net", 8);
    buf[8] = 0x00; buf[9] = 0x20;    /* ArtPoll */
    buf[10] = 0x00; buf[11] = 14;
    inject(buf, sizeof(buf));

    CHECK(artnet_read(h, 500, &opcode) == ESP_OK, "ArtPoll read failed");
    CHECK(opcode == ARTNET_OP_POLL, "opcode 0x%04x", opcode);
    CHECK(g_rx.calls == 0, "DMX callback fired on ArtPoll");

    artnet_deinit(h);
}

/* 7. Channel index bounds. 512 channels means valid indices 0..511. */
static void test_set_byte_bounds(void)
{
    artnet_handle_t h = make_node("127.0.0.1");

    CHECK(artnet_set_byte(h, 0, 1) == ESP_OK, "index 0 rejected");
    CHECK(artnet_set_byte(h, 511, 1) == ESP_OK, "index 511 rejected");
    CHECK(artnet_set_byte(h, 512, 1) == ESP_ERR_INVALID_ARG, "index 512 accepted");
    CHECK(artnet_set_byte(h, 65535, 1) == ESP_ERR_INVALID_ARG, "index 65535 accepted");

    CHECK(artnet_set_buffer(h, 0, (const uint8_t *)"abc", 3) == ESP_OK, "buffer 0+3 rejected");
    CHECK(artnet_set_buffer(h, 510, (const uint8_t *)"abc", 3) == ESP_ERR_INVALID_SIZE,
          "buffer 510+3 accepted, that overruns");
    CHECK(artnet_get_length(h) == 3, "length %u after set_buffer", artnet_get_length(h));

    artnet_deinit(h);
}

/* 8. Timeout paths: non-blocking poll and a bounded wait on an idle socket. */
static void test_timeouts(void)
{
    artnet_handle_t h = make_node("127.0.0.1");

    CHECK(artnet_read(h, 0, NULL) == ESP_ERR_TIMEOUT, "non-blocking poll did not time out");
    CHECK(artnet_read(h, 50, NULL) == ESP_ERR_TIMEOUT, "50 ms wait did not time out");
    /* Back to back, to exercise the cached SO_RCVTIMEO path. */
    CHECK(artnet_read(h, 50, NULL) == ESP_ERR_TIMEOUT, "cached timeout path broke");
    CHECK(artnet_read(h, 0, NULL) == ESP_ERR_TIMEOUT, "poll after timed wait broke");

    artnet_deinit(h);
}

/* 9. Sequence must wrap 255 -> 1, never 0 (0 means sequencing disabled). */
static void test_sequence_wrap(void)
{
    artnet_handle_t h = make_node("127.0.0.1");
    int saw_zero = 0;
    int saw_wrap = 0;

    artnet_set_length(h, 2);
    for (int i = 0; i < 300; i++) {
        artnet_write(h);
        if (artnet_read(h, 500, NULL) == ESP_OK) {
            if (g_rx.sequence == 0) {
                saw_zero = 1;
            }
            if (i > 250 && g_rx.sequence == 1) {
                saw_wrap = 1;
            }
        }
    }
    CHECK(saw_zero == 0, "sequence 0 was transmitted");
    CHECK(saw_wrap == 1, "sequence never wrapped back to 1 over 300 packets");

    artnet_deinit(h);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "round trip",        test_round_trip },
        { "wire format",       test_wire_format },
        { "truncated ArtDmx",  test_truncated_dmx },
        { "length clamp",      test_length_clamp },
        { "foreign packet",    test_foreign_packet },
        { "ArtPoll",           test_artpoll },
        { "set_byte bounds",   test_set_byte_bounds },
        { "timeouts",          test_timeouts },
        { "sequence wrap",     test_sequence_wrap },
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
