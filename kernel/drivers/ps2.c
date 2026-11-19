// PS/2 controller (i8042) on ports 0x60/0x64. Every wait is bounded so a
// missing or misbehaving controller can never hang the boot.
#include "ps2.h"
#include "io.h"

#define PS2_TIMEOUT 100000u   // status polls per wait

bool ps2_wait_write(void) {
    for (uint32_t i = 0; i < PS2_TIMEOUT; i++)
        if (!(inb(PS2_STATUS) & PS2_ST_IN_FULL)) return true;
    return false;
}

bool ps2_wait_read(void) {
    for (uint32_t i = 0; i < PS2_TIMEOUT; i++)
        if (inb(PS2_STATUS) & PS2_ST_OUT_FULL) return true;
    return false;
}

void ps2_flush(void) {
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & PS2_ST_OUT_FULL); i++)
        (void)inb(PS2_DATA);
}

bool ps2_cmd(uint8_t cmd) {
    if (!ps2_wait_write()) return false;
    outb(PS2_CMD, cmd);
    return true;
}

bool ps2_cmd_arg(uint8_t cmd, uint8_t arg) {
    if (!ps2_cmd(cmd)) return false;
    if (!ps2_wait_write()) return false;
    outb(PS2_DATA, arg);
    return true;
}

bool ps2_read(uint8_t *out) {
    if (!ps2_wait_read()) return false;
    *out = inb(PS2_DATA);
    return true;
}

static bool ps2_read_config(uint8_t *cfg) {
    return ps2_cmd(0x20) && ps2_read(cfg);
}

static bool ps2_write_config(uint8_t cfg) {
    return ps2_cmd_arg(0x60, cfg);
}

bool ps2_dev_send(int port, uint8_t byte) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (port == 2) {
            if (!ps2_cmd(0xD4)) return false;
        }
        if (!ps2_wait_write()) return false;
        outb(PS2_DATA, byte);
        // Skip unrelated bytes (e.g. stray scancodes) until ACK/RESEND.
        for (int n = 0; n < 8; n++) {
            uint8_t r;
            if (!ps2_read(&r)) return false;
            if (r == PS2_ACK) return true;
            if (r == PS2_RESEND) break;
        }
    }
    return false;
}

void ps2_controller_init(struct ps2_info *info) {
    *info = (struct ps2_info){0};

    // A floating bus reads 0xFF: no controller.
    if (inb(PS2_STATUS) == 0xFF) return;

    // 1. Disable both ports so nothing new arrives during setup.
    if (!ps2_cmd(0xAD)) return;
    ps2_cmd(0xA7);
    // 2. Flush whatever firmware left in the output buffer.
    ps2_flush();

    // 3. Config byte: IRQs off for now, translation ON (set 1 delivery).
    uint8_t cfg;
    if (!ps2_read_config(&cfg)) return;
    info->controller = true;
    bool maybe_dual = (cfg & 0x20) != 0;   // port 2 clock disabled after 0xA7
    cfg &= (uint8_t)~(0x01 | 0x02);
    cfg |= 0x40;
    ps2_write_config(cfg);

    // 4. Detect the aux port: enabling it must clear the port-2 clock bit.
    if (maybe_dual) {
        ps2_cmd(0xA8);
        uint8_t c2;
        if (ps2_read_config(&c2) && !(c2 & 0x20)) info->dual = true;
        ps2_cmd(0xA7);
    }

    // 5. Interface tests (0xAB / 0xA9 return 0x00 when OK). Not the 0xAA
    //    controller self-test, which can reset the config byte.
    uint8_t r;
    bool port1_ok = ps2_cmd(0xAB) && ps2_read(&r) && r == 0x00;
    if (info->dual) {
        if (!(ps2_cmd(0xA9) && ps2_read(&r) && r == 0x00)) info->dual = false;
    }
    ps2_flush();

    // 6. Enable ports and bring up devices.
    if (port1_ok) {
        ps2_cmd(0xAE);
        info->kbd = kbd_init_device();
    }
    if (info->dual) {
        ps2_cmd(0xA8);
        info->mouse = mouse_init_device(&info->wheel);
    }
    ps2_flush();

    // 7. Final config: enable IRQs of present devices, clocks on.
    if (ps2_read_config(&cfg)) {
        cfg |= 0x40;
        if (info->kbd)   { cfg |= 0x01; cfg &= (uint8_t)~0x10; }
        if (info->mouse) { cfg |= 0x02; cfg &= (uint8_t)~0x20; }
        ps2_write_config(cfg);
    }
    ps2_flush();
}
