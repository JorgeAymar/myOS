#pragma once
// myOS 2D graphics: 32-bit surfaces, clipped primitives, alpha blending.
// Integer math only (the kernel has no FPU/SSE). Colors are 0xAARRGGBB; the
// alpha byte is honored by the *blend* primitives and ignored by opaque ones.
// Pixels written into a surface always carry alpha 0xff (surfaces are opaque).
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t color_t;

#define RGB(r, g, b)      (0xff000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RGBA(r, g, b, a)  (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define WITH_ALPHA(c, a)  (((c) & 0x00ffffffu) | ((uint32_t)(a) << 24))
#define ALPHA_OF(c)       ((uint32_t)(c) >> 24)

struct rect { int x, y, w, h; };

// A surface is a view over 32-bit pixels. `stride` is in pixels. Every
// primitive is clipped against `clip` (always inside the surface bounds).
struct surface {
    uint32_t *px;
    int w, h, stride;
    struct rect clip;
};

// ---- rectangles ----
static inline struct rect rect_make(int x, int y, int w, int h) { return (struct rect){ x, y, w, h }; }
static inline bool rect_empty(struct rect r) { return r.w <= 0 || r.h <= 0; }
static inline bool rect_contains(struct rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
struct rect rect_intersect(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);       // bounding box (empty rects ignored)
bool rect_overlaps(struct rect a, struct rect b);
struct rect rect_inset(struct rect r, int d);               // negative d grows

// ---- surfaces ----
void gfx_surface_init(struct surface *s, uint32_t *px, int w, int h, int stride);
bool gfx_surface_alloc(struct surface *s, int w, int h);    // kmalloc'd, false on OOM
// Sub-surface view sharing pixels with `parent` (coordinates relative to x,y).
void gfx_surface_sub(const struct surface *parent, struct surface *out, struct rect r);
void gfx_set_clip(struct surface *s, struct rect r);       // intersected with bounds
void gfx_reset_clip(struct surface *s);

// ---- fast word copies (rep movs/stos) ----
void gfx_copy32(uint32_t *dst, const uint32_t *src, int n);
void gfx_fill32(uint32_t *dst, uint32_t v, int n);

// ---- pixel math ----
// a in 0..256 (256 = src fully opaque)
static inline uint32_t gfx_mix(uint32_t d, uint32_t s, uint32_t a) {
    uint32_t rb = ((s & 0xff00ffu) * a + (d & 0xff00ffu) * (256 - a)) >> 8;
    uint32_t g  = ((s & 0x00ff00u) * a + (d & 0x00ff00u) * (256 - a)) >> 8;
    return 0xff000000u | (rb & 0xff00ffu) | (g & 0x00ff00u);
}
static inline uint32_t gfx_a256(uint32_t a8) { return a8 + (a8 >> 7); }
color_t gfx_lerp_color(color_t a, color_t b, int t, int tmax);  // integer lerp incl. alpha
uint32_t gfx_isqrt(uint64_t v);

// ---- primitives (all clipped) ----
void gfx_fill(struct surface *s, struct rect r, color_t c);            // opaque
void gfx_blend(struct surface *s, struct rect r, color_t c);           // uses alpha of c
void gfx_vgradient(struct surface *s, struct rect r, color_t top, color_t bottom); // opaque or alpha
void gfx_hline(struct surface *s, int x, int y, int w, color_t c);     // blended if alpha<255
void gfx_vline(struct surface *s, int x, int y, int h, color_t c);
void gfx_line(struct surface *s, int x0, int y0, int x1, int y1, color_t c); // 1px Bresenham

// Rounded rectangles with anti-aliased corners (radius <= 32).
void gfx_fill_rrect(struct surface *s, struct rect r, int radius, color_t c);
void gfx_stroke_rrect(struct surface *s, struct rect r, int radius, color_t c);  // 1px
// Vertical gradient inside a rounded rect.
void gfx_gradient_rrect(struct surface *s, struct rect r, int radius, color_t top, color_t bottom);

// Soft drop shadow for a rounded rect `r` (shadow is computed for r moved by
// (0, offset_y) and spread `size` px). Pixels inside the rect's straight
// interior are skipped because the caller paints the window over them.
void gfx_shadow(struct surface *s, struct rect r, int radius, int size, int offset_y, int alpha);

// Anti-aliased shapes; coordinates and sizes in 1/16 pixel (subpixel precision).
#define SUBPX 16
void gfx_capsule(struct surface *s, int ax, int ay, int bx, int by, int radius, color_t c);
void gfx_disc(struct surface *s, int cx, int cy, int radius, color_t c);
void gfx_ring(struct surface *s, int cx, int cy, int r_out, int r_in, color_t c);

// ---- blits ----
void gfx_blit(struct surface *dst, int dx, int dy, const struct surface *src);   // opaque
void gfx_blit_rect(struct surface *dst, int dx, int dy, const struct surface *src, struct rect sr);
// Opaque blit whose four corners are masked to an anti-aliased radius.
void gfx_blit_rounded(struct surface *dst, int dx, int dy, const struct surface *src, int radius);
// 1-bit mask with 2 colors (e.g. the cursor): '#'=fg '.'=outline others transparent.
void gfx_draw_mask(struct surface *s, int x, int y, const char *const *rows, int nrows,
                   color_t fg, color_t outline);

// ---- integer trig: sin/cos for 60 steps of 6 degrees, scaled by 16384 ----
int gfx_sin60(int step);
int gfx_cos60(int step);
// 1024 steps per turn, scaled by 16384
int gfx_sin1024(int a);
int gfx_cos1024(int a);

// Separable box blur (radius r, `passes` times, ~Gaussian for 3 passes).
// tmp must hold max(w, h) pixels.
void gfx_box_blur(struct surface *s, int r, int passes, uint32_t *tmp);
// Anti-aliased ring limited to the upward 90-degree wedge (1/16 px units).
void gfx_arc_up(struct surface *s, int cx, int cy, int r_out, int r_in, color_t c);
