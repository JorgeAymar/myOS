#pragma once
// Keyboard + mouse input. Owner: drivers agent (kernel/drivers/ps2*.c, input.c).
#include <stdint.h>
#include <stdbool.h>

enum input_event_type { INPUT_NONE = 0, INPUT_KEY, INPUT_MOUSE };

// Keycodes for non-printable keys. Printable keys use their lowercase ASCII
// value as keycode ('a', '1', ' ', ...).
enum keycode {
    KEY_UNKNOWN = 0,
    KEY_BACKSPACE = 0x08, KEY_TAB = 0x09, KEY_ENTER = 0x0a, KEY_ESC = 0x1b,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INSERT, KEY_DELETE,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT,
    KEY_LSUPER, KEY_RSUPER, KEY_CAPSLOCK,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
};

#define MOD_SHIFT (1u << 0)
#define MOD_CTRL  (1u << 1)
#define MOD_ALT   (1u << 2)
#define MOD_SUPER (1u << 3)
#define MOD_CAPS  (1u << 4)

#define MOUSE_LEFT   (1u << 0)
#define MOUSE_RIGHT  (1u << 1)
#define MOUSE_MIDDLE (1u << 2)

struct input_event {
    uint8_t type;           // enum input_event_type
    // INPUT_KEY
    uint16_t keycode;       // enum keycode or lowercase ASCII
    char ascii;             // translated char honoring shift/caps (0 if none)
    bool pressed;           // true = make, false = break
    uint8_t mods;           // MOD_* at the time of the event
    // INPUT_MOUSE (relative motion, screen coordinates: +dy is DOWN)
    int16_t dx, dy;
    int8_t wheel;           // +1 scroll up, -1 down
    uint8_t buttons;        // MOUSE_* currently held
};

// Initializes the PS/2 controller, keyboard and mouse, and registers IRQ1/IRQ12
// via irq_register(). Events are queued from the IRQ handlers.
void input_init(void);
// Pops the next queued event. Returns false when the queue is empty.
bool input_poll(struct input_event *ev);
// Test hook: injects a synthetic event into the queue (used by self-tests).
void input_inject(const struct input_event *ev);
