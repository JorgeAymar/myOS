// Platform services for the TLS stack: random bytes and wall-clock time.
//
// ENTROPY.  Preferred source is the CPU: RDSEED (CPUID.7:EBX bit 18), else RDRAND
// (CPUID.1:ECX bit 30). QEMU's default CPU model (qemu64) advertises neither, so
// under the stock test/run targets the FALLBACK is what actually runs: CPU timing
// jitter (RDTSC deltas around short busy loops), the PIT millisecond tick counter
// and the RTC, compressed by SHA-256 into a seed for a BearSSL HMAC-DRBG.
// HONEST CAVEAT: the fallback is weak. Under emulation the TSC is derived from the
// host clock, and a hobby kernel has no hardware noise source, so an attacker who
// knows roughly when we booted could shrink the search space considerably. Run QEMU
// with `-cpu max` (or on real hardware with RDRAND) for proper seeding. Every call
// also folds in fresh RDTSC/tick values, but that does not add real entropy.
//
// TIME.  tls_unix_time() converts the CMOS RTC to Unix seconds. This repo runs QEMU
// with `-rtc base=localtime`, so the RTC reads host LOCAL time and the result is
// skewed by the host's UTC offset (up to +-14h). The RTC itself carries no zone;
// that is acceptable for certificate validity windows (days to years), not for
// anything finer.
#include <stdint.h>
#include <stddef.h>
#include "tls.h"
#include "rtc.h"
#include "arch.h"
#include "bearssl.h"

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                     : "a"(leaf), "c"(sub));
}

static int hw_rand64(uint64_t *out, int seed_insn) {
    for (int i = 0; i < 16; i++) {
        unsigned char ok;
        uint64_t v;
        if (seed_insn) __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
        else           __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
        if (ok) { *out = v; return 1; }
        __asm__ volatile("pause");
    }
    return 0;
}

// 0 = none, 1 = RDRAND, 2 = RDSEED
static int hw_kind(void) {
    uint32_t r[4];
    cpuid(0, 0, r);
    uint32_t maxleaf = r[0];
    if (maxleaf >= 7) { cpuid(7, 0, r); if (r[1] & (1u << 18)) return 2; }
    cpuid(1, 0, r);
    if (r[2] & (1u << 30)) return 1;
    return 0;
}

static br_hmac_drbg_context drbg;
static int drbg_ready;
static volatile int lock;
static int source_kind;   // 0 jitter fallback, 1 RDRAND, 2 RDSEED

static void absorb_jitter(br_sha256_context *h) {
    for (int round = 0; round < 512; round++) {
        uint64_t t0 = rdtsc();
        volatile uint32_t spin = 0;
        for (int i = 0; i < 37 + (round & 15); i++) spin = spin + (uint32_t)i * 2654435761u;
        (void)spin;
        uint64_t t1 = rdtsc();
        uint64_t d[2] = { t1 - t0, t1 };
        br_sha256_update(h, d, sizeof d);
    }
}

static void gather_seed(uint8_t seed[32]) {
    br_sha256_context h;
    br_sha256_init(&h);
    int kind = hw_kind();
    source_kind = 0;
    if (kind) {
        for (int i = 0; i < 8; i++) {
            uint64_t v;
            if (!hw_rand64(&v, kind == 2)) break;
            br_sha256_update(&h, &v, sizeof v);
            source_kind = kind;
        }
    }
    // Always mix the weak sources too (harmless when hardware RNG is present).
    absorb_jitter(&h);
    uint64_t ms = timer_ticks_ms();
    struct rtc_time t;
    rtc_read(&t);
    br_sha256_update(&h, &ms, sizeof ms);
    br_sha256_update(&h, &t, sizeof t);
    br_sha256_out(&h, seed);
}

void tls_entropy(void *buf, size_t n) {
    while (__sync_lock_test_and_set(&lock, 1)) __asm__ volatile("pause");
    if (!drbg_ready) {
        uint8_t seed[32];
        gather_seed(seed);
        br_hmac_drbg_init(&drbg, &br_sha256_vtable, seed, sizeof seed);
        drbg_ready = 1;
    } else {
        uint64_t extra[2] = { rdtsc(), timer_ticks_ms() };
        br_hmac_drbg_update(&drbg, extra, sizeof extra);
    }
    br_hmac_drbg_generate(&drbg, buf, n);
    __sync_lock_release(&lock);
}

const char *tls_entropy_source(void) {
    return source_kind == 2 ? "RDSEED" : source_kind == 1 ? "RDRAND" : "jitter-fallback(weak)";
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

uint64_t tls_unix_time(void) {
    struct rtc_time t;
    rtc_read(&t);
    int64_t days = days_from_civil(t.year, t.month, t.day);
    return (uint64_t)(days * 86400 + t.hour * 3600 + t.minute * 60 + t.second);
}
