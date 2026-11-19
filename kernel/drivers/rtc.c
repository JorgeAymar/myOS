// CMOS real-time clock (ports 0x70/0x71).
#include <stdbool.h>
#include "rtc.h"
#include "io.h"
#include "string.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static void wait_not_updating(void) {
    // Update-in-progress lasts < 2 ms; bounded so a dead CMOS can't hang us.
    for (uint32_t i = 0; i < 1000000u && (cmos_read(0x0A) & 0x80); i++) { }
}

struct raw_rtc { uint8_t sec, min, hour, day, mon, year, cent; };

static void read_raw(struct raw_rtc *r) {
    wait_not_updating();
    r->sec  = cmos_read(0x00);
    r->min  = cmos_read(0x02);
    r->hour = cmos_read(0x04);
    r->day  = cmos_read(0x07);
    r->mon  = cmos_read(0x08);
    r->year = cmos_read(0x09);
    r->cent = cmos_read(0x32);
}

static uint8_t bcd(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }

// Sakamoto's algorithm, 0 = Sunday.
static uint8_t weekday(int y, int m, int d) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

void rtc_read(struct rtc_time *t) {
    struct raw_rtc a, b;
    read_raw(&b);
    for (int tries = 0; tries < 10; tries++) {
        a = b;
        read_raw(&b);
        if (memcmp(&a, &b, sizeof a) == 0) break;
    }

    uint8_t regb = cmos_read(0x0B);
    bool binary = (regb & 0x04) != 0;
    bool h24 = (regb & 0x02) != 0;

    bool pm = (b.hour & 0x80) != 0;
    uint8_t hour = b.hour & 0x7F;
    uint8_t sec = b.sec, min = b.min, day = b.day, mon = b.mon, year = b.year, cent = b.cent;
    if (!binary) {
        sec = bcd(sec); min = bcd(min); hour = bcd(hour);
        day = bcd(day); mon = bcd(mon); year = bcd(year); cent = bcd(cent);
    }
    if (!h24) {
        if (hour == 12) hour = 0;
        if (pm) hour = (uint8_t)(hour + 12);
    }
    if (cent < 19 || cent > 21) cent = 20;   // implausible century register

    t->year = (uint16_t)(cent * 100 + year);
    t->month = mon;
    t->day = day;
    t->hour = hour;
    t->minute = min;
    t->second = sec;
    t->weekday = (mon >= 1 && mon <= 12) ? weekday(t->year, mon, day) : 0;
}
