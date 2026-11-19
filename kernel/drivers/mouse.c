// PS/2 mouse on the aux port: standard 3-byte packets or IntelliMouse 4-byte
// packets with a scroll wheel.
#include "ps2.h"
#include "string.h"

void mouse_state_init(struct mouse_state *s, bool wheel) {
    memset(s, 0, sizeof *s);
    s->packet_len = wheel ? 4 : 3;
}

bool mouse_decode(struct mouse_state *s, uint8_t byte, struct input_event *ev) {
    // Resync: the first byte of every packet has bit 3 set.
    if (s->idx == 0 && !(byte & 0x08)) return false;
    s->buf[s->idx++] = byte;
    if (s->idx < s->packet_len) return false;
    s->idx = 0;

    uint8_t b0 = s->buf[0];
    if (b0 & 0xC0) return false;   // X/Y overflow: discard

    int dx = (int)s->buf[1] - ((b0 & 0x10) ? 256 : 0);
    int dy = (int)s->buf[2] - ((b0 & 0x20) ? 256 : 0);
    int wheel = 0;
    if (s->packet_len == 4) {
        int z = s->buf[3] & 0x0F;          // signed 4-bit, negative = up
        if (z & 0x08) z -= 16;
        wheel = z < 0 ? 1 : (z > 0 ? -1 : 0);
    }

    memset(ev, 0, sizeof *ev);
    ev->type = INPUT_MOUSE;
    ev->dx = (int16_t)dx;
    ev->dy = (int16_t)-dy;                 // PS/2 +y is up; screen +y is down
    ev->wheel = (int8_t)wheel;
    ev->buttons = (uint8_t)((b0 & 0x01 ? MOUSE_LEFT : 0) |
                            (b0 & 0x02 ? MOUSE_RIGHT : 0) |
                            (b0 & 0x04 ? MOUSE_MIDDLE : 0));
    return true;
}

static bool mouse_set_rate(uint8_t rate) {
    return ps2_dev_send(2, 0xF3) && ps2_dev_send(2, rate);
}

bool mouse_init_device(bool *wheel) {
    *wheel = false;
    if (!ps2_dev_send(2, 0xF6)) return false;   // set defaults
    // IntelliMouse knock sequence, then read the device ID.
    if (mouse_set_rate(200) && mouse_set_rate(100) && mouse_set_rate(80) &&
        ps2_dev_send(2, 0xF2)) {
        uint8_t id;
        if (ps2_read(&id) && id == 3) *wheel = true;
    }
    mouse_set_rate(100);
    if (!ps2_dev_send(2, 0xF4)) return false;   // enable data reporting
    mouse_set_wheel(*wheel);
    return true;
}

static struct mouse_state mouse = { .packet_len = 3 };

void mouse_set_wheel(bool wheel) { mouse_state_init(&mouse, wheel); }

void mouse_irq_feed(uint8_t byte) {
    struct input_event ev;
    if (mouse_decode(&mouse, byte, &ev)) input_push(&ev);
}

// ---- self-test: feed known packets through the real decoder ----

static const char *expect_packet(struct mouse_state *s, const uint8_t *bytes, int n,
                                 bool want, int dx, int dy, int wheel, uint8_t buttons,
                                 const char *what) {
    struct input_event ev;
    bool got = false;
    for (int i = 0; i < n; i++) {
        bool r = mouse_decode(s, bytes[i], &ev);
        if (r && i != n - 1) return what;
        got = r;
    }
    if (got != want) return what;
    if (want && (ev.type != INPUT_MOUSE || ev.dx != dx || ev.dy != dy ||
                 ev.wheel != wheel || ev.buttons != buttons))
        return what;
    return 0;
}

#define EXPECTP(st, want, dx, dy, wheel, buttons, what, ...) do { \
        static const uint8_t b_[] = { __VA_ARGS__ }; \
        const char *e_ = expect_packet(st, b_, (int)sizeof b_, want, dx, dy, wheel, buttons, what); \
        if (e_) return e_; \
    } while (0)

const char *mouse_selftest(void) {
    struct mouse_state s3, s4;
    mouse_state_init(&s3, false);
    mouse_state_init(&s4, true);
    // left button, +5 right, +3 up (PS/2) => screen dy -3
    EXPECTP(&s3, true, 5, -3, 0, MOUSE_LEFT, "mouse: basic packet", 0x09, 0x05, 0x03);
    // negative x/y: dx=-5, PS/2 dy=-2 => screen +2, right button
    EXPECTP(&s3, true, -5, 2, 0, MOUSE_RIGHT, "mouse: negative", 0x3A, 0xFB, 0xFE);
    // garbage byte without bit 3 is dropped (resync), then a valid packet
    EXPECTP(&s3, true, 1, 0, 0, MOUSE_MIDDLE, "mouse: resync", 0x00, 0x0C, 0x01, 0x00);
    // overflow packet discarded, decoder stays aligned
    EXPECTP(&s3, false, 0, 0, 0, 0, "mouse: overflow", 0x48, 0x10, 0x10);
    EXPECTP(&s3, true, 0, 0, 0, 0, "mouse: after overflow", 0x08, 0x00, 0x00);
    // wheel: z=-1 => scroll up (+1); z=+1 => down (-1)
    EXPECTP(&s4, true, 2, -1, 1, 0, "mouse: wheel up", 0x08, 0x02, 0x01, 0xFF);
    EXPECTP(&s4, true, 0, 0, -1, MOUSE_LEFT, "mouse: wheel down", 0x09, 0x00, 0x00, 0x01);
    return 0;
}
