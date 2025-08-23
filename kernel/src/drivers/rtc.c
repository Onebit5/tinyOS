#include "drivers/rtc.h"
#include "cpu/io.h"
#include "cpu/interrupts.h"
#include <stdbool.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_SECOND  0x00
#define RTC_MINUTE  0x02
#define RTC_HOUR    0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_STATUS_A 0x0a
#define RTC_STATUS_B 0x0b

uint8_t rtc_from_bcd(uint8_t v) {
    return (uint8_t)((v & 0x0f) + ((v >> 4) * 10));
}

void rtc_decode(struct rtc_time *t, uint8_t status_b) {
    bool is_binary = status_b & 0x04;
    bool is_24h    = status_b & 0x02;

    /* the pm flag rides in the top bit of the hour, and it has to come
     * out before any bcd conversion or it corrupts the digits */
    bool pm = !is_24h && (t->hour & 0x80);
    t->hour &= 0x7f;

    if (!is_binary) {
        t->second = rtc_from_bcd(t->second);
        t->minute = rtc_from_bcd(t->minute);
        t->hour   = rtc_from_bcd(t->hour);
        t->day    = rtc_from_bcd(t->day);
        t->month  = rtc_from_bcd(t->month);
        t->year   = rtc_from_bcd((uint8_t)t->year);
    }

    if (!is_24h) {
        if (pm && t->hour != 12) {
            t->hour = (uint8_t)(t->hour + 12);
        } else if (!pm && t->hour == 12) {
            t->hour = 0;        /* 12am is hour zero, not hour twelve */
        }
    }

    /* two digits is all the chip stores, and the century register is
     * not reliable. we assume nobody boots this in 2099 */
    t->year = (uint16_t)(t->year + 2000);
}

#ifndef TINYOS_HOSTED

static uint8_t cmos_read(uint8_t reg) {
    /* the top bit of the address port also gates NMIs. keep it clear so
     * we dont silently leave them masked */
    outb(CMOS_ADDR, reg & 0x7f);
    return inb(CMOS_DATA);
}

static bool update_in_progress(void) {
    return cmos_read(RTC_STATUS_A) & 0x80;
}

static void read_raw(struct rtc_time *t) {
    t->second = cmos_read(RTC_SECOND);
    t->minute = cmos_read(RTC_MINUTE);
    t->hour   = cmos_read(RTC_HOUR);
    t->day    = cmos_read(RTC_DAY);
    t->month  = cmos_read(RTC_MONTH);
    t->year   = cmos_read(RTC_YEAR);
}

void rtc_read(struct rtc_time *out) {
    uint64_t flags = irq_save();

    struct rtc_time a, b;

    /* the chip updates itself once a second and the registers are
     * inconsistent while it does. wait for that to pass, then read
     * twice and only believe it when two reads agree -- an update can
     * still start in the middle of ours */
    do {
        while (update_in_progress()) { }
        read_raw(&a);
        while (update_in_progress()) { }
        read_raw(&b);
    } while (a.second != b.second || a.minute != b.minute || a.hour != b.hour
             || a.day != b.day || a.month != b.month || a.year != b.year);

    rtc_decode(&a, cmos_read(RTC_STATUS_B));

    *out = a;
    irq_restore(flags);
}

#endif /* TINYOS_HOSTED */
