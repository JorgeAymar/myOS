// Procedural myOS wallpaper: a sky gradient with a soft glow, overlaid by
// layered flowing waves (blue -> purple -> pink). Each wave has an
// anti-aliased crest, a light sheen just below it and a soft shadow cast on
// the layer behind. Computed once at init in 8.8 fixed point with ordered
// dithering; integer math only.
#include "wm.h"

struct wave {
    int base;        // crest baseline, 1/1000 of the height
    int a1, l1, p1;  // amplitude (1/1000 H), wavelength (1/1000 W), phase (1/1024 turn)
    int a2, l2, p2;
    color_t crest, deep;
};

static const struct wave waves[WP_WAVE_COUNT] = {
    { 330, 55, 1150, 100, 20, 520, 700, RGB(0x6c, 0x86, 0xf4), RGB(0x41, 0x4c, 0xcc) },
    { 450, 60, 1350, 600, 22, 610, 140, RGB(0x95, 0x76, 0xf0), RGB(0x5c, 0x40, 0xc4) },
    { 575, 52, 1100, 300, 25, 470, 900, RGB(0xc2, 0x78, 0xe6), RGB(0x82, 0x3e, 0xb8) },
    { 700, 48, 1500, 800, 18, 560, 400, RGB(0xee, 0x8c, 0xcc), RGB(0xb6, 0x4a, 0xaa) },
    { 830, 42, 1250, 50,  16, 430, 600, RGB(0xff, 0xb4, 0xcc), RGB(0xe2, 0x66, 0x9c) },
};

#define MAXW 2048
static int crest16[WP_WAVE_COUNT][MAXW];   // crest y per column, 1/16 px
static int acc[3][MAXW];                    // row accumulator, 8.8
#define MAXDEPTH 1024
static color_t shade[WP_WAVE_COUNT][MAXDEPTH];   // layer color by depth below crest

static const uint8_t bayer4[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

static inline int ch(color_t c, int k) { return (int)((c >> (16 - 8 * k)) & 0xff); }

static inline void mix_into(int x, color_t c, int w /*0..256*/) {
    for (int k = 0; k < 3; k++) acc[k][x] += ((ch(c, k) << 8) - acc[k][x]) * w >> 8;
}

void wallpaper_render(struct surface *s) {
    int W = s->w < MAXW ? s->w : MAXW, H = s->h;
    for (int i = 0; i < WP_WAVE_COUNT; i++) {
        const struct wave *v = &waves[i];
        long l1 = (long)v->l1 * W / 1000, l2 = (long)v->l2 * W / 1000;
        for (int x = 0; x < W; x++) {
            long y = (long)v->base * H * 16 / 1000;
            y += (long)v->a1 * H * 16 / 1000 * gfx_sin1024((int)(x * 1024 / l1) + v->p1) / 16384;
            y += (long)v->a2 * H * 16 / 1000 * gfx_sin1024((int)(x * 1024 / l2) + v->p2) / 16384;
            crest16[i][x] = (int)y;
        }
    }
    int depth = H * 30 / 100;            // gradient depth below a crest
    int shadow = 22 * 16;                // shadow reach above a crest (1/16 px)
    if (depth >= MAXDEPTH) depth = MAXDEPTH - 1;
    for (int i = 0; i < WP_WAVE_COUNT; i++)
        for (int t = 0; t <= depth; t++) {
            color_t c = gfx_lerp_color(waves[i].crest, waves[i].deep, t, depth);
            if (t < 18) c = gfx_lerp_color(c, RGB(0xff, 0xff, 0xff), 18 - t, 18 * 5);
            shade[i][t] = c;
        }
    long gx = W * 78 / 100, gy = H * 18 / 100, gr = (long)W * 45 / 100;

    for (int y = 0; y < H; y++) {
        color_t sky = gfx_lerp_color(WP_SKY_TOP, WP_SKY_BOTTOM, y, H - 1);
        for (int x = 0; x < W; x++)
            for (int k = 0; k < 3; k++) acc[k][x] = ch(sky, k) << 8;
        // soft glow in the upper right
        long dy = y - gy;
        if (dy * dy < gr * gr) {
            for (int x = 0; x < W; x++) {
                long dx = x - gx, d2 = (dx * dx + dy * dy) * 256 / (gr * gr);
                if (d2 >= 256) continue;
                long f = 256 - d2;
                mix_into(x, WP_GLOW, (int)(f * f >> 8) * 110 >> 8);
            }
        }
        long py = 16L * y + 8;
        for (int i = 0; i < WP_WAVE_COUNT; i++) {
            for (int x = 0; x < W; x++) {
                long t = py - crest16[i][x];          // distance below the crest, 1/16 px
                if (t < -shadow) continue;
                if (t < 0) {
                    // soft shadow cast upward onto what is behind
                    long f = shadow + t;              // 0..shadow
                    int w = (int)(f * f / shadow * 48 / shadow);
                    mix_into(x, RGB(0x20, 0x10, 0x50), w);
                    if (t < -16) continue;
                }
                int cov = t >= 16 ? 256 : (int)(t + 16) * 8;     // AA crest over 2 px
                long tt = t / 16;
                mix_into(x, shade[i][tt < 0 ? 0 : (tt < depth ? tt : depth)], cov);
            }
        }
        uint32_t *row = s->px + (long)y * s->stride;
        for (int x = 0; x < W; x++) {
            int d = bayer4[y & 3][x & 3] * 16;
            int r = (acc[0][x] + d) >> 8, g = (acc[1][x] + d) >> 8, b = (acc[2][x] + d) >> 8;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            row[x] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    }
}
