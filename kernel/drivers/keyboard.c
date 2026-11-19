// PS/2 keyboard: scancode set 1 (controller translation on), US layout.
#include "ps2.h"
#include "string.h"

#define K(x) ((uint16_t)(x))

// Base keycodes for set-1 make codes 0x00-0x58 (lowercase ASCII or enum keycode).
static const uint16_t set1_keys[0x59] = {
    [0x01] = KEY_ESC,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = KEY_ENTER, [0x1D] = KEY_LCTRL,
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2A] = KEY_LSHIFT, [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x36] = KEY_RSHIFT, [0x37] = '*', [0x38] = KEY_LALT, [0x39] = ' ',
    [0x3A] = KEY_CAPSLOCK,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    // 0x45 num lock, 0x46 scroll lock: KEY_UNKNOWN
    // keypad (treated as num lock on)
    [0x47] = '7', [0x48] = '8', [0x49] = '9', [0x4A] = '-',
    [0x4B] = '4', [0x4C] = '5', [0x4D] = '6', [0x4E] = '+',
    [0x4F] = '1', [0x50] = '2', [0x51] = '3', [0x52] = '0', [0x53] = '.',
    [0x57] = KEY_F11, [0x58] = KEY_F12,
};

static uint16_t set1_e0_key(uint8_t code) {
    switch (code) {
    case 0x1C: return KEY_ENTER;      // keypad enter
    case 0x1D: return KEY_RCTRL;
    case 0x35: return '/';            // keypad slash
    case 0x38: return KEY_RALT;
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PGUP;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x4F: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    case 0x5B: return KEY_LSUPER;
    case 0x5C: return KEY_RSUPER;
    default:   return KEY_UNKNOWN;
    }
}

static char shifted(uint16_t k) {
    if (k >= 'a' && k <= 'z') return (char)(k - 'a' + 'A');
    switch (k) {
    case '1': return '!'; case '2': return '@'; case '3': return '#';
    case '4': return '$'; case '5': return '%'; case '6': return '^';
    case '7': return '&'; case '8': return '*'; case '9': return '(';
    case '0': return ')'; case '-': return '_'; case '=': return '+';
    case '[': return '{'; case ']': return '}'; case ';': return ':';
    case '\'': return '"'; case '`': return '~'; case '\\': return '|';
    case ',': return '<'; case '.': return '>'; case '/': return '?';
    default: return (char)k;
    }
}

static char translate(uint16_t k, uint8_t mods, bool keypad) {
    if (k == KEY_BACKSPACE || k == KEY_TAB || k == KEY_ENTER || k == KEY_ESC)
        return (char)k;
    if (k < 0x20 || k > 0x7e) return 0;
    if (keypad) return (char)k;
    bool shift = (mods & MOD_SHIFT) != 0;
    if (k >= 'a' && k <= 'z') {
        bool upper = shift != ((mods & MOD_CAPS) != 0);
        return upper ? shifted(k) : (char)k;
    }
    return shift ? shifted(k) : (char)k;
}

static void recompute_mods(struct kbd_state *s) {
    uint8_t m = s->mods & MOD_CAPS;
    if (s->lshift || s->rshift) m |= MOD_SHIFT;
    if (s->lctrl || s->rctrl)   m |= MOD_CTRL;
    if (s->lalt || s->ralt)     m |= MOD_ALT;
    if (s->lsuper || s->rsuper) m |= MOD_SUPER;
    s->mods = m;
}

bool kbd_decode(struct kbd_state *s, uint8_t byte, struct input_event *ev) {
    if (s->e1_skip) { s->e1_skip--; return false; }
    if (byte == 0xE1) { s->e1_skip = 5; s->e0 = false; return false; } // Pause
    if (byte == 0xE0) { s->e0 = true; return false; }
    // Controller/keyboard responses, not scancodes.
    if (byte == 0x00 || byte == 0xFF || byte == PS2_ACK || byte == PS2_RESEND) {
        s->e0 = false;
        return false;
    }

    bool ext = s->e0;
    s->e0 = false;
    bool pressed = !(byte & 0x80);
    uint8_t code = byte & 0x7F;

    uint16_t k;
    if (ext) {
        // Fake shifts around Print Screen etc.: E0 2A / E0 36 (and breaks).
        if (code == 0x2A || code == 0x36) return false;
        k = set1_e0_key(code);
    } else {
        k = code < sizeof set1_keys / sizeof set1_keys[0] ? set1_keys[code] : KEY_UNKNOWN;
    }
    if (k == KEY_UNKNOWN) return false;

    switch (k) {
    case KEY_LSHIFT: s->lshift = pressed; break;
    case KEY_RSHIFT: s->rshift = pressed; break;
    case KEY_LCTRL:  s->lctrl = pressed; break;
    case KEY_RCTRL:  s->rctrl = pressed; break;
    case KEY_LALT:   s->lalt = pressed; break;
    case KEY_RALT:   s->ralt = pressed; break;
    case KEY_LSUPER: s->lsuper = pressed; break;
    case KEY_RSUPER: s->rsuper = pressed; break;
    case KEY_CAPSLOCK:
        if (pressed && !s->caps_down) s->mods ^= MOD_CAPS;
        s->caps_down = pressed;
        break;
    default: break;
    }
    recompute_mods(s);

    bool keypad = ext ? code == 0x35 : ((code >= 0x47 && code <= 0x53) || code == 0x37);
    memset(ev, 0, sizeof *ev);
    ev->type = INPUT_KEY;
    ev->keycode = k;
    ev->pressed = pressed;
    ev->mods = s->mods;
    ev->ascii = translate(k, s->mods, keypad);
    return true;
}

