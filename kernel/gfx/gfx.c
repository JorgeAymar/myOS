// myOS 2D graphics primitives. Integer-only, clipped, no per-pixel calls in
// the hot loops (span fills use rep stos, copies use rep movs).
#include "gfx.h"
#include "mm.h"

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

// ---------------------------------------------------------------- rects
struct rect rect_intersect(struct rect a, struct rect b) {
    int x0 = imax(a.x, b.x), y0 = imax(a.y, b.y);
    int x1 = imin(a.x + a.w, b.x + b.w), y1 = imin(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return (struct rect){ 0, 0, 0, 0 };
    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

struct rect rect_union(struct rect a, struct rect b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = imin(a.x, b.x), y0 = imin(a.y, b.y);
    int x1 = imax(a.x + a.w, b.x + b.w), y1 = imax(a.y + a.h, b.y + b.h);
    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

bool rect_overlaps(struct rect a, struct rect b) { return !rect_empty(rect_intersect(a, b)); }

struct rect rect_inset(struct rect r, int d) {
    return (struct rect){ r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d };
}

// ---------------------------------------------------------------- surfaces
void gfx_surface_init(struct surface *s, uint32_t *px, int w, int h, int stride) {
    s->px = px; s->w = w; s->h = h; s->stride = stride;
    s->clip = (struct rect){ 0, 0, w, h };
}

bool gfx_surface_alloc(struct surface *s, int w, int h) {
    uint32_t *px = kmalloc((size_t)w * (size_t)h * 4);
    if (!px) return false;
    gfx_surface_init(s, px, w, h, w);
    return true;
}

void gfx_surface_sub(const struct surface *parent, struct surface *out, struct rect r) {
    r = rect_intersect(r, (struct rect){ 0, 0, parent->w, parent->h });
    gfx_surface_init(out, parent->px + r.y * parent->stride + r.x, r.w, r.h, parent->stride);
}

void gfx_set_clip(struct surface *s, struct rect r) {
    s->clip = rect_intersect(r, (struct rect){ 0, 0, s->w, s->h });
}

void gfx_reset_clip(struct surface *s) { s->clip = (struct rect){ 0, 0, s->w, s->h }; }

static inline uint32_t *row_ptr(const struct surface *s, int x, int y) {
    return s->px + (long)y * s->stride + x;
}

// ---------------------------------------------------------------- words
void gfx_copy32(uint32_t *dst, const uint32_t *src, int n) {
    if (n <= 0) return;
    if (n & 1) { *dst++ = *src++; }
    long q = n >> 1;
    __asm__ volatile ("rep movsq" : "+D"(dst), "+S"(src), "+c"(q) :: "memory");
}

void gfx_fill32(uint32_t *dst, uint32_t v, int n) {
    if (n <= 0) return;
    long cnt = n;
    __asm__ volatile ("rep stosl" : "+D"(dst), "+c"(cnt) : "a"(v) : "memory");
}

color_t gfx_lerp_color(color_t a, color_t b, int t, int tmax) {
    if (tmax <= 0) return a;
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
        int v = ca + (cb - ca) * t / tmax;
        out |= (uint32_t)(v & 0xff) << sh;
    }
    return out;
}

uint32_t gfx_isqrt(uint64_t v) {
    uint64_t res = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return (uint32_t)res;
}

// ---------------------------------------------------------------- spans
static void span_blend(uint32_t *p, int n, uint32_t c, uint32_t a) {
    // a in 0..256. Pre-multiply the source once per span.
    uint32_t srb = (c & 0xff00ffu) * a, sg = (c & 0x00ff00u) * a, ia = 256 - a;
    for (int i = 0; i < n; i++) {
        uint32_t d = p[i];
        uint32_t rb = (srb + (d & 0xff00ffu) * ia) >> 8;
        uint32_t g  = (sg + (d & 0x00ff00u) * ia) >> 8;
        p[i] = 0xff000000u | (rb & 0xff00ffu) | (g & 0x00ff00u);
    }
}

static inline void put_blend(uint32_t *p, uint32_t c, uint32_t a) {
    if (a >= 256) *p = c | 0xff000000u;
    else if (a) *p = gfx_mix(*p, c, a);
}

void gfx_fill(struct surface *s, struct rect r, color_t c) {
    r = rect_intersect(r, s->clip);
    if (rect_empty(r)) return;
    c |= 0xff000000u;
    for (int y = r.y; y < r.y + r.h; y++) gfx_fill32(row_ptr(s, r.x, y), c, r.w);
}

void gfx_blend(struct surface *s, struct rect r, color_t c) {
    uint32_t a = ALPHA_OF(c);
    if (a == 255) { gfx_fill(s, r, c); return; }
    if (!a) return;
    r = rect_intersect(r, s->clip);
    if (rect_empty(r)) return;
    a = gfx_a256(a);
    for (int y = r.y; y < r.y + r.h; y++) span_blend(row_ptr(s, r.x, y), r.w, c, a);
}

void gfx_vgradient(struct surface *s, struct rect r, color_t top, color_t bottom) {
    struct rect c = rect_intersect(r, s->clip);
    if (rect_empty(c)) return;
    for (int y = c.y; y < c.y + c.h; y++) {
        color_t col = gfx_lerp_color(top, bottom, y - r.y, r.h > 1 ? r.h - 1 : 1);
        uint32_t a = ALPHA_OF(col);
        if (a == 255) gfx_fill32(row_ptr(s, c.x, y), col, c.w);
        else if (a) span_blend(row_ptr(s, c.x, y), c.w, col, gfx_a256(a));
    }
}

void gfx_hline(struct surface *s, int x, int y, int w, color_t c) {
    gfx_blend(s, (struct rect){ x, y, w, 1 }, c);
}

void gfx_vline(struct surface *s, int x, int y, int h, color_t c) {
    gfx_blend(s, (struct rect){ x, y, 1, h }, c);
}

void gfx_line(struct surface *s, int x0, int y0, int x1, int y1, color_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    uint32_t a = gfx_a256(ALPHA_OF(c));
    struct rect cl = s->clip;
    for (;;) {
        if (rect_contains(cl, x0, y0)) put_blend(row_ptr(s, x0, y0), c, a);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ---------------------------------------------------------------- corners
// Coverage (0..255) of an r x r quarter circle, sampled 4x4 per pixel.
// Index [i*r + j]: row i, column j measured from the outer corner.
#define MAX_RADIUS 32
static uint8_t corner_cov[MAX_RADIUS + 1][MAX_RADIUS * MAX_RADIUS];
static bool corner_ready[MAX_RADIUS + 1];

static const uint8_t *corner_table(int r) {
    if (r > MAX_RADIUS) r = MAX_RADIUS;
    uint8_t *t = corner_cov[r];
    if (!corner_ready[r]) {
        long R = 8L * r, R2 = R * R;
        for (int i = 0; i < r; i++)
            for (int j = 0; j < r; j++) {
                int cnt = 0;
                for (int si = 0; si < 4; si++)
                    for (int sj = 0; sj < 4; sj++) {
                        long py = 8L * i + 2 * si + 1, px = 8L * j + 2 * sj + 1;
                        long dy = R - py, dx = R - px;
                        if (dx * dx + dy * dy <= R2) cnt++;
                    }
                t[i * r + j] = (uint8_t)(cnt * 255 / 16);
            }
        corner_ready[r] = true;
    }
    return t;
}

// Coverage of pixel (px,py) relative to a rounded rect of size w x h.
static inline int rr_cov(const uint8_t *t, int r, int px, int py, int w, int h) {
    int i = py < r ? py : (py >= h - r ? h - 1 - py : -1);
    int j = px < r ? px : (px >= w - r ? w - 1 - px : -1);
    if (i < 0 || j < 0) return 255;
    return t[i * r + j];
}

static int clamp_radius(struct rect r, int radius) {
    if (radius < 0) radius = 0;
    if (radius > MAX_RADIUS) radius = MAX_RADIUS;
    if (radius * 2 > r.w) radius = r.w / 2;
    if (radius * 2 > r.h) radius = r.h / 2;
    return radius;
}

// Generic rounded-rect painter: straight middle rows are spans, only the
// top/bottom `radius` rows touch per-pixel coverage.
static void rrect_paint(struct surface *s, struct rect r, int radius, color_t top, color_t bottom, bool grad) {
    radius = clamp_radius(r, radius);
    struct rect c = rect_intersect(r, s->clip);
    if (rect_empty(c)) return;
    const uint8_t *t = corner_table(radius);
    for (int y = c.y; y < c.y + c.h; y++) {
        color_t col = grad ? gfx_lerp_color(top, bottom, y - r.y, r.h > 1 ? r.h - 1 : 1) : top;
        uint32_t a8 = ALPHA_OF(col);
        if (!a8) continue;
        uint32_t *row = row_ptr(s, 0, y);
        int py = y - r.y;
        if (py >= radius && py < r.h - radius) {
            if (a8 == 255) gfx_fill32(row + c.x, col, c.w);
            else span_blend(row + c.x, c.w, col, gfx_a256(a8));
            continue;
        }
        // corner row: left corner, straight middle, right corner
        int mid0 = imax(c.x, r.x + radius), mid1 = imin(c.x + c.w, r.x + r.w - radius);
        for (int x = c.x; x < imin(mid0, c.x + c.w); x++) {
            int cov = rr_cov(t, radius, x - r.x, py, r.w, r.h);
            put_blend(row + x, col, gfx_a256(a8 * (uint32_t)cov / 255));
        }
        if (mid1 > mid0) {
            if (a8 == 255) gfx_fill32(row + mid0, col, mid1 - mid0);
            else span_blend(row + mid0, mid1 - mid0, col, gfx_a256(a8));
        }
        for (int x = imax(mid1, c.x); x < c.x + c.w; x++) {
            if (x < mid0) continue;
            int cov = rr_cov(t, radius, x - r.x, py, r.w, r.h);
            put_blend(row + x, col, gfx_a256(a8 * (uint32_t)cov / 255));
        }
    }
}

void gfx_fill_rrect(struct surface *s, struct rect r, int radius, color_t c) {
    rrect_paint(s, r, radius, c, c, false);
}

void gfx_gradient_rrect(struct surface *s, struct rect r, int radius, color_t top, color_t bottom) {
    rrect_paint(s, r, radius, top, bottom, true);
}

void gfx_stroke_rrect(struct surface *s, struct rect r, int radius, color_t c) {
    radius = clamp_radius(r, radius);
    uint32_t a8 = ALPHA_OF(c);
    // straight edges
    gfx_hline(s, r.x + radius, r.y, r.w - 2 * radius, c);
    gfx_hline(s, r.x + radius, r.y + r.h - 1, r.w - 2 * radius, c);
    gfx_vline(s, r.x, r.y + radius, r.h - 2 * radius, c);
    gfx_vline(s, r.x + r.w - 1, r.y + radius, r.h - 2 * radius, c);
    if (radius <= 0) return;
    // corners: ring = outer coverage minus inner coverage (inset by 1px)
    const uint8_t *to = corner_table(radius);
    const uint8_t *ti = radius > 1 ? corner_table(radius - 1) : 0;
    for (int i = 0; i < radius; i++)
        for (int j = 0; j < radius; j++) {
            int co = to[i * radius + j];
            int ci = (ti && i > 0 && j > 0) ? ti[(i - 1) * (radius - 1) + (j - 1)] : 0;
            int cov = co - ci;
            if (cov <= 0) continue;
            uint32_t a = gfx_a256(a8 * (uint32_t)cov / 255);
            int xs[2] = { r.x + j, r.x + r.w - 1 - j }, ys[2] = { r.y + i, r.y + r.h - 1 - i };
            for (int k = 0; k < 4; k++) {
                int x = xs[k & 1], y = ys[k >> 1];
                if (rect_contains(s->clip, x, y)) put_blend(row_ptr(s, x, y), c, a);
            }
        }
}

// ---------------------------------------------------------------- shadow
// Alpha lookup by (dx,dy) distance from the rect shrunk by the radius.
#define SH_MAX 64
struct shadow_lut { int size, radius, alpha; uint8_t a[SH_MAX * SH_MAX]; };
static struct shadow_lut shadow_cache[4];
static int shadow_next;

static const struct shadow_lut *shadow_table(int size, int radius, int alpha) {
    for (int i = 0; i < 4; i++)
        if (shadow_cache[i].size == size && shadow_cache[i].radius == radius && shadow_cache[i].alpha == alpha)
            return &shadow_cache[i];
    struct shadow_lut *l = &shadow_cache[shadow_next++ & 3];
    l->size = size; l->radius = radius; l->alpha = alpha;
    int n = size + radius + 1;
    for (int dy = 0; dy < n; dy++)
        for (int dx = 0; dx < n; dx++) {
            int d16 = (int)gfx_isqrt((uint64_t)(dx * dx + dy * dy) * 256) - radius * 16;
            int v;
            if (d16 <= 0) v = alpha;
            else if (d16 >= size * 16) v = 0;
            else {
                int t = 256 - d16 * 256 / (size * 16);       // 256 .. 0
                v = alpha * t / 256 * t / 256;                 // quadratic falloff
            }
            l->a[dy * SH_MAX + dx] = (uint8_t)v;
        }
    return l;
}

void gfx_shadow(struct surface *s, struct rect r, int radius, int size, int offset_y, int alpha) {
    if (size + radius + 1 > SH_MAX) size = SH_MAX - radius - 1;
    const struct shadow_lut *l = shadow_table(size, radius, alpha);
    struct rect sr = rect_inset(r, -size);
    sr.y += offset_y;
    struct rect c = rect_intersect(sr, s->clip);
    if (rect_empty(c)) return;
    int ix0 = r.x + radius, ix1 = r.x + r.w - 1 - radius;
    int iy0 = r.y + offset_y + radius, iy1 = r.y + offset_y + r.h - 1 - radius;
    int lim = size + radius;
    for (int y = c.y; y < c.y + c.h; y++) {
        int dy = y < iy0 ? iy0 - y : (y > iy1 ? y - iy1 : 0);
        if (dy > lim) continue;
        uint32_t *row = row_ptr(s, 0, y);
        // Skip the part covered by the opaque window body.
        bool covered_row = y >= r.y + radius && y < r.y + r.h - radius;
        for (int x = c.x; x < c.x + c.w; x++) {
            if (covered_row && x >= r.x && x < r.x + r.w) { x = r.x + r.w - 1; continue; }
            int dx = x < ix0 ? ix0 - x : (x > ix1 ? x - ix1 : 0);
            if (dx > lim) continue;
            uint32_t a = l->a[dy * SH_MAX + dx];
            if (a) row[x] = gfx_mix(row[x], 0, gfx_a256(a));
        }
    }
}

// ---------------------------------------------------------------- AA shapes
static inline int cov_from_dist(int d, int edge) {
    // edge = shape boundary distance (1/16 px). Linear ramp over one pixel.
    int v = edge + 8 - d;
    if (v <= 0) return 0;
    if (v >= 16) return 256;
    return v * 16;
}

void gfx_capsule(struct surface *s, int ax, int ay, int bx, int by, int radius, color_t c) {
    int x0 = (imin(ax, bx) - radius) / 16 - 1, x1 = (imax(ax, bx) + radius) / 16 + 1;
    int y0 = (imin(ay, by) - radius) / 16 - 1, y1 = (imax(ay, by) + radius) / 16 + 1;
    struct rect cl = rect_intersect((struct rect){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 }, s->clip);
    if (rect_empty(cl)) return;
    long ex = bx - ax, ey = by - ay, len2 = ex * ex + ey * ey;
    long rin = radius - 8 > 0 ? radius - 8 : 0, rout = radius + 8;
    long rin2 = rin * rin, rout2 = rout * rout;
    uint32_t a8 = ALPHA_OF(c);
    for (int y = cl.y; y < cl.y + cl.h; y++) {
        uint32_t *row = row_ptr(s, 0, y);
        long py = 16L * y + 8 - ay;
        for (int x = cl.x; x < cl.x + cl.w; x++) {
            long px = 16L * x + 8 - ax;
            long qx = px, qy = py;
            if (len2) {
                long dot = px * ex + py * ey;
                if (dot > 0) {
                    if (dot >= len2) { qx = px - ex; qy = py - ey; }
                    else { qx = px - ex * dot / len2; qy = py - ey * dot / len2; }
                }
            }
            long d2 = qx * qx + qy * qy;
            if (d2 >= rout2) continue;
            uint32_t cov = d2 <= rin2 ? 256 : (uint32_t)cov_from_dist((int)gfx_isqrt((uint64_t)d2), radius);
            put_blend(row + x, c, (cov * gfx_a256(a8)) >> 8);
        }
    }
}

void gfx_disc(struct surface *s, int cx, int cy, int radius, color_t c) {
    gfx_capsule(s, cx, cy, cx, cy, radius, c);
}

void gfx_ring(struct surface *s, int cx, int cy, int r_out, int r_in, color_t c) {
    int x0 = (cx - r_out) / 16 - 1, x1 = (cx + r_out) / 16 + 1;
    int y0 = (cy - r_out) / 16 - 1, y1 = (cy + r_out) / 16 + 1;
    struct rect cl = rect_intersect((struct rect){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 }, s->clip);
    if (rect_empty(cl)) return;
    long ro2 = (long)(r_out + 8) * (r_out + 8);
    long ri = r_in - 8 > 0 ? r_in - 8 : 0, ri2 = ri * ri;
    long fo = r_out - 8 > 0 ? r_out - 8 : 0, fo2 = fo * fo;   // full-coverage band
    long fi2 = (long)(r_in + 8) * (r_in + 8);
    uint32_t a8 = gfx_a256(ALPHA_OF(c));
    for (int y = cl.y; y < cl.y + cl.h; y++) {
        uint32_t *row = row_ptr(s, 0, y);
        long dy = 16L * y + 8 - cy;
        for (int x = cl.x; x < cl.x + cl.w; x++) {
            long dx = 16L * x + 8 - cx, d2 = dx * dx + dy * dy;
            if (d2 >= ro2 || d2 < ri2) continue;
            uint32_t cov;
            if (d2 <= fo2 && d2 >= fi2) cov = 256;
            else {
                int d = (int)gfx_isqrt((uint64_t)d2);
                int co = cov_from_dist(d, r_out);
                int ci = r_in > 0 ? 256 - cov_from_dist(d, r_in) : 256;
                cov = (uint32_t)imin(co, ci);
            }
            put_blend(row + x, c, (cov * a8) >> 8);
        }
    }
}

// ---------------------------------------------------------------- blits
void gfx_blit_rect(struct surface *dst, int dx, int dy, const struct surface *src, struct rect sr) {
    sr = rect_intersect(sr, (struct rect){ 0, 0, src->w, src->h });
    struct rect d = rect_intersect((struct rect){ dx, dy, sr.w, sr.h }, dst->clip);
    if (rect_empty(d)) return;
    int sx = sr.x + (d.x - dx), sy = sr.y + (d.y - dy);
    for (int y = 0; y < d.h; y++)
        gfx_copy32(row_ptr(dst, d.x, d.y + y), row_ptr(src, sx, sy + y), d.w);
}

void gfx_blit(struct surface *dst, int dx, int dy, const struct surface *src) {
    gfx_blit_rect(dst, dx, dy, src, (struct rect){ 0, 0, src->w, src->h });
}

void gfx_blit_rounded(struct surface *dst, int dx, int dy, const struct surface *src, int radius) {
    struct rect full = { dx, dy, src->w, src->h };
    radius = clamp_radius(full, radius);
    struct rect d = rect_intersect(full, dst->clip);
    if (rect_empty(d)) return;
    const uint8_t *t = corner_table(radius);
    for (int y = d.y; y < d.y + d.h; y++) {
        int py = y - dy;
        uint32_t *drow = row_ptr(dst, 0, y);
        const uint32_t *srow = row_ptr(src, -dx, py);
        if (py >= radius && py < src->h - radius) {
            gfx_copy32(drow + d.x, srow + d.x, d.w);
            continue;
        }
        int m0 = imax(d.x, dx + radius), m1 = imin(d.x + d.w, dx + src->w - radius);
        for (int x = d.x; x < d.x + d.w; x++) {
            if (x >= m0 && x < m1) {
                gfx_copy32(drow + x, srow + x, m1 - x); x = m1 - 1; continue;
            }
            int cov = rr_cov(t, radius, x - dx, py, src->w, src->h);
            put_blend(drow + x, srow[x], gfx_a256((uint32_t)cov));
        }
    }
}

void gfx_draw_mask(struct surface *s, int x, int y, const char *const *rows, int nrows,
                   color_t fg, color_t outline) {
    uint32_t afg = gfx_a256(ALPHA_OF(fg)), aol = gfx_a256(ALPHA_OF(outline));
    for (int i = 0; i < nrows; i++) {
        int yy = y + i;
        if (yy < s->clip.y || yy >= s->clip.y + s->clip.h) continue;
        uint32_t *row = row_ptr(s, 0, yy);
        for (int j = 0; rows[i][j]; j++) {
            int xx = x + j;
            if (xx < s->clip.x || xx >= s->clip.x + s->clip.w) continue;
            char ch = rows[i][j];
            if (ch == '#') put_blend(row + xx, fg, afg);
            else if (ch == '.') put_blend(row + xx, outline, aol);
            else if (ch == ':') put_blend(row + xx, outline, aol / 2);
        }
    }
}

// ---------------------------------------------------------------- trig
static const int16_t sin60[60] = {
    0, 1713, 3406, 5063, 6664, 8192, 9630, 10963, 12176, 13255, 14189, 14968, 15582, 16026, 16294,
    16384, 16294, 16026, 15582, 14968, 14189, 13255, 12176, 10963, 9630, 8192, 6664, 5063, 3406, 1713,
    0, -1713, -3406, -5063, -6664, -8192, -9630, -10963, -12176, -13255, -14189, -14968, -15582, -16026, -16294,
    -16384, -16294, -16026, -15582, -14968, -14189, -13255, -12176, -10963, -9630, -8192, -6664, -5063, -3406, -1713,
};
int gfx_sin60(int step) { step %= 60; if (step < 0) step += 60; return sin60[step]; }
int gfx_cos60(int step) { return gfx_sin60(step + 15); }

// Quarter-wave table: sin(i * 2pi / 1024) * 16384 for i = 0..256.
static const int16_t sinq[257] = {
    0, 101, 201, 302, 402, 503, 603, 704, 804, 904, 1005, 1105, 1205, 1306, 1406, 1506, 1606, 1706, 1806, 1906, 2006, 2105, 2205, 2305, 2404, 2503, 2603, 2702, 2801, 2900, 2999, 3098, 3196, 3295, 3393, 3492, 3590, 3688, 3786, 3883, 3981, 4078, 4176, 4273, 4370, 4467, 4563, 4660, 4756, 4852, 4948, 5044, 5139, 5235, 5330, 5425, 5520, 5614, 5708, 5803, 5897, 5990, 6084, 6177, 6270, 6363, 6455, 6547, 6639, 6731, 6823, 6914, 7005, 7096, 7186, 7276, 7366, 7456, 7545, 7635, 7723, 7812, 7900, 7988, 8076, 8163, 8250, 8337, 8423, 8509, 8595, 8680, 8765, 8850, 8935, 9019, 9102, 9186, 9269, 9352, 9434, 9516, 9598, 9679, 9760, 9841, 9921, 10001, 10080, 10159, 10238, 10316, 10394, 10471, 10549, 10625, 10702, 10778, 10853, 10928, 11003, 11077, 11151, 11224, 11297, 11370, 11442, 11514, 11585, 11656, 11727, 11797, 11866, 11935, 12004, 12072, 12140, 12207, 12274, 12340, 12406, 12472, 12537, 12601, 12665, 12729, 12792, 12854, 12916, 12978, 13039, 13100, 13160, 13219, 13279, 13337, 13395, 13453, 13510, 13567, 13623, 13678, 13733, 13788, 13842, 13896, 13949, 14001, 14053, 14104, 14155, 14206, 14256, 14305, 14354, 14402, 14449, 14497, 14543, 14589, 14635, 14680, 14724, 14768, 14811, 14854, 14896, 14937, 14978, 15019, 15059, 15098, 15137, 15175, 15213, 15250, 15286, 15322, 15357, 15392, 15426, 15460, 15493, 15525, 15557, 15588, 15619, 15649, 15679, 15707, 15736, 15763, 15791, 15817, 15843, 15868, 15893, 15917, 15941, 15964, 15986, 16008, 16029, 16049, 16069, 16088, 16107, 16125, 16143, 16160, 16176, 16192, 16207, 16221, 16235, 16248, 16261, 16273, 16284, 16295, 16305, 16315, 16324, 16332, 16340, 16347, 16353, 16359, 16364, 16369, 16373, 16376, 16379, 16381, 16383, 16384, 16384,
};

int gfx_sin1024(int a) {
    a &= 1023;
    if (a < 256) return sinq[a];
    if (a < 512) return sinq[512 - a];
    if (a < 768) return -sinq[a - 512];
    return -sinq[1024 - a];
}
int gfx_cos1024(int a) { return gfx_sin1024(a + 256); }

// ---------------------------------------------------------------- blur
// One horizontal + one vertical box pass of radius r (edge-clamped), done
// `passes` times: ~Gaussian. tmp must hold max(w,h) pixels.
static void box_line(uint32_t *p, int n, int step, int r, uint32_t *tmp) {
    for (int i = 0; i < n; i++) tmp[i] = p[(long)i * step];
    uint32_t sr = 0, sg = 0, sb = 0;
    int win = 2 * r + 1;
    for (int k = -r; k <= r; k++) {
        uint32_t v = tmp[k < 0 ? 0 : (k >= n ? n - 1 : k)];
        sr += (v >> 16) & 0xff; sg += (v >> 8) & 0xff; sb += v & 0xff;
    }
    for (int i = 0; i < n; i++) {
        p[(long)i * step] = 0xff000000u | (sr / win) << 16 | (sg / win) << 8 | (sb / win);
        int out = i - r, in = i + r + 1;
        uint32_t vo = tmp[out < 0 ? 0 : out], vi = tmp[in >= n ? n - 1 : in];
        sr += ((vi >> 16) & 0xff) - ((vo >> 16) & 0xff);
        sg += ((vi >> 8) & 0xff) - ((vo >> 8) & 0xff);
        sb += (vi & 0xff) - (vo & 0xff);
    }
}

void gfx_box_blur(struct surface *s, int r, int passes, uint32_t *tmp) {
    for (int p = 0; p < passes; p++) {
        for (int y = 0; y < s->h; y++) box_line(row_ptr(s, 0, y), s->w, 1, r, tmp);
        for (int x = 0; x < s->w; x++) box_line(row_ptr(s, x, 0), s->h, s->stride, r, tmp);
    }
}

// Ring restricted to the upward 90-degree wedge (|dx| <= -dy), e.g. wifi arcs.
void gfx_arc_up(struct surface *s, int cx, int cy, int r_out, int r_in, color_t c) {
    struct surface t = *s;
    // clip to the upper half; the wedge test happens per pixel below
    int x0 = (cx - r_out) / 16 - 1, y0 = (cy - r_out) / 16 - 1;
    struct rect box = { x0, y0, (2 * r_out) / 16 + 3, (cy / 16) - y0 + 1 };
    gfx_set_clip(&t, rect_intersect(box, s->clip));
    if (rect_empty(t.clip)) return;
    uint32_t a8 = gfx_a256(ALPHA_OF(c));
    for (int y = t.clip.y; y < t.clip.y + t.clip.h; y++) {
        uint32_t *row = row_ptr(s, 0, y);
        long dy = 16L * y + 8 - cy;
        for (int x = t.clip.x; x < t.clip.x + t.clip.w; x++) {
            long dx = 16L * x + 8 - cx;
            long adx = dx < 0 ? -dx : dx;
            if (dy > 0) continue;
            // soft wedge edge over ~1px
            long wedge = (-dy - adx) + 12;
            if (wedge <= 0) continue;
            int d = (int)gfx_isqrt((uint64_t)(dx * dx + dy * dy));
            int co = cov_from_dist(d, r_out);
            int ci = 256 - cov_from_dist(d, r_in);
            int cov = imin(co, ci);
            if (wedge < 24) cov = cov * (int)wedge / 24;
            if (cov > 0) put_blend(row + x, c, ((uint32_t)cov * a8) >> 8);
        }
    }
}
