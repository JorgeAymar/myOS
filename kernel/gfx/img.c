// Image decoding front-end over stb_image (third_party/stb). Integer math only.
#include "img.h"
#include "mm.h"
#include "string.h"

int stbwrap_info(const void *d, size_t len, int *w, int *h);
uint8_t *stbwrap_load(const void *d, size_t len, int *w, int *h);

void img_free(struct img *im) {
    if (im->px) kfree(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}

// Area-average with alpha weighting (premultiplied sums), then un-premultiply.
bool img_scale(const struct img *src, int nw, int nh, struct img *out) {
    out->w = out->h = 0;
    out->px = NULL;
    if (nw <= 0 || nh <= 0 || nw > src->w || nh > src->h) return false;
    uint32_t *dst = kmalloc((size_t)nw * nh * 4);
    if (!dst) return false;
    for (int y = 0; y < nh; y++) {
        int sy0 = (int)((int64_t)y * src->h / nh);
        int sy1 = (int)((int64_t)(y + 1) * src->h / nh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < nw; x++) {
            int sx0 = (int)((int64_t)x * src->w / nw);
            int sx1 = (int)((int64_t)(x + 1) * src->w / nw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint64_t r = 0, g = 0, b = 0, a = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint32_t *row = src->px + (size_t)sy * src->w;
                for (int sx = sx0; sx < sx1; sx++) {
                    uint32_t p = row[sx], pa = p >> 24;
                    a += pa;
                    r += ((p >> 16) & 0xff) * pa;
                    g += ((p >> 8) & 0xff) * pa;
                    b += (p & 0xff) * pa;
                }
            }
            uint64_t n = (uint64_t)(sx1 - sx0) * (sy1 - sy0);
            uint32_t oa = (uint32_t)((a + n / 2) / n);
            uint32_t or_ = 0, og = 0, ob = 0;
            if (a) {
                or_ = (uint32_t)((r + a / 2) / a);
                og = (uint32_t)((g + a / 2) / a);
                ob = (uint32_t)((b + a / 2) / a);
            }
            dst[(size_t)y * nw + x] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
        }
    }
    out->w = nw;
    out->h = nh;
    out->px = dst;
    return true;
}

bool img_decode(const void *data, size_t len, int max_w, int max_h, struct img *out) {
    out->w = out->h = 0;
    out->px = NULL;
    if (!data || len < 8) return false;
    int w = 0, h = 0;
    if (!stbwrap_info(data, len, &w, &h)) return false;
    if (w <= 0 || h <= 0 || w > IMG_MAX_SIDE || h > IMG_MAX_SIDE ||
        (int64_t)w * h > IMG_MAX_PIXELS)
        return false;
    uint8_t *raw = stbwrap_load(data, len, &w, &h);
    if (!raw) return false;
    // RGBA bytes -> 0xAARRGGBB in place.
    uint32_t *px = (uint32_t *)raw;
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = raw + i * 4;
        px[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    // Careful: in-place write at i only overwrites bytes i*4..i*4+3 after they were read.
    struct img full = { w, h, px };
    int nw = w, nh = h;
    if (max_w > 0 && nw > max_w) { nh = (int)((int64_t)nh * max_w / nw); nw = max_w; }
    if (max_h > 0 && nh > max_h) { nw = (int)((int64_t)nw * max_h / nh); nh = max_h; }
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw == w && nh == h) { *out = full; return true; }
    bool ok = img_scale(&full, nw, nh, out);
    img_free(&full);
    return ok;
}

void img_draw(struct surface *s, const struct img *im, int x, int y) {
    if (!im->px) return;
    struct rect r = rect_intersect(rect_make(x, y, im->w, im->h), s->clip);
    if (rect_empty(r)) return;
    for (int dy = r.y; dy < r.y + r.h; dy++) {
        const uint32_t *src = im->px + (size_t)(dy - y) * im->w + (r.x - x);
        uint32_t *dst = s->px + (size_t)dy * s->stride + r.x;
        for (int i = 0; i < r.w; i++) {
            uint32_t p = src[i], a = p >> 24;
            if (a == 255) dst[i] = p;
            else if (a) dst[i] = gfx_mix(dst[i], p, gfx_a256(a));
        }
    }
}