bool kbd_init_device(void) {
    // Make sure the keyboard uses set 2 (translated to set 1 by the controller).
    if (ps2_dev_send(1, 0xF0)) ps2_dev_send(1, 0x02);
    return ps2_dev_send(1, 0xF4);   // enable scanning
}

static struct kbd_state kbd;

void kbd_irq_feed(uint8_t byte) {
    struct input_event ev;
    if (kbd_decode(&kbd, byte, &ev)) input_push(&ev);
}

// ---- self-test: feed known byte sequences through the real decoder ----

static const char *expect_key(struct kbd_state *s, const uint8_t *bytes, int n,
                              uint16_t key, char ascii, bool pressed, uint8_t mods,
                              const char *what) {
    struct input_event ev;
    bool got = false;
    for (int i = 0; i < n; i++) {
        bool r = kbd_decode(s, bytes[i], &ev);
        if (r && i != n - 1) return what;   // event too early
        got = r;
    }
    if (!got || ev.type != INPUT_KEY || ev.keycode != key || ev.ascii != ascii ||
        ev.pressed != pressed || ev.mods != mods)
        return what;
    return 0;
}

#define EXPECT(seq, key, ascii, pressed, mods, what) do { \
        static const uint8_t b_[] = seq; \
        const char *e_ = expect_key(&s, b_, (int)sizeof b_, K(key), ascii, pressed, mods, what); \
        if (e_) return e_; \
    } while (0)
#define SEQ(...) { __VA_ARGS__ }

const char *kbd_selftest(void) {
    struct kbd_state s;
    memset(&s, 0, sizeof s);
    EXPECT(SEQ(0x1E), 'a', 'a', true, 0, "kbd: plain a");
    EXPECT(SEQ(0x9E), 'a', 'a', false, 0, "kbd: a break");
    EXPECT(SEQ(0x2A), KEY_LSHIFT, 0, true, MOD_SHIFT, "kbd: lshift make");
    EXPECT(SEQ(0x1E), 'a', 'A', true, MOD_SHIFT, "kbd: shift+a");
    EXPECT(SEQ(0x02), '1', '!', true, MOD_SHIFT, "kbd: shift+1");
    EXPECT(SEQ(0xAA), KEY_LSHIFT, 0, false, 0, "kbd: lshift break");
    EXPECT(SEQ(0xE0, 0x48), KEY_UP, 0, true, 0, "kbd: E0 48 up");
    EXPECT(SEQ(0xE0, 0xC8), KEY_UP, 0, false, 0, "kbd: E0 C8 up break");
    EXPECT(SEQ(0xE0, 0x53), KEY_DELETE, 0, true, 0, "kbd: delete");
    EXPECT(SEQ(0xE0, 0x1D), KEY_RCTRL, 0, true, MOD_CTRL, "kbd: rctrl");
    EXPECT(SEQ(0xE0, 0x9D), KEY_RCTRL, 0, false, 0, "kbd: rctrl break");
    EXPECT(SEQ(0xE0, 0x5B), KEY_LSUPER, 0, true, MOD_SUPER, "kbd: lsuper");
    EXPECT(SEQ(0xE0, 0xDB), KEY_LSUPER, 0, false, 0, "kbd: lsuper break");
    EXPECT(SEQ(0x3A), KEY_CAPSLOCK, 0, true, MOD_CAPS, "kbd: caps on");
    EXPECT(SEQ(0x3A), KEY_CAPSLOCK, 0, true, MOD_CAPS, "kbd: caps repeat");
    EXPECT(SEQ(0xBA), KEY_CAPSLOCK, 0, false, MOD_CAPS, "kbd: caps break");
    EXPECT(SEQ(0x10), 'q', 'Q', true, MOD_CAPS, "kbd: caps+q");
    EXPECT(SEQ(0x03), '2', '2', true, MOD_CAPS, "kbd: caps+2");
    EXPECT(SEQ(0x36), KEY_RSHIFT, 0, true, MOD_CAPS | MOD_SHIFT, "kbd: rshift");
    EXPECT(SEQ(0x10), 'q', 'q', true, MOD_CAPS | MOD_SHIFT, "kbd: caps+shift+q");
    EXPECT(SEQ(0xB6), KEY_RSHIFT, 0, false, MOD_CAPS, "kbd: rshift break");
    EXPECT(SEQ(0x3A), KEY_CAPSLOCK, 0, true, 0, "kbd: caps off");
    EXPECT(SEQ(0xBA), KEY_CAPSLOCK, 0, false, 0, "kbd: caps off break");
    EXPECT(SEQ(0x1C), KEY_ENTER, '\n', true, 0, "kbd: enter");
    EXPECT(SEQ(0xE0, 0x2A, 0xE0, 0x37, 0x1E), 'a', 'a', true, 0, "kbd: prtsc skipped");
    EXPECT(SEQ(0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5, 0x58), KEY_F12, 0, true, 0,
           "kbd: pause skipped");
    return 0;
}
