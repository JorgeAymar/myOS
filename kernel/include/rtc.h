#pragma once
// CMOS real-time clock. Owner: drivers agent (kernel/drivers/rtc.c).
#include <stdint.h>

struct rtc_time {
    uint16_t year;   // e.g. 2026
    uint8_t month;   // 1-12
    uint8_t day;     // 1-31
    uint8_t hour;    // 0-23
    uint8_t minute;
    uint8_t second;
    uint8_t weekday; // 0 = Sunday
};

void rtc_read(struct rtc_time *t);
