// Small bounded CSS engine (see css.h). Integer only. Every parse loop consumes input on each
// iteration; the only recursion is selector matching, limited to CSS_MAX_PARTS levels.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "css.h"
#include "mm.h"
#include "string.h"

#define MAX_DECL_POOL 20000
#define DF_IMP     1
#define DF_INHERIT 2
enum { U_NONE, U_PX, U_EM, U_PCT };
#define INLINE_SPEC (1ull << 24)

// Property ids; order matches the CSS_H_* bits.
enum { P_DISPLAY, P_VISIBILITY, P_COLOR, P_BG, P_BOLD, P_ITALIC, P_FSIZE, P_ALIGN, P_DECO, P_WS,
       P_MT, P_MB, P_PT, P_PB, P_WIDTH, P_MAXW, P_LIST, P_FLOAT, P_POS, P_OVERFLOW, P_TT, P_COUNT };
_Static_assert(P_TT == 20 && CSS_H_TRANSFORM == (1u << P_TT) && CSS_H_BG == (1u << P_BG) &&
               CSS_H_MAXW == (1u << P_MAXW), "css property/bit order");

struct css_decl {
    uint8_t prop, flags, unit, pad;
    int32_t val;
};
struct cpart {
    uint16_t tag, id, cls[3];  // offsets into sheet text (0 = none)
    uint8_t comb;              // relation to the part on its left: 0 descendant, 1 child
    uint8_t pad;
};
struct crule {
    struct cpart parts[CSS_MAX_PARTS];
    uint32_t spec;             // (ids << 16) | (classes << 8) | tags
    uint32_t dstart;
    uint16_t order;
    uint8_t nparts, dcount;
};
struct css_sheet {
    char *text; size_t text_len, text_cap;
    struct crule *rules; size_t nrules, rules_cap;
    struct css_decl *decls; size_t ndecls, decls_cap;
    size_t consumed;
};

static int grow(void **p, size_t *cap, size_t esz, size_t need, size_t max) {
    if (need <= *cap) return 1;
    if (need > max) return 0;
    size_t nc = *cap ? *cap * 2 : 64;
    while (nc < need) nc *= 2;
    if (nc > max) nc = max;
    void *n = kmalloc(nc * esz);
    if (!n) return 0;
    if (*p) { memcpy(n, *p, *cap * esz); kfree(*p); }
    *p = n; *cap = nc;
    return 1;
}

struct css_sheet *css_sheet_new(void) {
    struct css_sheet *sh = kmalloc(sizeof *sh);
    if (!sh) return NULL;
    memset(sh, 0, sizeof *sh);
    if (!grow((void **)&sh->text, &sh->text_cap, 1, 256, CSS_MAX_TEXT)) { kfree(sh); return NULL; }
    sh->text[0] = 0;
    sh->text_len = 1;
    return sh;
}
void css_free(struct css_sheet *sh) {
    if (!sh) return;
    kfree(sh->text); kfree(sh->rules); kfree(sh->decls); kfree(sh);
}
int css_rule_count(const struct css_sheet *sh) { return sh ? (int)sh->nrules : 0; }

// ---------------------------------------------------------------- small helpers
static inline bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static inline bool is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || (unsigned char)c >= 0x80;
}
static inline char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// s[i] is a quote. Returns the index after the closing quote, or at the newline/end.
static size_t skip_string(const char *s, size_t i, size_t n) {
    char q = s[i++];
    while (i < n) {
        char c = s[i];
        if (c == '\\') { i += 2; continue; }
        if (c == q) return i + 1;
        if (c == '\n') return i;
        i++;
    }
    return n;
}

// Copies src to dst without comments / CDO-CDC, NUL -> space. Returns new length (<= len).
static size_t css_clean(const char *src, size_t len, char *dst) {
    size_t i = 0, o = 0;
    char q = 0;
    while (i < len) {
        char c = src[i];
        if (q) {
            dst[o++] = c ? c : ' ';
            if (c == '\\' && i + 1 < len) { dst[o++] = src[i + 1] ? src[i + 1] : ' '; i += 2; continue; }
            if (c == q || c == '\n') q = 0;
            i++;
        } else if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            i += 2;
            while (i < len && !(src[i] == '*' && i + 1 < len && src[i + 1] == '/')) i++;
            i = i + 2 > len ? len : i + 2;
            dst[o++] = ' ';
        } else if (c == '<' && i + 3 < len && src[i + 1] == '!' && src[i + 2] == '-' && src[i + 3] == '-') {
            i += 4; dst[o++] = ' ';
        } else if (c == '-' && i + 2 < len && src[i + 1] == '-' && src[i + 2] == '>') {
            i += 3; dst[o++] = ' ';
        } else {
            if (c == '"' || c == '\'') q = c;
            dst[o++] = c ? c : ' ';
            i++;
        }
    }
    return o;
}

// Fixed point number (x100) from p[0..n). Sets *used. Returns 0 if no digits.
static int parse_fixed(const char *p, int n, int *used, int32_t *out) {
    int i = 0, neg = 0;
    if (i < n && (p[i] == '+' || p[i] == '-')) { neg = p[i] == '-'; i++; }
    int64_t ip = 0; int nd = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        if (ip < 100000000) ip = ip * 10 + (p[i] - '0');
        i++; nd++;
    }
    int64_t fr = 0; int fd = 0;
    if (i < n && p[i] == '.') {
        i++;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            if (fd < 2) { fr = fr * 10 + (p[i] - '0'); fd++; }
            i++; nd++;
        }
    }
    if (!nd) return 0;
    if (fd == 1) fr *= 10;
    int64_t v = ip * 100 + fr;
    if (v > 100000000) v = 100000000;
    *out = (int32_t)(neg ? -v : v);
    *used = i;
    return 1;
}

