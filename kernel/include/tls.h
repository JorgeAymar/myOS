#pragma once
// HTTPS client (TLS 1.2 via BearSSL). Phase 13.
#include <stdint.h>
#include <stddef.h>

// Same contract as http_get_timeout(): GET https://host/path, returns the raw HTTP
// response (status line + headers + body) copied into buf, or a negative NET_E* code.
// Blocking; call it from the net job thread, never from the GUI thread.
int https_get_timeout(const char *host, const char *path, char *buf, size_t len, uint32_t timeout_ms);

// Platform services the TLS stack needs (kernel/lib/entropy.c).
void tls_entropy(void *buf, size_t n);     // cryptographic-quality-as-possible random bytes
uint64_t tls_unix_time(void);              // seconds since 1970-01-01 UTC, from the RTC
