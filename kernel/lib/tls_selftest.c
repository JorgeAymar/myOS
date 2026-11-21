// Boot self-check for the BearSSL port: hash, AEAD, entropy, time.
#include <stdint.h>
#include <stddef.h>
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "tls.h"
#include "subsystems.h"
#include "bearssl.h"

const char *tls_entropy_source(void);

static void hex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t v = 0;
        for (int k = 0; k < 2; k++) {
            char c = *s++;
            v = (uint8_t)(v << 4 | (c <= '9' ? c - '0' : (c | 32) - 'a' + 10));
        }
        out[i] = v;
    }
}

void tls_selftest(void) {
    // SHA-256("abc") (FIPS 180-2)
    uint8_t h[32], want[32];
    br_sha256_context sc;
    br_sha256_init(&sc);
    br_sha256_update(&sc, "abc", 3);
    br_sha256_out(&sc, h);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    if (memcmp(h, want, 32) != 0) panic("bearssl: SHA-256 known-answer mismatch");

    // AES-128-GCM, McGrew-Viega Test Case 4 (with AAD)
    uint8_t key[16], iv[12], pt[60], aad[20], tag[16], buf[60], wct[60], wtag[16];
    hex("feffe9928665731c6d6a8f9467308308", key, 16);
    hex("cafebabefacedbaddecaf888", iv, 12);
    hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", pt, 60);
    hex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
    hex("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091", wct, 60);
    hex("5bc94fbc3221a5db94fae95ae7121a47", wtag, 16);
    br_aes_ct_ctr_keys bc;
    br_gcm_context gc;
    br_aes_ct_ctr_init(&bc, key, 16);
    br_gcm_init(&gc, &bc.vtable, br_ghash_ctmul);
    br_gcm_reset(&gc, iv, 12);
    br_gcm_aad_inject(&gc, aad, 20);
    br_gcm_flip(&gc);
    memcpy(buf, pt, 60);
    br_gcm_run(&gc, 1, buf, 60);
    br_gcm_get_tag(&gc, tag);
    if (memcmp(buf, wct, 60) != 0 || memcmp(tag, wtag, 16) != 0)
        panic("bearssl: AES-128-GCM encrypt known-answer mismatch");
    br_gcm_reset(&gc, iv, 12);
    br_gcm_aad_inject(&gc, aad, 20);
    br_gcm_flip(&gc);
    br_gcm_run(&gc, 0, buf, 60);
    if (!br_gcm_check_tag(&gc, wtag) || memcmp(buf, pt, 60) != 0)
        panic("bearssl: AES-128-GCM decrypt/tag check failed");

    // Entropy must not repeat
    uint8_t e1[32], e2[32];
    tls_entropy(e1, 32);
    tls_entropy(e2, 32);
    if (memcmp(e1, e2, 32) == 0) panic("bearssl: tls_entropy returned identical output");
    int allsame = 1;
    for (int i = 1; i < 32; i++) if (e1[i] != e1[0]) allsame = 0;
    if (allsame) panic("bearssl: tls_entropy output is constant");

    uint64_t now = tls_unix_time();
    if (now < 1767225600ull) panic("bearssl: tls_unix_time before 2026");   // 2026-01-01

    kprintf("[OK] bearssl: sha256+aes128-gcm KAT, entropy=%s, unix_time=%lu\n",
            tls_entropy_source(), (unsigned long)now);
}