static int32_t clamp32(int64_t v) { return v > 100000000 ? 100000000 : v < -100000000 ? -100000000 : (int32_t)v; }

// Length token: sets *val (x100) and *unit. rem/pt are converted to px.
static int parse_len(const char *p, int n, int32_t *val, int *unit) {
    int used; int32_t v;
    if (!parse_fixed(p, n, &used, &v)) return 0;
    const char *u = p + used; int ul = n - used;
    if (ul == 0) { if (v != 0) return 0; *val = 0; *unit = U_PX; return 1; }
    if (ul == 2 && u[0] == 'p' && u[1] == 'x') { *val = v; *unit = U_PX; return 1; }
    if (ul == 2 && u[0] == 'e' && u[1] == 'm') { *val = v; *unit = U_EM; return 1; }
    if (ul == 1 && u[0] == '%') { *val = v; *unit = U_PCT; return 1; }
    if (ul == 3 && !memcmp(u, "rem", 3)) { *val = clamp32((int64_t)v * 16); *unit = U_PX; return 1; }
    if (ul == 2 && u[0] == 'p' && u[1] == 't') { *val = clamp32((int64_t)v * 4 / 3); *unit = U_PX; return 1; }
    return 0;
}

// ---------------------------------------------------------------- colors
static const struct { const char *name; uint32_t rgb; } named[] = {
    {"black", 0x000000}, {"silver", 0xc0c0c0}, {"gray", 0x808080}, {"grey", 0x808080},
    {"white", 0xffffff}, {"maroon", 0x800000}, {"red", 0xff0000}, {"purple", 0x800080},
    {"fuchsia", 0xff00ff}, {"magenta", 0xff00ff}, {"green", 0x008000}, {"lime", 0x00ff00},
    {"olive", 0x808000}, {"yellow", 0xffff00}, {"navy", 0x000080}, {"blue", 0x0000ff},
    {"teal", 0x008080}, {"aqua", 0x00ffff}, {"cyan", 0x00ffff}, {"orange", 0xffa500},
    {"brown", 0xa52a2a}, {"pink", 0xffc0cb}, {"gold", 0xffd700}, {"indigo", 0x4b0082},
    {"violet", 0xee82ee}, {"tan", 0xd2b48c}, {"beige", 0xf5f5dc}, {"coral", 0xff7f50},
    {"crimson", 0xdc143c}, {"salmon", 0xfa8072}, {"khaki", 0xf0e68c}, {"lightgray", 0xd3d3d3},
    {"lightgrey", 0xd3d3d3}, {"darkgray", 0xa9a9a9}, {"darkgrey", 0xa9a9a9}, {"dimgray", 0x696969},
    {"darkgreen", 0x006400}, {"darkblue", 0x00008b}, {"darkred", 0x8b0000}, {"steelblue", 0x4682b4},
    {"royalblue", 0x4169e1}, {"dodgerblue", 0x1e90ff}, {"skyblue", 0x87ceeb}, {"lightblue", 0xadd8e6},
    {"whitesmoke", 0xf5f5f5}, {"gainsboro", 0xdcdcdc}, {"orangered", 0xff4500}, {"ivory", 0xfffff0},
    {"lightyellow", 0xffffe0}, {"lightgreen", 0x90ee90}, {"firebrick", 0xb22222}, {"slategray", 0x708090},
    {"midnightblue", 0x191970}, {"chocolate", 0xd2691e}, {"tomato", 0xff6347}, {"turquoise", 0x40e0d0},
};

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static uint32_t blend(uint32_t c, int a) {  // a 0..100 against white
    uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    r = (r * a + 255 * (100 - a)) / 100; g = (g * a + 255 * (100 - a)) / 100; b = (b * a + 255 * (100 - a)) / 100;
    return (r << 16) | (g << 8) | b;
}

