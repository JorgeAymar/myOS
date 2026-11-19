// Bitmap text rendering (clipped, one glyph row = one uint16 bitmask).
#include "font.h"
#include "string.h"

int font_width(const struct font *f, const char *s) { return (int)strlen(s) * f->w; }

void gfx_glyph(struct surface *s, const struct font *f, int x, int y, unsigned char ch, color_t c) {
    if (ch == ' ') return;
    if (ch < f->first || ch > f->last) ch = '?';
    const struct rect cl = s->clip;
    int x0 = x > cl.x ? x : cl.x, x1 = x + f->w < cl.x + cl.w ? x + f->w : cl.x + cl.w;
    int y0 = y > cl.y ? y : cl.y, y1 = y + f->h < cl.y + cl.h ? y + f->h : cl.y + cl.h;
    if (x0 >= x1 || y0 >= y1) return;
    const uint16_t *rows = f->rows + (ch - f->first) * f->h;
    uint32_t a8 = ALPHA_OF(c);
    uint32_t a = gfx_a256(a8);
    c |= 0xff000000u;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t bits = (uint32_t)rows[yy - y] << (x0 - x);
        bits &= 0xffffu;
        if (!bits) continue;
        uint32_t *p = s->px + (long)yy * s->stride + x0;
        int n = x1 - x0;
        if (a8 == 255) {
            for (int i = 0; i < n && bits; i++, bits = (bits << 1) & 0xffffu)
                if (bits & 0x8000u) p[i] = c;
        } else {
            for (int i = 0; i < n && bits; i++, bits = (bits << 1) & 0xffffu)
                if (bits & 0x8000u) p[i] = gfx_mix(p[i], c, a);
        }
    }
}

int gfx_text_n(struct surface *s, const struct font *f, int x, int y, const char *str, int n, color_t c) {
    // Skip whole strings outside the clip quickly.
    if (y >= s->clip.y + s->clip.h || y + f->h <= s->clip.y) return x + n * f->w;
    for (int i = 0; i < n && str[i]; i++, x += f->w) {
        if (x >= s->clip.x + s->clip.w) { while (i < n && str[i]) { i++; x += f->w; } break; }
        if (x + f->w <= s->clip.x) continue;
        gfx_glyph(s, f, x, y, (unsigned char)str[i], c);
    }
    return x;
}

int gfx_text(struct surface *s, const struct font *f, int x, int y, const char *str, color_t c) {
    return gfx_text_n(s, f, x, y, str, (int)strlen(str), c);
}

int gfx_text_bold(struct surface *s, const struct font *f, int x, int y, const char *str, color_t c) {
    gfx_text(s, f, x + 1, y, str, c);
    return gfx_text(s, f, x, y, str, c) + 1;
}

void gfx_text_center(struct surface *s, const struct font *f, int x, int w, int y, const char *str, color_t c) {
    gfx_text(s, f, x + (w - font_width(f, str)) / 2, y, str, c);
}

// ---------------------------------------------------------------- AA text
static const struct aglyph *aglyph_of(const struct afont *f, unsigned char ch) {
    if (ch < f->first || ch > f->last) ch = '?';
    return &f->g[ch - f->first];
}

int text_width(const struct afont *f, const char *s) {
    int w = 0;
    for (; *s; s++) w += aglyph_of(f, (unsigned char)*s)->adv;
    return w;
}

int text_draw(struct surface *s, const struct afont *f, int x, int y, const char *str, color_t c) {
    const struct rect cl = s->clip;
    uint32_t ca = gfx_a256(ALPHA_OF(c));
    if (y >= cl.y + cl.h || y + f->height + 4 <= cl.y) return x + text_width(f, str);
    for (; *str; str++) {
        const struct aglyph *g = aglyph_of(f, (unsigned char)*str);
        int gx = x + g->bx, gy = y + g->by;
        x += g->adv;
        if (!g->w) continue;
        int x0 = gx > cl.x ? gx : cl.x, x1 = gx + g->w < cl.x + cl.w ? gx + g->w : cl.x + cl.w;
        int y0 = gy > cl.y ? gy : cl.y, y1 = gy + g->h < cl.y + cl.h ? gy + g->h : cl.y + cl.h;
        if (x0 >= x1 || y0 >= y1) continue;
        for (int yy = y0; yy < y1; yy++) {
            const uint8_t *src = f->data + g->off + (yy - gy) * g->w + (x0 - gx);
            uint32_t *p = s->px + (long)yy * s->stride + x0;
            for (int i = 0; i < x1 - x0; i++) {
                uint32_t a = src[i];
                if (!a) continue;
                a = (gfx_a256(a) * ca) >> 8;
                p[i] = gfx_mix(p[i], c, a);
            }
        }
    }
    return x;
}

void text_center(struct surface *s, const struct afont *f, int x, int w, int y, const char *str, color_t c) {
    text_draw(s, f, x + (w - text_width(f, str)) / 2, y, str, c);
}
