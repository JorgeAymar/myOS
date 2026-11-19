// Input event queue + PS/2 bring-up and boot self-tests.
#include "ps2.h"
#include "input.h"
#include "rtc.h"
#include "arch.h"
#include "io.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"

#define QUEUE_SIZE 256u   // power of two

static struct input_event queue[QUEUE_SIZE];
static volatile uint32_t q_head;   // next write (free-running)
static volatile uint32_t q_tail;   // next read  (free-running)

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}

static inline void irq_restore(uint64_t f) {
    __asm__ volatile ("push %0; popfq" : : "r"(f) : "memory", "cc");
}

void input_push(const struct input_event *ev) {
    uint64_t f = irq_save();
    if (q_head - q_tail < QUEUE_SIZE) {    // drop on overflow
        queue[q_head % QUEUE_SIZE] = *ev;
        q_head++;
    }
    irq_restore(f);
}

void input_inject(const struct input_event *ev) { input_push(ev); }

bool input_poll(struct input_event *ev) {
    uint64_t f = irq_save();
    bool got = q_head != q_tail;
    if (got) {
        *ev = queue[q_tail % QUEUE_SIZE];
        q_tail++;
    }
    irq_restore(f);
    return got;
}

// ---- IRQ handlers ----
// Both IRQ1 and IRQ12 read one byte and route it by the status AUX bit, so a
// byte is never fed to the wrong decoder even if IRQs race.
void ps2_irq_dispatch(void) {
    uint8_t st = inb(PS2_STATUS);
    if (!(st & PS2_ST_OUT_FULL)) return;
    uint8_t b = inb(PS2_DATA);
    if (st & PS2_ST_AUX_DATA) mouse_irq_feed(b);
    else kbd_irq_feed(b);
}

static void kbd_irq(void)   { ps2_irq_dispatch(); }
static void mouse_irq(void) { ps2_irq_dispatch(); }

// ---- self-tests ----

static bool ev_equal(const struct input_event *a, const struct input_event *b) {
    return a->type == b->type && a->keycode == b->keycode && a->ascii == b->ascii &&
           a->pressed == b->pressed && a->mods == b->mods && a->dx == b->dx &&
           a->dy == b->dy && a->wheel == b->wheel && a->buttons == b->buttons;
}

static void queue_selftest(void) {
    uint64_t f = irq_save();   // keep real IRQs out of the test
    struct input_event ev, k, m;
    while (input_poll(&ev)) { }  // discard anything already queued

    memset(&k, 0, sizeof k);
    k.type = INPUT_KEY; k.keycode = 'a'; k.ascii = 'A'; k.pressed = true; k.mods = MOD_SHIFT;
    memset(&m, 0, sizeof m);
    m.type = INPUT_MOUSE; m.dx = -7; m.dy = 12; m.wheel = 1; m.buttons = MOUSE_LEFT | MOUSE_RIGHT;

    input_inject(&k);
    input_inject(&m);
    if (!input_poll(&ev) || !ev_equal(&ev, &k)) panic("input: key roundtrip mismatch");
    if (!input_poll(&ev) || !ev_equal(&ev, &m)) panic("input: mouse roundtrip mismatch");
    if (input_poll(&ev)) panic("input: queue not empty after roundtrip");

    // Overflow drops the newest events and keeps the oldest intact.
    for (uint32_t i = 0; i < QUEUE_SIZE + 10; i++) {
        k.keycode = (uint16_t)i;
        input_inject(&k);
    }
    uint32_t n = 0;
    while (input_poll(&ev)) {
        if (ev.keycode != n) panic("input: overflow order wrong at %u", n);
        n++;
    }
    if (n != QUEUE_SIZE) panic("input: overflow kept %u events", n);
    irq_restore(f);
}

static void rtc_selftest(void) {
    struct rtc_time t;
    rtc_read(&t);
    if (t.year < 2024 || t.year > 2199 || t.month < 1 || t.month > 12 ||
        t.day < 1 || t.day > 31 || t.hour > 23 || t.minute > 59 || t.second > 59 ||
        t.weekday > 6)
        panic("rtc: implausible time %u-%u-%u %u:%u:%u", (unsigned)t.year,
              (unsigned)t.month, (unsigned)t.day, (unsigned)t.hour,
              (unsigned)t.minute, (unsigned)t.second);
    kprintf("[OK] rtc %04u-%02u-%02u %02u:%02u:%02u\n", (unsigned)t.year,
            (unsigned)t.month, (unsigned)t.day, (unsigned)t.hour,
            (unsigned)t.minute, (unsigned)t.second);
}

void input_init(void) {
    const char *err = kbd_selftest();
    if (!err) err = mouse_selftest();
    if (err) panic("input decoder self-test failed: %s", err);

    struct ps2_info info;
    uint64_t f = irq_save();
    ps2_controller_init(&info);
    if (info.kbd)   irq_register(1, kbd_irq);
    if (info.mouse) irq_register(12, mouse_irq);
    irq_restore(f);

    queue_selftest();
    kprintf("[OK] input ps2 kbd=%s mouse=%s wheel=%s\n", info.kbd ? "yes" : "no",
            info.mouse ? "yes" : "no", info.wheel ? "yes" : "no");
    rtc_selftest();
}