// 0 = not a color, 1 = ok, 2 = transparent
static int parse_color(const char *p, int n, uint32_t *out) {
    if (n <= 0) return 0;
    if (p[0] == '#') {
        int d[8];
        if (n != 4 && n != 5 && n != 7 && n != 9) return 0;
        for (int i = 1; i < n; i++) if ((d[i - 1] = hexv(p[i])) < 0) return 0;
        uint32_t c; int a = 100;
        if (n <= 5) {
            c = ((uint32_t)(d[0] * 17) << 16) | ((uint32_t)(d[1] * 17) << 8) | (uint32_t)(d[2] * 17);
            if (n == 5) a = d[3] * 17 * 100 / 255;
        } else {
            c = ((uint32_t)(d[0] * 16 + d[1]) << 16) | ((uint32_t)(d[2] * 16 + d[3]) << 8) | (uint32_t)(d[4] * 16 + d[5]);
            if (n == 9) a = (d[6] * 16 + d[7]) * 100 / 255;
        }
        if (a == 0) return 2;
        *out = blend(c, a);
        return 1;
    }
    int skip = 0;
    if (n > 5 && !memcmp(p, "rgba(", 5)) skip = 5;
    else if (n > 4 && !memcmp(p, "rgb(", 4)) skip = 4;
    if (skip) {
        int i = skip, k = 0; int32_t comp[4] = {0, 0, 0, 100}; int pct[4] = {0, 0, 0, 0};
        while (k < 4 && i < n) {
            while (i < n && (is_ws(p[i]) || p[i] == ',' || p[i] == '/')) i++;
            if (i >= n || p[i] == ')') break;
            int used; int32_t v;
            if (!parse_fixed(p + i, n - i, &used, &v)) return 0;
            i += used;
            if (i < n && p[i] == '%') { pct[k] = 1; i++; }
            comp[k++] = v;
        }
        if (k < 3) return 0;
        int32_t rgb[3];
        for (int j = 0; j < 3; j++) {
            int64_t v = pct[j] ? (int64_t)comp[j] * 255 / 10000 : comp[j] / 100;
            rgb[j] = v < 0 ? 0 : v > 255 ? 255 : (int32_t)v;
        }
        int a = k == 4 ? (pct[3] ? comp[3] / 100 : comp[3]) : 100;
        if (a < 0) a = 0;
        if (a > 100) a = 100;
        if (a == 0) return 2;
        *out = blend(((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | (uint32_t)rgb[2], a);
        return 1;
    }
    if (n == 11 && !memcmp(p, "transparent", 11)) return 2;
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        if ((int)strlen(named[i].name) == n && !memcmp(named[i].name, p, (size_t)n)) { *out = named[i].rgb; return 1; }
    return 0;
}

// ---------------------------------------------------------------- declarations
struct dlist { struct css_decl *d; int n, cap; };
struct tok { const char *p; int n; };

static void emit(struct dlist *dl, int prop, int flags, int unit, int32_t val) {
    if (dl->n >= dl->cap) return;
    struct css_decl *d = &dl->d[dl->n++];
    d->prop = (uint8_t)prop; d->flags = (uint8_t)flags; d->unit = (uint8_t)unit; d->pad = 0; d->val = val;
}

static int tokenize(const char *v, size_t vl, struct tok *t, int max) {
    int cnt = 0; size_t i = 0;
    while (i < vl && cnt < max) {
        while (i < vl && is_ws(v[i])) i++;
        if (i >= vl) break;
        size_t s = i; int pd = 0;
        while (i < vl && (pd || !is_ws(v[i]))) {
            if (v[i] == '(') pd++;
            else if (v[i] == ')' && pd) pd--;
            i++;
        }
        t[cnt].p = v + s; t[cnt].n = (int)(i - s); cnt++;
    }
    return cnt;
}
static bool teq(const struct tok *t, const char *s) { return (size_t)t->n == strlen(s) && !memcmp(t->p, s, (size_t)t->n); }

static const struct { const char *name; int px; } fs_kw[] = {
    {"xx-small", 9}, {"x-small", 10}, {"small", 12}, {"medium", 16}, {"large", 18},
    {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 40},
};

static void apply_prop(struct dlist *dl, const char *nm, const char *v, size_t vl, int imp) {
    int f = imp ? DF_IMP : 0;
    struct tok t[6];
    int nt = tokenize(v, vl, t, 6);
    if (nt == 0) return;
    if (strstr(v, "var(") || strstr(v, "calc(") || strstr(v, "env(") || strstr(v, "attr(")) return;
    int prop = -1;
    if (!strcmp(nm, "color")) prop = P_COLOR;
    else if (!strcmp(nm, "font-weight")) prop = P_BOLD;
    else if (!strcmp(nm, "font-style")) prop = P_ITALIC;
    else if (!strcmp(nm, "font-size")) prop = P_FSIZE;
    else if (!strcmp(nm, "text-align")) prop = P_ALIGN;
    else if (!strcmp(nm, "visibility")) prop = P_VISIBILITY;
    else if (!strcmp(nm, "white-space")) prop = P_WS;
    else if (!strcmp(nm, "text-transform")) prop = P_TT;
    else if (!strcmp(nm, "list-style") || !strcmp(nm, "list-style-type")) prop = P_LIST;
    if (prop >= 0 && nt == 1 && (teq(&t[0], "inherit") || teq(&t[0], "unset"))) { emit(dl, prop, f | DF_INHERIT, U_NONE, 0); return; }
    if (nt == 1 && (teq(&t[0], "inherit") || teq(&t[0], "initial") || teq(&t[0], "unset") || teq(&t[0], "revert"))) return;

    uint32_t col;
    if (!strcmp(nm, "display")) {
        static const struct { const char *n; int v; } dv[] = {
            {"none", CSS_DISPLAY_NONE}, {"block", CSS_DISPLAY_BLOCK}, {"flex", CSS_DISPLAY_BLOCK},
            {"grid", CSS_DISPLAY_BLOCK}, {"flow-root", CSS_DISPLAY_BLOCK}, {"inline", CSS_DISPLAY_INLINE},
            {"inline-block", CSS_DISPLAY_INLINE_BLOCK}, {"inline-flex", CSS_DISPLAY_INLINE_BLOCK},
            {"inline-grid", CSS_DISPLAY_INLINE_BLOCK}, {"list-item", CSS_DISPLAY_LIST_ITEM},
            {"table", CSS_DISPLAY_TABLE}, {"table-row", CSS_DISPLAY_TABLE_ROW},
            {"table-row-group", CSS_DISPLAY_TABLE_ROW}, {"table-header-group", CSS_DISPLAY_TABLE_ROW},
            {"table-footer-group", CSS_DISPLAY_TABLE_ROW}, {"table-cell", CSS_DISPLAY_TABLE_CELL},
            {"table-caption", CSS_DISPLAY_BLOCK},
        };
        for (size_t i = 0; i < sizeof dv / sizeof dv[0]; i++)
            if (teq(&t[0], dv[i].n)) { emit(dl, P_DISPLAY, f, U_NONE, dv[i].v); return; }
    } else if (prop == P_VISIBILITY) {
        if (teq(&t[0], "hidden") || teq(&t[0], "collapse")) emit(dl, P_VISIBILITY, f, U_NONE, 1);
        else if (teq(&t[0], "visible")) emit(dl, P_VISIBILITY, f, U_NONE, 0);
    } else if (prop == P_COLOR) {
        if (parse_color(t[0].p, t[0].n, &col) == 1) emit(dl, P_COLOR, f, U_NONE, (int32_t)col);
    } else if (!strcmp(nm, "background-color") || !strcmp(nm, "background")) {
        for (int i = 0; i < nt; i++) {
            int r = parse_color(t[i].p, t[i].n, &col);
            if (r == 1) { emit(dl, P_BG, f, U_NONE, (int32_t)col); return; }
            if (r == 2) { emit(dl, P_BG, f, U_NONE, -1); return; }
        }
        if (nt == 1 && teq(&t[0], "none")) emit(dl, P_BG, f, U_NONE, -1);
    } else if (prop == P_BOLD) {
        int used; int32_t n;
        if (teq(&t[0], "bold") || teq(&t[0], "bolder")) emit(dl, P_BOLD, f, U_NONE, 1);
        else if (teq(&t[0], "normal") || teq(&t[0], "lighter")) emit(dl, P_BOLD, f, U_NONE, 0);
        else if (parse_fixed(t[0].p, t[0].n, &used, &n) && used == t[0].n) emit(dl, P_BOLD, f, U_NONE, n >= 60000 ? 1 : 0);
    } else if (prop == P_ITALIC) {
        if (teq(&t[0], "italic") || teq(&t[0], "oblique")) emit(dl, P_ITALIC, f, U_NONE, 1);
        else if (teq(&t[0], "normal")) emit(dl, P_ITALIC, f, U_NONE, 0);
    } else if (prop == P_FSIZE) {
        int32_t val; int unit;
        for (size_t i = 0; i < sizeof fs_kw / sizeof fs_kw[0]; i++)
            if (teq(&t[0], fs_kw[i].name)) { emit(dl, P_FSIZE, f, U_PX, fs_kw[i].px * 100); return; }
        if (teq(&t[0], "larger")) emit(dl, P_FSIZE, f, U_EM, 120);
        else if (teq(&t[0], "smaller")) emit(dl, P_FSIZE, f, U_EM, 83);
        else if (parse_len(t[0].p, t[0].n, &val, &unit) && val > 0) emit(dl, P_FSIZE, f, unit, val);
    } else if (prop == P_ALIGN) {
        if (teq(&t[0], "left") || teq(&t[0], "start") || teq(&t[0], "justify")) emit(dl, P_ALIGN, f, U_NONE, CSS_ALIGN_LEFT);
        else if (teq(&t[0], "center") || teq(&t[0], "-webkit-center")) emit(dl, P_ALIGN, f, U_NONE, CSS_ALIGN_CENTER);
        else if (teq(&t[0], "right") || teq(&t[0], "end")) emit(dl, P_ALIGN, f, U_NONE, CSS_ALIGN_RIGHT);
    } else if (!strcmp(nm, "text-decoration") || !strcmp(nm, "text-decoration-line")) {
        for (int i = 0; i < nt; i++) {
            if (teq(&t[i], "underline")) { emit(dl, P_DECO, f, U_NONE, CSS_DECO_UNDERLINE); return; }
            if (teq(&t[i], "line-through")) { emit(dl, P_DECO, f, U_NONE, CSS_DECO_LINE_THROUGH); return; }
            if (teq(&t[i], "none")) { emit(dl, P_DECO, f, U_NONE, CSS_DECO_NONE); return; }
        }
    } else if (prop == P_WS) {
        if (teq(&t[0], "pre") || teq(&t[0], "pre-wrap") || teq(&t[0], "break-spaces")) emit(dl, P_WS, f, U_NONE, 1);
        else if (teq(&t[0], "normal") || teq(&t[0], "nowrap") || teq(&t[0], "pre-line")) emit(dl, P_WS, f, U_NONE, 0);
    } else if (!strcmp(nm, "margin-top") || !strcmp(nm, "margin-bottom") ||
               !strcmp(nm, "padding-top") || !strcmp(nm, "padding-bottom")) {
        int32_t val; int unit;
        int p = nm[0] == 'm' ? (nm[7] == 't' ? P_MT : P_MB) : (nm[8] == 't' ? P_PT : P_PB);
        if (teq(&t[0], "auto")) { emit(dl, p, f, U_PX, 0); return; }
        if (parse_len(t[0].p, t[0].n, &val, &unit) && unit != U_PCT) emit(dl, p, f, unit, val);
    } else if (!strcmp(nm, "margin") || !strcmp(nm, "padding")) {
        int32_t val[4]; int unit[4];
        if (nt > 4) return;
        for (int i = 0; i < nt; i++) {
            if (teq(&t[i], "auto")) { val[i] = 0; unit[i] = U_PX; continue; }
            if (!parse_len(t[i].p, t[i].n, &val[i], &unit[i])) return;
            if (unit[i] == U_PCT) { val[i] = 0; unit[i] = U_PX; }
        }
        int bi = nt >= 3 ? 2 : 0;
        bool m = nm[0] == 'm';
        emit(dl, m ? P_MT : P_PT, f, unit[0], val[0]);
        emit(dl, m ? P_MB : P_PB, f, unit[bi], val[bi]);
    } else if (!strcmp(nm, "width") || !strcmp(nm, "max-width")) {
        int p = nm[0] == 'w' ? P_WIDTH : P_MAXW;
        int32_t val; int unit;
        if (teq(&t[0], "auto") || teq(&t[0], "none")) { emit(dl, p, f, U_NONE, 0); return; }
        if (parse_len(t[0].p, t[0].n, &val, &unit) && val >= 0) emit(dl, p, f, unit, val);
    } else if (prop == P_LIST) {
        for (int i = 0; i < nt; i++) {
            if (teq(&t[i], "none")) { emit(dl, P_LIST, f, U_NONE, 1); return; }
            if (teq(&t[i], "disc") || teq(&t[i], "circle") || teq(&t[i], "square") || teq(&t[i], "decimal") ||
                teq(&t[i], "lower-alpha") || teq(&t[i], "upper-alpha") || teq(&t[i], "lower-roman") ||
                teq(&t[i], "upper-roman") || teq(&t[i], "decimal-leading-zero")) { emit(dl, P_LIST, f, U_NONE, 0); return; }
        }
    } else if (!strcmp(nm, "float")) {
        if (teq(&t[0], "left")) emit(dl, P_FLOAT, f, U_NONE, CSS_FLOAT_LEFT);
        else if (teq(&t[0], "right")) emit(dl, P_FLOAT, f, U_NONE, CSS_FLOAT_RIGHT);
        else if (teq(&t[0], "none")) emit(dl, P_FLOAT, f, U_NONE, CSS_FLOAT_NONE);
    } else if (!strcmp(nm, "position")) {
        if (teq(&t[0], "absolute")) emit(dl, P_POS, f, U_NONE, 1);
        else if (teq(&t[0], "fixed")) emit(dl, P_POS, f, U_NONE, 2);
        else if (teq(&t[0], "static") || teq(&t[0], "relative") || teq(&t[0], "sticky")) emit(dl, P_POS, f, U_NONE, 0);
    } else if (!strcmp(nm, "overflow") || !strcmp(nm, "overflow-x") || !strcmp(nm, "overflow-y")) {
        if (teq(&t[0], "hidden") || teq(&t[0], "clip")) emit(dl, P_OVERFLOW, f, U_NONE, 1);
        else emit(dl, P_OVERFLOW, f, U_NONE, 0);
    } else if (prop == P_TT) {
        if (teq(&t[0], "uppercase")) emit(dl, P_TT, f, U_NONE, CSS_TT_UPPER);
        else if (teq(&t[0], "lowercase")) emit(dl, P_TT, f, U_NONE, CSS_TT_LOWER);
        else if (teq(&t[0], "capitalize")) emit(dl, P_TT, f, U_NONE, CSS_TT_CAPITALIZE);
        else if (teq(&t[0], "none")) emit(dl, P_TT, f, U_NONE, CSS_TT_NONE);
    }
}

// Parses a declaration block body (comments already removed). Returns number of decls.
static int parse_decls(const char *s, size_t n, struct css_decl *out, int cap) {
    struct dlist dl = { out, 0, cap };
    size_t i = 0;
    while (i < n && dl.n < cap) {
        while (i < n && (is_ws(s[i]) || s[i] == ';')) i++;
        if (i >= n) break;
        size_t j = i; int pd = 0, bd = 0;
        while (j < n) {
            char c = s[j];
            if (c == '"' || c == '\'') { j = skip_string(s, j, n); continue; }
            if (c == '(') pd++;
            else if (c == ')') { if (pd) pd--; }
            else if (c == '{') bd++;
            else if (c == '}') { if (bd) bd--; else break; }
            else if (c == ';' && !pd && !bd) break;
            j++;
        }
        size_t e = j;
        size_t colon = i;
        while (colon < e && s[colon] != ':') colon++;
        if (colon < e) {
            size_t ns = i, ne = colon;
            while (ne > ns && is_ws(s[ne - 1])) ne--;
            size_t vs = colon + 1, ve = e;
            while (vs < ve && is_ws(s[vs])) vs++;
            while (ve > vs && is_ws(s[ve - 1])) ve--;
            int imp = 0;
            if (ve - vs >= 9) {
                char tmp[10];
                for (int q = 0; q < 9; q++) tmp[q] = lc(s[ve - 9 + (size_t)q]);
                tmp[9] = 0;
                if (!strcmp(tmp, "important")) {
                    size_t k = ve - 9;
                    while (k > vs && is_ws(s[k - 1])) k--;
                    if (k > vs && s[k - 1] == '!') { imp = 1; ve = k - 1; while (ve > vs && is_ws(s[ve - 1])) ve--; }
                }
            }
            char nm[32], val[256];
            if (ne - ns > 0 && ne - ns < sizeof nm && ve - vs > 0 && ve - vs < sizeof val) {
                for (size_t q = 0; q < ne - ns; q++) nm[q] = lc(s[ns + q]);
                nm[ne - ns] = 0;
                for (size_t q = 0; q < ve - vs; q++) val[q] = lc(s[vs + q]);
                val[ve - vs] = 0;
                apply_prop(&dl, nm, val, ve - vs, imp);
            }
        }
        i = j < n ? j + 1 : n;
    }
    return dl.n;
}

// ---------------------------------------------------------------- selectors
static uint16_t add_text(struct css_sheet *sh, const char *s, size_t n, bool lower) {
    if (n == 0 || n > 100 || sh->text_len + n + 1 > 65535) return 0;
    if (!grow((void **)&sh->text, &sh->text_cap, 1, sh->text_len + n + 1, CSS_MAX_TEXT)) return 0;
    for (size_t i = 0; i < n; i++) sh->text[sh->text_len + i] = lower ? lc(s[i]) : s[i];
    sh->text[sh->text_len + n] = 0;
    uint16_t off = (uint16_t)sh->text_len;
    sh->text_len += n + 1;
    return off;
}

static bool parse_selector(struct css_sheet *sh, const char *s, size_t n, struct crule *r) {
    while (n && is_ws(s[n - 1])) n--;
    while (n && is_ws(*s)) { s++; n--; }
    if (!n) return false;
    memset(r, 0, sizeof *r);
    struct cpart cur; memset(&cur, 0, sizeof cur);
    int ids = 0, cls = 0, tags = 0, ncls = 0;
    bool have = false, gt = false;
    int next_comb = 0;
    size_t i = 0;
    for (;;) {
        char c = i < n ? s[i] : 0;
        if (i >= n || is_ws(c) || c == '>') {
            if (have) {
                if (r->nparts >= CSS_MAX_PARTS) return false;
                cur.comb = (uint8_t)next_comb;
                r->parts[r->nparts++] = cur;
                memset(&cur, 0, sizeof cur);
                have = false; ncls = 0; next_comb = 0; gt = false;
            }
            if (i >= n) break;
            if (c == '>') {
                if (r->nparts == 0 || gt) return false;
                gt = true; next_comb = 1;
            }
            i++;
            continue;
        }
        size_t st;
        if (c == '*') {
            if (have) return false;
            have = true; i++;
        } else if (c == '.' || c == '#') {
            i++; st = i;
            while (i < n && is_ident(s[i])) i++;
            uint16_t off = add_text(sh, s + st, i - st, false);
            if (!off) return false;
            if (c == '.') { if (ncls >= 3) return false; cur.cls[ncls++] = off; cls++; }
            else { if (cur.id) return false; cur.id = off; ids++; }
            have = true;
        } else if (c == ':') {
            if (i + 1 < n && s[i + 1] == ':') return false;
            i++; st = i;
            while (i < n && is_ident(s[i])) i++;
            size_t l = i - st;
            if ((l == 4 && !strncmp(s + st, "link", 4)) || (l == 7 && !strncmp(s + st, "visited", 7))) cls++;
            else return false;
            have = true;
        } else if (is_ident(c)) {
            if (have) return false;
            st = i;
            while (i < n && is_ident(s[i])) i++;
            uint16_t off = add_text(sh, s + st, i - st, true);
            if (!off) return false;
            cur.tag = off; tags++; have = true;
        } else return false;
    }
    if (r->nparts == 0 || gt) return false;
    if (ids > 255) ids = 255;
    if (cls > 255) cls = 255;
    r->spec = ((uint32_t)ids << 16) | ((uint32_t)cls << 8) | (uint32_t)tags;
    return true;
}

static size_t skip_block(const char *s, size_t i, size_t n) {  // s[i] == '{'; returns after matching '}'
    int depth = 0;
    while (i < n) {
        char c = s[i];
        if (c == '"' || c == '\'') { i = skip_string(s, i, n); continue; }
        if (c == '{') depth++;
        else if (c == '}') { if (--depth <= 0) return i + 1; }
        i++;
    }
    return n;
}

static void add_rules(struct css_sheet *sh, const char *sel, size_t sl, const struct css_decl *d, int nd) {
    bool stored = false;
    uint32_t dstart = 0;
    size_t a = 0;
    while (a <= sl && sh->nrules < CSS_MAX_RULES) {
        size_t b = a;
        while (b < sl && sel[b] != ',') b++;
        struct crule r;
        if (parse_selector(sh, sel + a, b - a, &r)) {
            if (!stored) {
                if (!grow((void **)&sh->decls, &sh->decls_cap, sizeof(struct css_decl), sh->ndecls + (size_t)nd, MAX_DECL_POOL)) return;
                memcpy(sh->decls + sh->ndecls, d, (size_t)nd * sizeof *d);
                dstart = (uint32_t)sh->ndecls;
                sh->ndecls += (size_t)nd;
                stored = true;
            }
            if (!grow((void **)&sh->rules, &sh->rules_cap, sizeof(struct crule), sh->nrules + 1, CSS_MAX_RULES)) return;
            r.dstart = dstart; r.dcount = (uint8_t)nd; r.order = (uint16_t)sh->nrules;
            sh->rules[sh->nrules++] = r;
        }
        a = b + 1;
    }
}

static void parse_sheet(struct css_sheet *sh, const char *s, size_t n) {
    size_t i = 0;
    while (i < n && sh->nrules < CSS_MAX_RULES) {
        while (i < n && is_ws(s[i])) i++;
        if (i >= n) break;
        if (s[i] == '}' || s[i] == ';') { i++; continue; }
        if (s[i] == '@') {  // at-rules (@media, @font-face, @import...) are skipped as a whole
            i++;
            while (i < n && s[i] != ';' && s[i] != '{') {
                if (s[i] == '"' || s[i] == '\'') i = skip_string(s, i, n); else i++;
            }
            if (i < n && s[i] == ';') i++;
            else if (i < n) i = skip_block(s, i, n);
            continue;
        }
        size_t ps = i;
        while (i < n && s[i] != '{') {
            if (s[i] == '"' || s[i] == '\'') { i = skip_string(s, i, n); continue; }
            if (s[i] == ';' || s[i] == '}') ps = i + 1;
            i++;
        }
        if (i >= n) break;
        size_t pe = i;
        size_t bs = i + 1;
        size_t after = skip_block(s, i, n);
        size_t be = after;
        if (after > bs && s[after - 1] == '}') be = after - 1;
        i = after;
        if (ps >= pe) continue;
        struct css_decl d[CSS_MAX_DECLS];
        int nd = parse_decls(s + bs, be > bs ? be - bs : 0, d, CSS_MAX_DECLS);
        if (nd > 0) add_rules(sh, s + ps, pe - ps, d, nd);
    }
}

void css_parse(struct css_sheet *sh, const char *css, size_t len) {
    if (!sh || !css || !len || sh->consumed >= CSS_MAX_INPUT) return;
    if (len > CSS_MAX_INPUT - sh->consumed) len = CSS_MAX_INPUT - sh->consumed;
    sh->consumed += len;
    char *buf = kmalloc(len + 1);
    if (!buf) return;
    size_t n = css_clean(css, len, buf);
    buf[n] = 0;
    parse_sheet(sh, buf, n);
    kfree(buf);
}

// ---------------------------------------------------------------- matching & cascade
static bool has_class(const char *list, const char *name) {
    if (!list) return false;
    size_t nl = strlen(name);
    while (*list) {
        while (*list && is_ws(*list)) list++;
        const char *s = list;
        while (*list && !is_ws(*list)) list++;
        if ((size_t)(list - s) == nl && !memcmp(s, name, nl)) return true;
    }
    return false;
}

static bool match_compound(const struct css_sheet *sh, const struct cpart *p, const struct css_elem *e) {
    if (p->tag && (!e->tag || strcasecmp(sh->text + p->tag, e->tag))) return false;
    if (p->id && (!e->id || strcmp(sh->text + p->id, e->id))) return false;
    for (int k = 0; k < 3 && p->cls[k]; k++)
        if (!has_class(e->classes, sh->text + p->cls[k])) return false;
    return true;
}

static bool match_from(const struct css_sheet *sh, const struct crule *r, int idx, const struct css_elem *e, int *budget) {
    if (--*budget < 0) return false;
    if (!match_compound(sh, &r->parts[idx], e)) return false;
    if (idx == 0) return true;
    const struct css_elem *a = e->parent;
    if (r->parts[idx].comb) return a && match_from(sh, r, idx - 1, a, budget);
    for (int d = 0; a && d < 32; d++, a = a->parent) {
        if (match_from(sh, r, idx - 1, a, budget)) return true;
        if (*budget < 0) return false;
    }
    return false;
}

struct win { uint64_t key; struct css_decl d; uint8_t set; };

static void offer(struct win *w, const struct css_decl *d, uint64_t spec_order) {
    if (d->prop >= P_COUNT) return;
    uint64_t key = ((uint64_t)(d->flags & DF_IMP) << 48) | spec_order;
    struct win *x = &w[d->prop];
    if (!x->set || key >= x->key) { x->key = key; x->d = *d; x->set = 1; }
}

static int bucket(int px) { return px <= 12 ? 0 : px <= 17 ? 1 : px <= 23 ? 2 : 3; }
static int px_of(const struct css_decl *d, int font) {
    int64_t v = d->unit == U_EM ? (int64_t)font * d->val / 100 : d->val / 100;
    return v > 100000 ? 100000 : v < -100000 ? -100000 : (int)v;
}
static uint8_t clamp_sp(int v) { return v < 0 ? 0 : v > 64 ? 64 : (uint8_t)v; }

void css_inherit(const struct css_style *p, struct css_style *c) {
    if (!p || !c) return;
    uint32_t ph = p->has;
    if (!(c->has & CSS_H_VISIBILITY) && (ph & CSS_H_VISIBILITY)) { c->hidden = p->hidden; c->has |= CSS_H_VISIBILITY; }
    if (!(c->has & CSS_H_COLOR) && (ph & CSS_H_COLOR)) { c->color = p->color; c->has |= CSS_H_COLOR; }
    if (!(c->has & CSS_H_BOLD) && (ph & CSS_H_BOLD)) { c->bold = p->bold; c->has |= CSS_H_BOLD; }
    if (!(c->has & CSS_H_ITALIC) && (ph & CSS_H_ITALIC)) { c->italic = p->italic; c->has |= CSS_H_ITALIC; }
    if (!(c->has & CSS_H_ALIGN) && (ph & CSS_H_ALIGN)) { c->align = p->align; c->has |= CSS_H_ALIGN; }
    if (!(c->has & CSS_H_WHITESPACE) && (ph & CSS_H_WHITESPACE)) { c->white_pre = p->white_pre; c->has |= CSS_H_WHITESPACE; }
    if (!(c->has & CSS_H_TRANSFORM) && (ph & CSS_H_TRANSFORM)) { c->text_transform = p->text_transform; c->has |= CSS_H_TRANSFORM; }
    if (!(c->has & CSS_H_LISTSTYLE) && (ph & CSS_H_LISTSTYLE)) { c->list_none = p->list_none; c->has |= CSS_H_LISTSTYLE; }
    if (!(c->has & CSS_H_FONTSIZE) && p->font_px) {
        c->font_px = p->font_px; c->font_bucket = p->font_bucket;
        if (ph & CSS_H_FONTSIZE) c->has |= CSS_H_FONTSIZE;
    }
}

void css_compute(const struct css_sheet *sh, const struct css_elem *el, const char *inline_style,
                 const struct css_style *parent, struct css_style *out) {
    struct win w[P_COUNT];
    memset(w, 0, sizeof w);
    memset(out, 0, sizeof *out);
    if (sh && el) {
        for (size_t i = 0; i < sh->nrules; i++) {
            const struct crule *r = &sh->rules[i];
            int budget = 200;
            if (!match_from(sh, r, r->nparts - 1, el, &budget)) continue;
            uint64_t so = ((uint64_t)r->spec << 16) | r->order;
            for (int k = 0; k < r->dcount; k++) offer(w, &sh->decls[r->dstart + (uint32_t)k], so);
        }
    }
    if (inline_style && *inline_style) {
        char buf[1024];
        size_t l = strlen(inline_style);
        if (l > sizeof buf - 1) l = sizeof buf - 1;
        size_t n = css_clean(inline_style, l, buf);
        struct css_decl d[CSS_MAX_DECLS];
        int nd = parse_decls(buf, n, d, CSS_MAX_DECLS);
        for (int k = 0; k < nd; k++) offer(w, &d[k], (INLINE_SPEC << 16) | (uint64_t)k);
    }

    int pfont = parent && parent->font_px ? parent->font_px : 16;
    int font = pfont;
    if (w[P_FSIZE].set && !(w[P_FSIZE].d.flags & DF_INHERIT)) {
        const struct css_decl *d = &w[P_FSIZE].d;
        int64_t v = d->unit == U_PX ? d->val / 100 : d->unit == U_EM ? ((int64_t)pfont * d->val + 50) / 100
                                                                    : ((int64_t)pfont * d->val + 5000) / 10000;
        font = v < 1 ? 1 : v > 200 ? 200 : (int)v;
        out->has |= CSS_H_FONTSIZE;
    }
    out->font_px = (uint8_t)font;
    out->font_bucket = (uint8_t)bucket(font);

    for (int p = 0; p < P_COUNT; p++) {
        if (!w[p].set || (w[p].d.flags & DF_INHERIT) || p == P_FSIZE) continue;
        const struct css_decl *d = &w[p].d;
        int32_t v = d->val;
        uint32_t bit = 1u << p;
        switch (p) {
        case P_DISPLAY: out->display = (uint8_t)v; out->has |= bit; break;
        case P_VISIBILITY: out->hidden = (uint8_t)v; out->has |= bit; break;
        case P_COLOR: out->color = (uint32_t)v; out->has |= bit; break;
        case P_BG: if (v >= 0) { out->bg = (uint32_t)v; out->has |= bit; } break;
        case P_BOLD: out->bold = (uint8_t)v; out->has |= bit; break;
        case P_ITALIC: out->italic = (uint8_t)v; out->has |= bit; break;
        case P_ALIGN: out->align = (uint8_t)v; out->has |= bit; break;
        case P_DECO: out->deco = (uint8_t)v; out->has |= bit; break;
        case P_WS: out->white_pre = (uint8_t)v; out->has |= bit; break;
        case P_MT: out->mt = clamp_sp(px_of(d, font)); out->has |= bit; break;
        case P_MB: out->mb = clamp_sp(px_of(d, font)); out->has |= bit; break;
        case P_PT: out->pt = clamp_sp(px_of(d, font)); out->has |= bit; break;
        case P_PB: out->pb = clamp_sp(px_of(d, font)); out->has |= bit; break;
        case P_WIDTH: case P_MAXW: {
            if (d->unit == U_NONE) break;
            uint16_t *dst = p == P_WIDTH ? &out->width : &out->maxw;
            uint8_t *pc = p == P_WIDTH ? &out->width_pct : &out->maxw_pct;
            int x;
            if (d->unit == U_PCT) { x = v / 100; if (x > 100) x = 100; *pc = 1; }
            else { x = px_of(d, font); if (x > 4096) x = 4096; *pc = 0; }
            if (x < 0) x = 0;
            *dst = (uint16_t)x; out->has |= bit;
            break;
        }
        case P_LIST: out->list_none = (uint8_t)v; out->has |= bit; break;
        case P_FLOAT: out->float_ = (uint8_t)v; out->has |= bit; break;
        case P_POS: out->position_absolute = v == 1; out->position_fixed = v == 2; out->has |= bit; break;
        case P_OVERFLOW: out->overflow_hidden = (uint8_t)v; out->has |= bit; break;
        case P_TT: out->text_transform = (uint8_t)v; out->has |= bit; break;
        default: break;
        }
    }
    if (parent) css_inherit(parent, out);
}
