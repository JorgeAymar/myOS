// Boot self-check for the image decoder (stb_image + integer downscaler).
#include "img.h"
#include "kprintf.h"
#include "mm.h"
#include "panic.h"
#include "string.h"
#include "subsystems.h"
#include "img_testdata.h"

static int cases;
#define CHECK(c) do { cases++; if (!(c)) panic("img selftest: %s (line %d)", #c, __LINE__); } while (0)

static int chan_near(uint32_t p, uint32_t want, int tol) {
    for (int sh = 0; sh < 32; sh += 8) {
        int d = (int)((p >> sh) & 0xff) - (int)((want >> sh) & 0xff);
        if (d < -tol || d > tol) return 0;
    }
    return 1;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

// Minimal PNG header claiming w x h (stb only reads IHDR for info).
static size_t fake_png(uint8_t *b, uint32_t w, uint32_t h) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    memcpy(b, sig, 8);
    put_be32(b + 8, 13);
    memcpy(b + 12, "IHDR", 4);
    put_be32(b + 16, w);
    put_be32(b + 20, h);
    b[24] = 8; b[25] = 6; b[26] = b[27] = b[28] = 0;
    put_be32(b + 29, 0);
    return 33;
}

static void run_cases(void) {
    struct img im, sc;

    // PNG with alpha, exact.
    CHECK(img_decode(png_grad16, sizeof png_grad16, 0, 0, &im));
    CHECK(im.w == 16 && im.h == 16);
    CHECK(im.px[5 * 16 + 3] == 0xff305080u);
    CHECK(im.px[2 * 16 + 12] == 0x80c02080u);
    CHECK(im.px[0] == 0xff000080u);

    // Area-average 16x16 -> 8x8.
    CHECK(img_scale(&im, 8, 8, &sc));
    CHECK(sc.w == 8 && sc.h == 8);
    CHECK(sc.px[0] == 0xff080880u);
    CHECK(sc.px[1 * 8 + 1] == 0xff282880u);   // x 2..3 -> 40, y 2..3 -> 40
    CHECK(ALPHA_OF(sc.px[7]) == 0x80);        // right half has alpha 0x80
    img_free(&sc);
    CHECK(sc.px == NULL);
    // Upscale request is rejected.
    CHECK(!img_scale(&im, 32, 32, &sc));
    img_free(&im);

    // Size limits at decode time (aspect preserved, never enlarged).
    CHECK(img_decode(png_grad16, sizeof png_grad16, 8, 100, &im) && im.w == 8 && im.h == 8);
    img_free(&im);
    CHECK(img_decode(png_grad16, sizeof png_grad16, 100, 4, &im) && im.w == 4 && im.h == 4);
    img_free(&im);
    CHECK(img_decode(png_grad16, sizeof png_grad16, 100, 100, &im) && im.w == 16 && im.h == 16);
    img_free(&im);
    CHECK(img_decode(png_grad16, sizeof png_grad16, 1, 1, &im) && im.w == 1 && im.h == 1);
    img_free(&im);

    // JPEG (lossy: tolerance).
    CHECK(img_decode(jpg_q32, sizeof jpg_q32, 0, 0, &im));
    CHECK(im.w == 32 && im.h == 32);
    CHECK(chan_near(im.px[8 * 32 + 8], 0xffff0000u, 40));
    CHECK(chan_near(im.px[8 * 32 + 24], 0xff00ff00u, 40));
    CHECK(chan_near(im.px[24 * 32 + 8], 0xff0000ffu, 40));
    CHECK(chan_near(im.px[24 * 32 + 24], 0xffffffffu, 40));
    img_free(&im);

    // GIF first frame and BMP.
    CHECK(img_decode(gif_q8, sizeof gif_q8, 0, 0, &im));
    CHECK(im.w == 8 && im.h == 8);
    CHECK(chan_near(im.px[1 * 8 + 1], 0xffff0000u, 8));
    CHECK(chan_near(im.px[6 * 8 + 6], 0xffffffffu, 8));
    img_free(&im);
    CHECK(img_decode(bmp_q8, sizeof bmp_q8, 0, 0, &im));
    CHECK(im.w == 8 && im.h == 8);
    CHECK(im.px[1 * 8 + 6] == 0xff00ff00u);
    CHECK(im.px[6 * 8 + 1] == 0xff0000ffu);
    img_free(&im);

    // Truncated / corrupt inputs must fail cleanly.
    CHECK(!img_decode(png_grad16, sizeof png_grad16 / 2, 0, 0, &im) && im.px == NULL);
    CHECK(!img_decode(png_grad16, 20, 0, 0, &im));
    CHECK(!img_decode(png_grad16, 4, 0, 0, &im));
    CHECK(!img_decode(NULL, 100, 0, 0, &im));
    uint8_t bad[sizeof png_grad16];
    memcpy(bad, png_grad16, sizeof bad);
    for (size_t i = 40; i < sizeof bad - 12; i += 3) bad[i] ^= 0xa5;   // corrupt IDAT
    CHECK(!img_decode(bad, sizeof bad, 0, 0, &im));
    uint8_t junk[64];
    for (int i = 0; i < 64; i++) junk[i] = (uint8_t)(i * 37 + 11);
    CHECK(!img_decode(junk, sizeof junk, 0, 0, &im));
    CHECK(!img_decode(jpg_q32, 2, 0, 0, &im));

    // Hostile headers: absurd and just-over-limit dimensions are rejected before decoding.
    uint8_t hdr[33];
    size_t n = fake_png(hdr, 60000, 60000);
    CHECK(!img_decode(hdr, n, 0, 0, &im));
    n = fake_png(hdr, IMG_MAX_SIDE + 1, 16);
    CHECK(!img_decode(hdr, n, 0, 0, &im));
    n = fake_png(hdr, IMG_MAX_SIDE, IMG_MAX_SIDE);   // 16 MP > IMG_MAX_PIXELS
    CHECK(!img_decode(hdr, n, 0, 0, &im));

    // Drawing: opaque, blended and clipped.
    uint32_t canvas[20 * 20];
    struct surface s;
    gfx_surface_init(&s, canvas, 20, 20, 20);
    gfx_fill(&s, rect_make(0, 0, 20, 20), RGB(0, 0, 0));
    CHECK(img_decode(png_grad16, sizeof png_grad16, 0, 0, &im));
    img_draw(&s, &im, 10, 10);                   // partly outside
    CHECK(canvas[10 * 20 + 10] == 0xff000080u);
    CHECK(canvas[15 * 20 + 13] == 0xff305080u);
    img_draw(&s, &im, -100, -100);               // fully outside: no crash
    uint32_t half = canvas[10 * 20 + 19];        // image x=9 (alpha 0x80, r=144) over black
    CHECK(((half >> 16) & 0xff) > 60 && ((half >> 16) & 0xff) < 80);
    img_free(&im);
}

void img_selftest(void) {
    cases = 0;
    run_cases();               // warm-up (heap arena may grow once)
    int total = cases;
    uint64_t before = pmm_free_bytes();
    run_cases();
    uint64_t after = pmm_free_bytes();
    if (after + 64 * 1024 < before)
        panic("img selftest: leak, free %lu -> %lu", before, after);
    kprintf("[OK] img decode (%d cases)\n", total);
}
