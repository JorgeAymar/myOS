#pragma once
// Private interface of the PS/2 drivers (controller, keyboard, mouse decoders).
// Not part of the public kernel API: only kernel/drivers/*.c include this.
#include <stdint.h>
#include <stdbool.h>
#include "input.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64   // read
#define PS2_CMD    0x64   // write

#define PS2_ST_OUT_FULL 0x01
#define PS2_ST_IN_FULL  0x02
#define PS2_ST_AUX_DATA 0x20

#define PS2_ACK    0xFA
#define PS2_RESEND 0xFE

// ---- controller (ps2.c) ----
struct ps2_info {
    bool controller;   // something answers on 0x64
    bool dual;         // second (aux) port exists
    bool kbd;          // keyboard acknowledged enable-scanning
    bool mouse;        // mouse acknowledged enable-reporting
    bool wheel;        // IntelliMouse (ID 3, 4-byte packets)
};

bool ps2_wait_write(void);                  // input buffer empty (bounded wait)
bool ps2_wait_read(void);                   // output buffer full (bounded wait)
void ps2_flush(void);
bool ps2_cmd(uint8_t cmd);                  // controller command
bool ps2_cmd_arg(uint8_t cmd, uint8_t arg); // controller command + data byte
bool ps2_read(uint8_t *out);                // bounded read of 0x60
// Sends a byte to a device (port 1 or 2) and waits for ACK, retrying on RESEND.
bool ps2_dev_send(int port, uint8_t byte);
// Initializes the controller and devices; fills *info. Never hangs.
void ps2_controller_init(struct ps2_info *info);

// ---- keyboard decoder (keyboard.c) ----
struct kbd_state {
    bool e0;            // last byte was 0xE0
    uint8_t e1_skip;    // remaining bytes of an 0xE1 (Pause) sequence
    uint8_t mods;       // MOD_*
    bool lshift, rshift, lctrl, rctrl, lalt, ralt, lsuper, rsuper;
    bool caps_down;     // caps key held (ignore typematic repeat)
};
// Feeds one scancode-set-1 byte. Returns true and fills *ev when a key event
// is complete.
bool kbd_decode(struct kbd_state *s, uint8_t byte, struct input_event *ev);
bool kbd_init_device(void);
void kbd_irq_feed(uint8_t byte);

// ---- mouse decoder (mouse.c) ----
struct mouse_state {
    uint8_t packet_len;   // 3 or 4
    uint8_t idx;
    uint8_t buf[4];
};
void mouse_state_init(struct mouse_state *s, bool wheel);
// Feeds one aux byte. Returns true and fills *ev when a valid packet is complete.
bool mouse_decode(struct mouse_state *s, uint8_t byte, struct input_event *ev);
// Enables the aux device; returns true if present, sets *wheel.
bool mouse_init_device(bool *wheel);
void mouse_irq_feed(uint8_t byte);
void mouse_set_wheel(bool wheel);

// ---- shared IRQ path (input.c) ----
// Reads one pending byte from the controller and routes it by the AUX bit.
void ps2_irq_dispatch(void);
// Queue push used by IRQ handlers (drops on overflow).
void input_push(const struct input_event *ev);

// Self-tests of the decoders (keyboard.c / mouse.c). Return NULL on success
// or a description of the first failure.
const char *kbd_selftest(void);
const char *mouse_selftest(void);
