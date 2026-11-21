// Tiny tolerant HTML tokenizer + streaming layout (no CSS, no images, no JS).
#include "html.h"
#include "../gfx/font.h"
#include "../wm/theme.h"
#include "string.h"
#include "mm.h"

#define MAX_RUNS  9000
#define POOL_SZ   (160 * 1024)
#define MAX_LINES 2000

enum { F_UI, F_BOLD, F_TITLE, F_MONO, F_MED, F_SMALL };
enum { K_TEXT, K_HR, K_CTL };
#define CTL_H   24
#define CTL_ASC 17

static const struct afont *afont_of(int f) {
    switch (f) {
    case F_BOLD: return &afont_ui_bold;
    case F_TITLE: return &afont_title;
    case F_MED: return &afont_ui_med;
    case F_SMALL: return &afont_small;
    default: return &afont_ui;
    }
}
static int f_height(int f) { return f == F_MONO ? 16 : afont_of(f)->height; }
static int f_ascent(int f) { return f == F_MONO ? 13 : afont_of(f)->ascent; }
static int f_width(int f, const char *s) {
    return f == F_MONO ? font_width(&font_8x16, s) : text_width(afont_of(f), s);
}

struct lay {
    struct hdoc *d;
    const char *end;
    int width, lm, x, y, gap;
    int maxasc, maxdesc, first, nlines;
    bool has, space, stop, skip_nl;
    bool no_head_end;               // searched once: no </head anywhere
    bool trunc;                     // a hard limit (runs/pool) was hit
    bool reserve;                   // writing the truncation notice: use the reserved space
    int bold, ital, mono, pre, hf, quote, link;
    int depth, ul_over, q_over, cur_form;
    bool lord[8];
    int lnum[8];
    char wb[120]; int wl;
    char pb[200]; int pl, pcap;
};

static int imax(int a, int b) { return a > b ? a : b; }

static void end_line(struct lay *L) {
    if (!L->has) return;
    for (int i = L->first; i < L->d->nruns; i++) {
        struct hrun *r = &L->d->runs[i];
        if (r->kind == K_TEXT) r->y = L->y + (L->maxasc - f_ascent(r->font));
        else if (r->kind == K_CTL) r->y = L->y + (L->maxasc - CTL_ASC);
    }
    L->y += L->maxasc + L->maxdesc + 3;
    L->x = L->lm;
    L->has = false; L->space = false;
    L->maxasc = L->maxdesc = 0;
    L->first = L->d->nruns;
    if (++L->nlines >= MAX_LINES) L->stop = true;
}

static void begin_content(struct lay *L) {
    if (L->has) return;
    L->y += L->gap; L->gap = 0;
    L->has = true;
    L->x = L->lm;
    L->first = L->d->nruns;
}

static void block_break(struct lay *L, int g) {
    end_line(L);
    if (g > L->gap) L->gap = g;
}

static void add_run(struct lay *L, const char *t, int len, int font, color_t c, int x, int kind, int w) {
    struct hdoc *d = L->d;
    int rmax = L->reserve ? MAX_RUNS : MAX_RUNS - 4, pmax = L->reserve ? POOL_SZ : POOL_SZ - 64;
    if (d->nruns >= rmax || d->pool_len + len + 1 > pmax) { L->stop = true; L->trunc = true; return; }
    if (x < 0) x = 0;
    if (x > 30000) x = 30000;
    if (w < 0) w = 0;
    if (w > 30000) w = 30000;
    struct hrun *r = &d->runs[d->nruns++];
    r->x = (int16_t)x; r->w = (int16_t)w; r->y = L->y;
    r->off = (uint32_t)d->pool_len;
    r->color = c; r->link = (int16_t)(kind == K_TEXT ? L->link : -1); r->ctl = -1;
    r->font = (uint8_t)font; r->kind = (uint8_t)kind;
    r->ul = (uint8_t)(kind == K_TEXT && L->link >= 0);
    memcpy(d->pool + d->pool_len, t, (size_t)len);
    d->pool[d->pool_len + len] = 0;
    d->pool_len += len + 1;
    if (kind == K_TEXT) {
        L->maxasc = imax(L->maxasc, f_ascent(font));
        L->maxdesc = imax(L->maxdesc, f_height(font) - f_ascent(font));
    } else if (kind == K_CTL) {
        L->maxasc = imax(L->maxasc, CTL_ASC);
        L->maxdesc = imax(L->maxdesc, CTL_H - CTL_ASC);
    }
}

static int cur_font(const struct lay *L) {
    if (L->pre || L->mono > 0) return F_MONO;
    if (L->hf >= 0) return L->hf;
    if (L->bold > 0) return F_BOLD;
    if (L->ital > 0) return F_MED;
    return F_UI;
}
static color_t cur_color(const struct lay *L) {
    if (L->link >= 0) return C_ACCENT;
    if (L->mono > 0 || L->pre) return C_TEXT_2;
    if (L->quote > 0 && L->hf < 0) return C_TEXT_2;
    return C_TEXT;
}

static void put_word(struct lay *L) {
    if (L->wl == 0 || L->stop) { L->wl = 0; return; }
    int font = cur_font(L);
    color_t col = cur_color(L);
    int sp = (L->has && L->space) ? f_width(font, " ") : 0;
    while (L->wl > 0 && !L->stop) {
        L->wb[L->wl] = 0;
        int ww = f_width(font, L->wb);
        if (L->has && L->x + sp + ww > L->width) { end_line(L); sp = 0; }
        begin_content(L);
        if (L->x + sp + ww > L->width) {
            int n = 0, acc = 0;
            while (n < L->wl) {
                char ch[2] = { L->wb[n], 0 };
                int cw = f_width(font, ch);
                if (n > 0 && L->x + acc + cw > L->width) break;
                acc += cw; n++;
            }
            add_run(L, L->wb, n, font, col, L->x, K_TEXT, acc);
            end_line(L);
            memmove(L->wb, L->wb + n, (size_t)(L->wl - n));
            L->wl -= n; sp = 0;
        } else {
            if (sp && L->link >= 0 && L->d->nruns > 0) {      // continuous link underline
                struct hrun *pr = &L->d->runs[L->d->nruns - 1];
                if (pr->kind == K_TEXT && pr->link == L->link && pr->y >= 0) pr->w = (int16_t)(pr->w + sp);
            }
            L->x += sp;
            add_run(L, L->wb, L->wl, font, col, L->x, K_TEXT, ww);
            L->x += ww;
            L->wl = 0;
        }
    }
    L->wl = 0; L->space = false;
}

static void pre_flush(struct lay *L) {
    if (L->pl == 0 || L->stop) { L->pl = 0; return; }
    L->pb[L->pl] = 0;
    begin_content(L);
    int w = f_width(F_MONO, L->pb);
    if (L->x + w > 20000) { end_line(L); begin_content(L); }
    add_run(L, L->pb, L->pl, F_MONO, cur_color(L), L->x, K_TEXT, w);
    L->x += w;
    L->pl = 0;
}

static void pre_char(struct lay *L, char c) {
    if (c == '\r') return;
    if (c == '\n') {
        if (L->skip_nl) { L->skip_nl = false; return; }
        if (L->pl) { pre_flush(L); end_line(L); }
        else if (L->has) end_line(L);
        else { L->y += L->gap + 16; L->gap = 0; if (++L->nlines >= MAX_LINES) L->stop = true; }
        return;
    }
    L->skip_nl = false;
    if (c == '\t') { for (int i = 0; i < 4; i++) pre_char(L, ' '); return; }
    if (L->pl >= L->pcap) { pre_flush(L); end_line(L); }
    L->pb[L->pl++] = c;
}

static void feed(struct lay *L, char c) {
    if (L->wl >= (int)sizeof L->wb - 2) put_word(L);
    L->wb[L->wl++] = c;
}

// Font slot for a code point >= 0x7f: U+00A0..U+00FF map 1:1, typographic extras live at 0x80..0x8a.
static char cp_slot(unsigned cp) {
    switch (cp) {
    case 0xa0: return ' ';
    case 0xb7: return 0x7f;
    case 0x2022: return (char)0x81;
    case 0x2014: return (char)0x82;
    case 0x20ac: return (char)0x83;
    case 0x2013: return (char)0x84;
    case 0x2026: return (char)0x85;
    case 0x2018: return (char)0x86;
    case 0x2019: return (char)0x87;
    case 0x201c: return (char)0x88;
    case 0x201d: return (char)0x89;
    case 0x2122: return (char)0x8a;
    default: return (cp > 0xa0 && cp <= 0xff) ? (char)cp : '?';
    }
}

static void emit_cp(struct lay *L, unsigned cp) {
    if (cp == 0xa0) cp = ' ';
    if (cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t') {
        if (L->pre) pre_char(L, (char)cp);
        else { put_word(L); L->space = true; }
        return;
    }
    if (cp < 0x20 || cp == 0x7f) return;
    char one[2] = { cp < 0x7f ? (char)cp : cp_slot(cp), 0 };
    const char *s = one;
    for (; *s; s++) {
        if (L->pre) pre_char(L, *s);
        else feed(L, *s);
    }
}

// p points at '&'. Returns bytes consumed (0 = not an entity) and the code point.
// HTML 4 names of U+00A0..U+00FF, in order.
static const char *const latin1_names[96] = {
    "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect",
    "uml", "copy", "ordf", "laquo", "not", "shy", "reg", "macr",
    "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot",
    "cedil", "sup1", "ordm", "raquo", "frac14", "frac12", "frac34", "iquest",
    "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil",
    "Egrave", "Eacute", "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml",
    "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times",
    "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig",
    "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil",
    "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml",
    "eth", "ntilde", "ograve", "oacute", "ocirc", "otilde", "ouml", "divide",
    "oslash", "ugrave", "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml",
};

static int entity(const char *p, const char *end, unsigned *cp) {
    const char *q = p + 1;
    if (q >= end) return 0;
    if (*q == '#') {
        q++;
        bool hex = q < end && (*q == 'x' || *q == 'X');
        if (hex) q++;
        unsigned v = 0; int nd = 0;
        for (; q < end && nd < 8; q++, nd++) {
            int c = *q, dv;
            if (c >= '0' && c <= '9') dv = c - '0';
            else if (hex && c >= 'a' && c <= 'f') dv = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') dv = c - 'A' + 10;
            else break;
            v = v * (hex ? 16 : 10) + (unsigned)dv;
            if (v > 0x10ffff) v = 0x10ffff;
        }
        if (nd == 0) return 0;
        if (q < end && *q == ';') q++;
        *cp = v;
        return (int)(q - p);
    }
    char name[10]; int n = 0;
    while (q < end && n < 9 && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9')))
        name[n++] = *q++;
    name[n] = 0;
    if (n == 0 || q >= end || *q != ';') return 0;
    q++;
    static const struct { const char *n; unsigned cp; } tab[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
        {"mdash", 0x2014}, {"ndash", 0x2013}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
        {"ldquo", 0x201c}, {"rdquo", 0x201d}, {"hellip", 0x2026}, {"copy", 0xa9},
        {"bull", 0x2022}, {"euro", 0x20ac}, {"trade", 0x2122}, {"sbquo", 0x2018},
        {"lsaquo", '<'}, {"rsaquo", '>'},
    };
    *cp = '?';
    for (unsigned i = 0; i < 96; i++)
        if (strcmp(latin1_names[i], name) == 0) { *cp = 0xa0 + i; return (int)(q - p); }
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(tab[i].n, name) == 0) { *cp = tab[i].cp; break; }
    return (int)(q - p);
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Case-insensitive search for needle (lowercase) in [p, end).
static const char *find_ci(const char *p, const char *end, const char *needle) {
    size_t n = strlen(needle);
    for (; p + n <= end; p++) {
        size_t i = 0;
        while (i < n && lc(p[i]) == needle[i]) i++;
        if (i == n) return p;
    }
    return NULL;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static bool get_attr(const char *s, const char *e, const char *name, char *out, int n) {
    const char *p = s;
    while (p < e) {
        while (p < e && (is_ws(*p) || *p == '/')) p++;
        char an[16]; int al = 0;
        while (p < e && *p != '=' && !is_ws(*p) && *p != '/') {
            if (al < 15) an[al++] = lc(*p);
            p++;
        }
        an[al] = 0;
        while (p < e && is_ws(*p)) p++;
        if (p < e && *p == '=') {
            p++;
            while (p < e && is_ws(*p)) p++;
            const char *vs, *ve;
            if (p < e && (*p == '"' || *p == '\'')) {
                char q = *p++;
                vs = p;
                while (p < e && *p != q) p++;
                ve = p;
                if (p < e) p++;
            } else {
                vs = p;
                while (p < e && !is_ws(*p)) p++;
                ve = p;
            }
            if (strcmp(an, name) == 0) {
                int o = 0;
                for (const char *c = vs; c < ve && o < n - 1; c++) {
                    unsigned ecp;
                    int ek = *c == '&' ? entity(c, ve, &ecp) : 0;
                    if (ek) { out[o++] = (ecp >= 0x20 && ecp < 0x7f) ? (char)ecp : (ecp == 0xa0 ? ' ' : '?'); c += ek - 1; }
                    else if (*c != '\n' && *c != '\r' && *c != '\t') out[o++] = *c;
                }
                out[o] = 0;
                return true;
            }
        } else {
            if (al > 0 && strcmp(an, name) == 0) { if (n > 0) out[0] = 0; return true; }   // boolean attribute
            if (al == 0) p++;
        }
    }
    return false;
}

// Links the Browser cannot (or must not) navigate: shown as plain text.
static bool bad_scheme(const char *h) {
    while (*h == ' ') h++;
    static const char *const bad[] = { "javascript:", "mailto:", "tel:", "data:", "vbscript:", "sms:" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        size_t n = strlen(bad[i]), k = 0;
        while (k < n && lc(h[k]) == bad[i][k]) k++;
        if (k == n) return true;
    }
    return false;
}

static bool is(const char *a, const char *b) { return strcmp(a, b) == 0; }

static unsigned utf8_next(const char **pp, const char *end);

// Feeds a UTF-8 string (at most max bytes) into the text flow.
static void emit_str(struct lay *L, const char *t, int max) {
    const char *e = t;
    while (e - t < max && *e) e++;
    while (t < e) emit_cp(L, utf8_next(&t, e));
}

// Small non-negative integer attribute, -1 if absent/garbage (never overflows).
static int attr_int(const char *s, const char *e, const char *name) {
    char b[12];
    if (!get_attr(s, e, name, b, sizeof b) || b[0] < '0' || b[0] > '9') return -1;
    int v = 0;
    for (const char *c = b; *c >= '0' && *c <= '9'; c++) v = v * 10 + (*c - '0');
    return v;
}


// ---- forms ----

// Text between s and end as display bytes (font slots), whitespace collapsed. tags: skip <...>.
static int strip_text(const char *s, const char *end, bool tags, char *out, int n) {
    int o = 0; bool sp = false;
    while (s < end && o < n - 1) {
        if (tags && *s == '<') {
            while (s < end && *s != '>') s++;
            if (s < end) s++;
            continue;
        }
        unsigned cp;
        int k = *s == '&' ? entity(s, end, &cp) : 0;
        if (k) s += k; else cp = utf8_next(&s, end);
        if (cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t' || cp == 0xa0) { sp = o > 0; continue; }
        if (cp < 0x20 || cp == 0x7f) continue;
        if (sp) { out[o++] = ' '; sp = false; if (o >= n - 1) break; }
        out[o++] = cp < 0x7f ? (char)cp : cp_slot(cp);
    }
    out[o] = 0;
    return o;
}

static struct hctl *new_ctl(struct lay *L, int type) {
    struct hdoc *d = L->d;
    if (d->nctl >= HTML_MAX_CTLS) return NULL;
    struct hctl *c = &d->ctl[d->nctl++];
    memset(c, 0, sizeof *c);
    c->type = (uint8_t)type;
    c->form = (int8_t)L->cur_form;
    return c;
}

static int ctl_index(struct lay *L, const struct hctl *c) { return (int)(c - L->d->ctl); }

// Places a control box of width w in the text flow.
static void put_ctl(struct lay *L, struct hctl *c, int w) {
    if (L->stop) return;
    if (w > L->width - L->lm) w = L->width - L->lm;
    if (w < 20) w = 20;
    int sp = (L->has && L->space) ? f_width(F_UI, " ") : 0;
    if (L->has && L->x + sp + w > L->width) { end_line(L); sp = 0; }
    begin_content(L);
    L->x += sp;
    int n0 = L->d->nruns;
    add_run(L, "", 0, F_UI, 0, L->x, K_CTL, w);
    if (L->d->nruns > n0) {
        L->d->runs[n0].ctl = (int8_t)ctl_index(L, c);
        c->vis = true;
        L->x += w;
        L->space = true;
    }
}

static void ctl_common(struct hctl *c, const char *s, const char *e) {
    get_attr(s, e, "name", c->name, sizeof c->name);
    get_attr(s, e, "value", c->value, sizeof c->value);
    get_attr(s, e, "placeholder", c->ph, sizeof c->ph);
    char b[4];
    c->disabled = get_attr(s, e, "disabled", b, sizeof b);
    c->checked = get_attr(s, e, "checked", b, sizeof b);
    int ml = attr_int(s, e, "maxlength");
    c->maxlen = (uint8_t)(ml > 0 && ml < HTML_VAL_LEN ? ml : 0);
    c->len = c->cur = (int16_t)strlen(c->value);
    if (c->maxlen && c->len > c->maxlen) { c->len = c->cur = c->maxlen; c->value[c->len] = 0; }
}

static int box_w(int size, int def) {
    if (size <= 0 || size > 80) size = def;
    return size * 8 + 16;
}

static void do_input(struct lay *L, const char *s, const char *e) {
    char ty[16] = "";
    get_attr(s, e, "type", ty, sizeof ty);
    for (char *c = ty; *c; c++) *c = lc(*c);
    int t = HC_TEXT;
    if (is(ty, "hidden")) t = HC_HIDDEN;
    else if (is(ty, "checkbox")) t = HC_CHECK;
    else if (is(ty, "radio")) t = HC_RADIO;
    else if (is(ty, "submit") || is(ty, "image")) t = HC_SUBMIT;
    else if (is(ty, "button") || is(ty, "reset")) t = HC_BUTTON;
    else if (is(ty, "password")) t = HC_PASSWORD;
    struct hctl *c = new_ctl(L, t);
    if (!c) {                                   // too many controls: plain text fallback
        char v[64] = "";
        if (t == HC_HIDDEN) return;
        if (t == HC_CHECK || t == HC_RADIO) { emit_str(L, "[ ]", 3); return; }
        if (!get_attr(s, e, "value", v, sizeof v) || !v[0]) {
            if (t == HC_SUBMIT || t == HC_BUTTON) strcpy(v, "Submit");
            else if (!get_attr(s, e, "placeholder", v, sizeof v)) v[0] = 0;
        }
        emit_str(L, "[ ", 2); emit_str(L, v[0] ? v : "______", 40); emit_str(L, " ]", 2);
        return;
    }
    ctl_common(c, s, e);
    if (t == HC_HIDDEN) return;
    if (t == HC_CHECK || t == HC_RADIO) {
        if (!c->value[0]) { strcpy(c->value, "on"); c->len = c->cur = 2; }
        put_ctl(L, c, 16);
    } else if (t == HC_SUBMIT || t == HC_BUTTON) {
        const char *lab = c->value[0] ? c->value : (is(ty, "reset") ? "Reset" : "Submit");
        put_ctl(L, c, f_width(F_UI, lab) + 24);
    } else {
        put_ctl(L, c, box_w(attr_int(s, e, "size"), 24));
    }
}

static void do_button(struct lay *L, const char *s, const char *e, const char *lab_s, const char *lab_e) {
    char ty[16] = "submit";
    get_attr(s, e, "type", ty, sizeof ty);
    for (char *c = ty; *c; c++) *c = lc(*c);
    struct hctl *c = new_ctl(L, is(ty, "button") || is(ty, "reset") ? HC_BUTTON : HC_SUBMIT);
    if (!c) return;
    ctl_common(c, s, e);
    c->ph[0] = 0;
    strip_text(lab_s, lab_e, true, c->ph, sizeof c->ph);
    if (!c->ph[0]) strcpy(c->ph, c->value[0] ? c->value : "Submit");
    put_ctl(L, c, f_width(F_UI, c->ph) + 24);
}

// q points at the closing </select> or </textarea>.
static void do_select(struct lay *L, const char *s, const char *e, const char *q) {
    struct hdoc *d = L->d;
    struct hctl *c = new_ctl(L, HC_SELECT);
    if (!c) { emit_str(L, "[ select ]", 12); return; }
    ctl_common(c, s, e);
    c->value[0] = 0; c->len = c->cur = 0;
    c->opt_first = (uint16_t)d->nopts;
    int maxw = 0;
    const char *p = e + 1;
    while (p < q && c->nopt < HTML_SEL_OPTS && d->nopts < HTML_MAX_OPTS) {
        const char *o = find_ci(p, q, "<option");
        if (!o) break;
        const char *te = o + 7;
        while (te < q && *te != '>') te++;
        if (te >= q) break;
        const char *le = te + 1;
        while (le < q && *le != '<') le++;
        struct hopt *op = &d->opts[d->nopts];
        strip_text(te + 1, le, false, op->label, sizeof op->label);
        if (!get_attr(o + 7, te, "value", op->value, sizeof op->value)) strcpy(op->value, op->label);
        char b[4];
        if (get_attr(o + 7, te, "selected", b, sizeof b)) c->sel = c->nopt;
        maxw = imax(maxw, f_width(F_UI, op->label));
        c->nopt++; d->nopts++;
        p = te + 1;
    }
    put_ctl(L, c, imax(maxw, 40) + 36);
}

static void do_textarea(struct lay *L, const char *s, const char *e, const char *q) {
    struct hctl *c = new_ctl(L, HC_TEXTAREA);
    if (!c) { emit_str(L, "[ text ]", 12); return; }
    ctl_common(c, s, e);
    const char *b = e + 1;
    if (b < q && *b == '\n') b++;
    c->len = c->cur = (int16_t)strip_text(b, q, false, c->value, HTML_VAL_LEN);
    int cols = attr_int(s, e, "cols");
    put_ctl(L, c, box_w(cols, 30));
}

static void heading(struct lay *L, int level, bool open) {
    if (open) {
        block_break(L, level == 1 ? 14 : 12);
        L->hf = level <= 2 ? F_TITLE : F_BOLD;
    } else {
        L->hf = -1;
        block_break(L, 8);
    }
}

static void adj(int *v, bool closing) { *v += closing ? (*v > 0 ? -1 : 0) : 1; }

// Handles one tag; s..e is the text between '<' and '>'. Returns the next
// input position or NULL for "after the tag".
static const char *handle_tag(struct lay *L, const char *s, const char *e) {
    bool closing = false;
    if (s < e && *s == '/') { closing = true; s++; }
    char nm[16]; int n = 0;
    while (s < e && ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')) && n < 15)
        nm[n++] = lc(*s++);
    nm[n] = 0;
    if (n == 0) return NULL;
    if (!L->pre) put_word(L); else pre_flush(L);

    if (is(nm, "img")) {
        char alt[80];
        if (!get_attr(s, e, "alt", alt, sizeof alt) || !alt[0]) return NULL;
        int w = attr_int(s, e, "width"), h = attr_int(s, e, "height");
        if (w == 1 || h == 1 || w == 0 || h == 0) return NULL;      // tracking pixel
        emit_str(L, "[", 1); emit_str(L, alt, 60); emit_str(L, "]", 1);
        return NULL;
    }
    if (is(nm, "form")) {
        L->cur_form = -1;
        if (!closing && L->d->nforms < HTML_MAX_FORMS) {
            struct hform *f = &L->d->forms[L->d->nforms];
            f->action[0] = 0;
            get_attr(s, e, "action", f->action, sizeof f->action);
            char m[8] = "";
            get_attr(s, e, "method", m, sizeof m);
            f->post = (uint8_t)(lc(m[0]) == 'p');
            L->cur_form = L->d->nforms++;
        }
        block_break(L, 0);
        return NULL;
    }
    if (is(nm, "input")) { do_input(L, s, e); return NULL; }
    if (is(nm, "button")) {
        if (closing) { emit_str(L, " ]", 2); return NULL; }
        const char *lim = e + 600 < L->end ? e + 600 : L->end;
        const char *q = find_ci(e, lim, "</button");
        if (q && L->d->nctl < HTML_MAX_CTLS) {
            do_button(L, s, e, e + 1, q);
            while (q < L->end && *q != '>') q++;
            return q < L->end ? q + 1 : L->end;
        }
        emit_str(L, "[ ", 2);
        return NULL;
    }
    if (!closing && (is(nm, "select") || is(nm, "textarea"))) {
        bool sel = is(nm, "select");
        const char *q = find_ci(e, L->end, sel ? "</select" : "</textarea");
        if (!q) { L->stop = true; return L->end; }
        if (sel) do_select(L, s, e, q); else do_textarea(L, s, e, q);
        while (q < L->end && *q != '>') q++;
        return q < L->end ? q + 1 : L->end;
    }

    if (is(nm, "script") || is(nm, "style") || is(nm, "title") || is(nm, "head") || is(nm, "noscript") ||
        is(nm, "svg") || is(nm, "template") || is(nm, "select") || is(nm, "textarea") || is(nm, "iframe") ||
        is(nm, "object") || is(nm, "canvas")) {
        if (closing) return NULL;
        char needle[24] = "</";
        strcat(needle, nm);
        if (L->no_head_end && is(nm, "head")) return NULL;
        const char *q = find_ci(e, L->end, needle);
        if (!q) {
            if (!is(nm, "head")) { L->stop = true; return L->end; }
            L->no_head_end = true;       // avoid a quadratic rescan on every later <head>
            return NULL;
        }
        while (q < L->end && *q != '>') q++;
        return q < L->end ? q + 1 : L->end;
    }
    if (nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6' && nm[2] == 0) { heading(L, nm[1] - '0', !closing); return NULL; }
    if (is(nm, "p")) { block_break(L, 10); return NULL; }
    if (is(nm, "br")) {
        if (L->has) end_line(L);
        else { L->y += L->gap + 17; L->gap = 0; }
        return NULL;
    }
    if (is(nm, "div") || is(nm, "section") || is(nm, "article") || is(nm, "header") || is(nm, "footer") ||
        is(nm, "nav") || is(nm, "main") || is(nm, "center") || is(nm, "figure") ||
        is(nm, "address") || is(nm, "fieldset") || is(nm, "details") || is(nm, "summary") ||
        is(nm, "dl") || is(nm, "dt") || is(nm, "dd") || is(nm, "tr") || is(nm, "caption")) {
        block_break(L, is(nm, "dd") || is(nm, "dt") ? 3 : 0);
        return NULL;
    }
    if (is(nm, "ul") || is(nm, "ol")) {
        if (!closing) {
            block_break(L, L->depth == 0 ? 8 : 2);
            if (L->depth < 8) { L->lord[L->depth] = is(nm, "ol"); L->lnum[L->depth] = 0; L->depth++; L->lm += 26; }
            else L->ul_over++;          // too deep: ignored, but balanced on close
        } else {
            block_break(L, L->depth == 1 ? 8 : 2);
            if (L->ul_over > 0) L->ul_over--;
            else if (L->depth > 0) { L->depth--; L->lm -= 26; }
        }
        return NULL;
    }
    if (is(nm, "li")) {
        if (closing) return NULL;
        block_break(L, 3);
        begin_content(L);
        char mk[16] = CH_BULLET;
        if (L->depth > 0 && L->lord[L->depth - 1]) {
            int v = ++L->lnum[L->depth - 1], k = 0;
            char t[12]; int tl = 0;
            do { t[tl++] = (char)('0' + v % 10); v /= 10; } while (v && tl < 10);
            while (tl) mk[k++] = t[--tl];
            mk[k++] = '.'; mk[k] = 0;
        }
        int mx = L->lm - 6 - f_width(F_UI, mk);
        if (mx < 2) mx = 2;
        int saved = L->link;
        L->link = -1;
        add_run(L, mk, (int)strlen(mk), F_UI, C_TEXT_2, mx, K_TEXT, f_width(F_UI, mk));
        L->link = saved;
        return NULL;
    }
    if (is(nm, "a")) {
        if (closing) { L->link = -1; return NULL; }
        char href[HTML_LINK_LEN];
        L->link = -1;
        if (get_attr(s, e, "href", href, sizeof href) && href[0] && !bad_scheme(href) &&
            L->d->nlinks < HTML_MAX_LINKS) {
            strcpy(L->d->links[L->d->nlinks], href);
            L->link = L->d->nlinks++;
        }
        return NULL;
    }
    if (is(nm, "b") || is(nm, "strong")) { adj(&L->bold, closing); return NULL; }
    if (is(nm, "i") || is(nm, "em") || is(nm, "cite") || is(nm, "u")) { adj(&L->ital, closing); return NULL; }
    if (is(nm, "code") || is(nm, "tt") || is(nm, "kbd") || is(nm, "samp")) { adj(&L->mono, closing); return NULL; }
    if (is(nm, "pre")) {
        block_break(L, 8);
        L->pre = closing ? 0 : 1;
        L->skip_nl = !closing;
        L->pl = 0;
        return NULL;
    }
    if (is(nm, "blockquote")) {
        block_break(L, 8);
        if (!closing) {
            if (L->quote < 6) { L->lm += 28; L->quote++; }
            else L->q_over++;
        } else if (L->q_over > 0) L->q_over--;
        else if (L->quote > 0) { L->lm -= 28; L->quote--; }
        return NULL;
    }
    if (is(nm, "hr")) {
        block_break(L, 6);
        begin_content(L);
        add_run(L, "", 0, F_UI, 0, L->lm, K_HR, imax(L->width - L->lm, 0));
        if (L->d->nruns > 0) L->d->runs[L->d->nruns - 1].y = L->y + 4;
        L->maxasc = 8; L->maxdesc = 1;
        end_line(L);
        L->gap = 6;
        return NULL;
    }
    if (is(nm, "table")) { block_break(L, 8); return NULL; }
    if (is(nm, "td") || is(nm, "th")) {
        if (is(nm, "th")) adj(&L->bold, closing);
        if (!closing && L->has) { if (L->x < L->width) L->x += 20; L->space = false; }
        return NULL;
    }
    return NULL;
}

struct hdoc *html_new(void) {
    struct hdoc *d = kmalloc(sizeof *d);
    if (!d) return NULL;
    memset(d, 0, sizeof *d);
    d->runs = kmalloc(sizeof(struct hrun) * MAX_RUNS);
    d->pool = kmalloc(POOL_SZ);
    d->links = kmalloc((size_t)HTML_MAX_LINKS * HTML_LINK_LEN);
    d->ctl = kmalloc(sizeof(struct hctl) * HTML_MAX_CTLS);
    d->opts = kmalloc(sizeof(struct hopt) * HTML_MAX_OPTS);
    d->focus = -1;
    if (!d->runs || !d->pool || !d->links || !d->ctl || !d->opts) {
        kfree(d->runs); kfree(d->pool); kfree(d->links); kfree(d->ctl); kfree(d->opts); kfree(d);
        return NULL;
    }
    return d;
}

static unsigned utf8_next(const char **pp, const char *end) {
    const unsigned char *p = (const unsigned char *)*pp;
    unsigned c = *p, cp; int extra;
    if (c < 0x80) { *pp += 1; return c; }
    if (c >= 0xf0) { extra = 3; cp = c & 7; }
    else if (c >= 0xe0) { extra = 2; cp = c & 15; }
    else if (c >= 0xc0) { extra = 1; cp = c & 31; }
    else { *pp += 1; return c >= 0xa0 ? c : 0xfffd; }   // stray byte: assume Latin-1
    const char *q = *pp + 1;
    for (int i = 0; i < extra; i++, q++) {
        if (q >= end || (((unsigned char)*q) & 0xc0) != 0x80) {
            *pp += 1;                                       // not UTF-8 after all: lead byte is Latin-1
            return c;
        }
        cp = (cp << 6) | (((unsigned char)*q) & 0x3f);
    }
    *pp = q;
    return cp;
}

void html_free(struct hdoc *d) {
    if (!d) return;
    kfree(d->runs); kfree(d->pool); kfree(d->links); kfree(d->ctl); kfree(d->opts); kfree(d);
}

void html_layout(struct hdoc *d, const char *html, int width, bool plain) {
    if (width < 120) width = 120;
    if (width > 8000) width = 8000;
    d->nruns = 0; d->pool_len = 0; d->nlinks = 0; d->height = 0; d->title[0] = 0;
    d->nforms = 0; d->nctl = 0; d->nopts = 0; d->focus = -1;
    size_t n = 0;
    while (n < HTML_MAX_INPUT && html[n]) n++;
    const char *end = html + n;

    struct lay L;
    memset(&L, 0, sizeof L);
    L.d = d; L.end = end; L.width = width; L.lm = 28; L.x = 28; L.y = 14;
    L.hf = -1; L.link = -1; L.cur_form = -1;
    L.pcap = (width - L.lm) / 8;
    if (L.pcap > 190) L.pcap = 190;
    if (L.pcap < 8) L.pcap = 8;

    const char *p = html;
    if (plain) {
        L.pre = 1; L.skip_nl = false;
        while (p < end && !L.stop) emit_cp(&L, utf8_next(&p, end));
    } else {
        const char *t = find_ci(html, end, "<title");
        if (t) {
            const char *a = t;
            while (a < end && *a != '>') a++;
            const char *z = a < end ? find_ci(a, end, "</title") : NULL;
            if (a < end && z) {
                int o = 0; bool sp = false;
                for (const char *c = a + 1; c < z && o < (int)sizeof d->title - 1;) {
                    unsigned cp;
                    int k = *c == '&' ? entity(c, z, &cp) : 0;
                    if (k) c += k; else cp = utf8_next(&c, z);
                    if (cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t' || cp == 0xa0) { sp = o > 0; continue; }
                    if (cp < 0x20 || cp >= 0x7f) cp = cp >= 0xa0 && cp <= 0xff ? cp : '?';
                    if (sp) { d->title[o++] = ' '; sp = false; if (o >= (int)sizeof d->title - 1) break; }
                    d->title[o++] = (char)cp;
                }
                d->title[o] = 0;
            }
        }
        while (p < end && !L.stop) {
            char c = *p;
            if (c == '<' && p + 1 < end && ((p[1] >= 'a' && p[1] <= 'z') || (p[1] >= 'A' && p[1] <= 'Z') ||
                                           p[1] == '/' || p[1] == '!' || p[1] == '?')) {
                if (p + 4 <= end && memcmp(p, "<!--", 4) == 0) {
                    const char *q = p + 4;
                    while (q + 3 <= end && memcmp(q, "-->", 3) != 0) q++;
                    p = q + 3 <= end ? q + 3 : end;
                    continue;
                }
                const char *q = p + 1; char qt = 0;
                while (q < end) {
                    if (qt) { if (*q == qt) qt = 0; }
                    else if (*q == '"' || *q == '\'') qt = *q;
                    else if (*q == '>') break;
                    q++;
                }
                if (q >= end) break;
                const char *nx = handle_tag(&L, p + 1, q);
                p = nx ? nx : q + 1;
                continue;
            }
            if (c == '&') {
                unsigned cp;
                int k = entity(p, end, &cp);
                if (k) { emit_cp(&L, cp); p += k; continue; }
            }
            emit_cp(&L, utf8_next(&p, end));
        }
    }
    put_word(&L);
    pre_flush(&L);
    end_line(&L);
    if (L.trunc || (L.stop && p < end) || (n == HTML_MAX_INPUT && html[n])) {
        L.stop = false; L.reserve = true;
        L.lm = 28; L.pre = 0; L.hf = -1; L.link = -1; L.bold = L.mono = L.quote = 0; L.gap = 8;
        L.wl = 0; L.pl = 0; L.has = false;
        emit_str(&L, "[page truncated]", 16);
        put_word(&L);
        end_line(&L);
    }
    d->height = L.y + 16;
}

// ---- form controls: drawing, hit testing, editing helpers ----

static struct rect rect_inter(struct rect a, struct rect b) {
    int x0 = imax(a.x, b.x), y0 = imax(a.y, b.y);
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct rect){ x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
}

// Display bytes of the first n bytes of a raw value (UTF-8 or Latin-1); password fields show bullets.
static int to_disp(const char *v, int n, bool pw, char *out, int cap) {
    const char *p = v, *end = v + n;
    int o = 0;
    while (p < end && o < cap - 1) {
        unsigned cp = utf8_next(&p, end);
        if (cp < 0x20 || cp == 0x7f) continue;
        out[o++] = pw ? CH_BULLET[0] : (cp < 0x7f ? (char)cp : cp_slot(cp));
    }
    out[o] = 0;
    return o;
}

static int disp_w(const struct hctl *c, int n) {
    char t[HTML_VAL_LEN + 4];
    to_disp(c->value, n, c->type == HC_PASSWORD, t, sizeof t);
    return text_width(&afont_ui, t);
}

// Horizontal scroll of the text so the caret stays inside a box of width w.
static int text_off(const struct hctl *c, int w) {
    int cw = disp_w(c, c->cur), avail = w - 18;
    return cw > avail ? cw - avail : 0;
}

static bool is_cont(char c) { return (((unsigned char)c) & 0xc0) == 0x80; }

int html_ctl_caret_at(const struct hdoc *d, int ci, int relx) {
    if (ci < 0 || ci >= d->nctl) return 0;
    const struct hctl *c = &d->ctl[ci];
    if (c->type != HC_TEXT && c->type != HC_PASSWORD && c->type != HC_TEXTAREA) return 0;
    int w = 0;
    for (int i = 0; i < d->nruns; i++) if (d->runs[i].kind == K_CTL && d->runs[i].ctl == ci) { w = d->runs[i].w; break; }
    int target = relx - 8 + text_off(c, w), i = 0;
    while (i < c->len) {
        int j = i + 1;
        while (j < c->len && is_cont(c->value[j])) j++;
        if (target < (disp_w(c, i) + disp_w(c, j)) / 2) break;
        i = j;
    }
    return i;
}

static void draw_ctl(const struct hdoc *d, struct surface *s, struct rect view, const struct hrun *r, int sy) {
    int ci = r->ctl;
    if (ci < 0 || ci >= d->nctl) return;
    const struct hctl *c = &d->ctl[ci];
    int x = view.x + r->x, w = r->w, th = f_height(F_UI);
    bool foc = d->focus == ci;
    color_t border = RGB(0xc7, 0xc7, 0xcc), tc = c->disabled ? C_TEXT_INACTIVE : C_TEXT;
    struct rect b = { x, sy, w, CTL_H };
    char t[HTML_VAL_LEN + 4];
    switch (c->type) {
    case HC_TEXT: case HC_PASSWORD: case HC_TEXTAREA: case HC_SELECT: {
        gfx_fill_rrect(s, b, 6, c->disabled ? RGB(0xf2, 0xf2, 0xf5) : RGB(0xff, 0xff, 0xff));
        gfx_stroke_rrect(s, b, 6, foc ? C_ACCENT : border);
        if (foc) gfx_stroke_rrect(s, (struct rect){ x + 1, sy + 1, w - 2, CTL_H - 2 }, 5, C_ACCENT);
        int ty = sy + (CTL_H - th) / 2;
        if (c->type == HC_SELECT) {
            gfx_set_clip(s, rect_inter(view, (struct rect){ x + 6, sy + 1, w - 28, CTL_H - 2 }));
            const char *lab = c->nopt ? d->opts[c->opt_first + c->sel].label : "";
            text_draw(s, &afont_ui, x + 8, ty, lab, tc);
            gfx_set_clip(s, view);
            gfx_line(s, x + w - 18, sy + 10, x + w - 14, sy + 14, C_TEXT_2);
            gfx_line(s, x + w - 14, sy + 14, x + w - 10, sy + 10, C_TEXT_2);
            break;
        }
        gfx_set_clip(s, rect_inter(view, (struct rect){ x + 5, sy + 1, w - 10, CTL_H - 2 }));
        int off = text_off(c, w);
        if (c->len == 0) {
            if (c->ph[0]) text_draw(s, &afont_ui, x + 8, ty, c->ph, C_TEXT_3);
        } else {
            to_disp(c->value, c->len, c->type == HC_PASSWORD, t, sizeof t);
            text_draw(s, &afont_ui, x + 8 - off, ty, t, tc);
        }
        if (foc && d->caret) gfx_vline(s, x + 8 + disp_w(c, c->cur) - off, sy + 4, CTL_H - 8, C_ACCENT);
        gfx_set_clip(s, view);
        break;
    }
    case HC_SUBMIT: case HC_BUTTON: {
        gfx_fill_rrect(s, b, 6, c->disabled ? RGB(0xf7, 0xf7, 0xf9) : RGB(0xf2, 0xf2, 0xf5));
        gfx_stroke_rrect(s, b, 6, foc ? C_ACCENT : border);
        const char *lab = c->ph[0] ? c->ph : c->value[0] ? c->value : "Submit";
        to_disp(lab, (int)strlen(lab), false, t, sizeof t);
        gfx_set_clip(s, rect_inter(view, (struct rect){ x + 3, sy + 1, w - 6, CTL_H - 2 }));
        int tw = text_width(&afont_ui, t), tx = x + (w - tw) / 2;
        text_draw(s, &afont_ui, tx > x + 3 ? tx : x + 3, sy + (CTL_H - th) / 2, t, tc);
        gfx_set_clip(s, view);
        break;
    }
    case HC_CHECK: {
        struct rect q = { x, sy + 4, 16, 16 };
        gfx_fill_rrect(s, q, 4, c->checked ? C_ACCENT : RGB(0xff, 0xff, 0xff));
        gfx_stroke_rrect(s, q, 4, foc || c->checked ? C_ACCENT : border);
        if (c->checked)
            for (int k = 0; k < 2; k++) {
                gfx_line(s, x + 4, sy + 12 + k, x + 7, sy + 15 + k, RGB(0xff, 0xff, 0xff));
                gfx_line(s, x + 7, sy + 15 + k, x + 12, sy + 8 + k, RGB(0xff, 0xff, 0xff));
            }
        break;
    }
    case HC_RADIO: {
        struct rect q = { x, sy + 4, 16, 16 };
        gfx_fill_rrect(s, q, 8, c->checked ? C_ACCENT : RGB(0xff, 0xff, 0xff));
        gfx_stroke_rrect(s, q, 8, foc || c->checked ? C_ACCENT : border);
        if (c->checked) gfx_fill_rrect(s, (struct rect){ x + 5, sy + 9, 6, 6 }, 3, RGB(0xff, 0xff, 0xff));
        break;
    }
    default: break;
    }
}

void html_draw(const struct hdoc *d, struct surface *s, struct rect view, int scroll) {
    gfx_set_clip(s, view);
    for (int i = 0; i < d->nruns; i++) {
        const struct hrun *r = &d->runs[i];
        int sy = view.y + r->y - scroll;
        int h = r->kind == K_HR ? 1 : r->kind == K_CTL ? CTL_H : f_height(r->font);
        if (sy + h < view.y || sy > view.y + view.h) continue;
        if (r->kind == K_HR) { gfx_hline(s, view.x + r->x, sy, r->w, RGB(0xd0, 0xd0, 0xd5)); continue; }
        if (r->kind == K_CTL) { draw_ctl(d, s, view, r, sy); continue; }
        const char *t = d->pool + r->off;
        if (r->font == F_MONO) gfx_text(s, &font_8x16, view.x + r->x, sy, t, r->color);
        else text_draw(s, afont_of(r->font), view.x + r->x, sy, t, r->color);
        if (r->ul) gfx_hline(s, view.x + r->x, sy + f_ascent(r->font) + 2, r->w, r->color);
    }
    gfx_reset_clip(s);
}

int html_link_at(const struct hdoc *d, int x, int y) {
    for (int i = 0; i < d->nruns; i++) {
        const struct hrun *r = &d->runs[i];
        if (r->link < 0 || r->kind != K_TEXT) continue;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + f_height(r->font)) return r->link;
    }
    return -1;
}

int html_ctl_at(const struct hdoc *d, int x, int y) {
    for (int i = 0; i < d->nruns; i++) {
        const struct hrun *r = &d->runs[i];
        if (r->kind != K_CTL || r->ctl < 0) continue;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + CTL_H) return r->ctl;
    }
    return -1;
}

bool html_ctl_rect(const struct hdoc *d, int ci, struct rect *out) {
    for (int i = 0; i < d->nruns; i++) {
        const struct hrun *r = &d->runs[i];
        if (r->kind == K_CTL && r->ctl == ci) { *out = (struct rect){ r->x, r->y, r->w, CTL_H }; return true; }
    }
    return false;
}

int html_ctl_next(const struct hdoc *d, int from, int dir) {
    if (d->nctl <= 0) return -1;
    int i = from;
    if (i < 0 || i >= d->nctl) i = dir > 0 ? -1 : d->nctl;
    for (int k = 0; k < d->nctl; k++) {
        i += dir;
        if (i >= d->nctl) i = 0;
        if (i < 0) i = d->nctl - 1;
        if (d->ctl[i].vis && !d->ctl[i].disabled) return i;
    }
    return -1;
}

void html_ctl_toggle(struct hdoc *d, int ci) {
    if (ci < 0 || ci >= d->nctl) return;
    struct hctl *c = &d->ctl[ci];
    if (c->type == HC_CHECK) c->checked = !c->checked;
    else if (c->type == HC_RADIO) {
        if (c->name[0])
            for (int i = 0; i < d->nctl; i++)
                if (d->ctl[i].type == HC_RADIO && d->ctl[i].form == c->form && strcmp(d->ctl[i].name, c->name) == 0)
                    d->ctl[i].checked = false;
        c->checked = true;
    }
}

void html_ctl_step(struct hdoc *d, int ci, int dir) {
    if (ci < 0 || ci >= d->nctl) return;
    struct hctl *c = &d->ctl[ci];
    if (c->type != HC_SELECT || c->nopt == 0) return;
    c->sel = (uint8_t)((c->sel + c->nopt + (dir < 0 ? -1 : 1)) % c->nopt);
}

int html_form_default_button(const struct hdoc *d, int form) {
    for (int i = 0; i < d->nctl; i++)
        if (d->ctl[i].form == form && d->ctl[i].type == HC_SUBMIT && !d->ctl[i].disabled) return i;
    return -1;
}

// ---- GET submission ----

struct ub { char *o; int n, cap; };      // cap = usable bytes (excluding the NUL)

static bool ub_chr(struct ub *u, char c) {
    if (u->n >= u->cap) return false;
    u->o[u->n++] = c;
    return true;
}

static bool ub_enc(struct ub *u, const char *v, bool trunc_ok) {
    static const char hx[] = "0123456789ABCDEF";
    for (; *v; v++) {
        unsigned char c = (unsigned char)*v;
        bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     c == '-' || c == '_' || c == '.' || c == '~';
        int need = plain || c == ' ' ? 1 : 3;
        if (u->n + need > u->cap) return trunc_ok;       // never cut an escape in half
        if (plain) u->o[u->n++] = (char)c;
        else if (c == ' ') u->o[u->n++] = '+';
        else { u->o[u->n++] = '%'; u->o[u->n++] = hx[c >> 4]; u->o[u->n++] = hx[c & 15]; }
    }
    return true;
}

bool html_form_build_url(const struct hdoc *d, int form, int submitter, const char *action, char *out, int outsz) {
    if (outsz < 2) return false;
    struct ub u = { out, 0, outsz - 1 };
    for (const char *a = action; *a && *a != '?' && *a != '#'; a++)
        if (!ub_chr(&u, *a)) { out[0] = 0; return false; }
    bool first = true;
    for (int i = 0; i < d->nctl; i++) {
        const struct hctl *c = &d->ctl[i];
        if (c->form != form || !c->name[0] || c->disabled) continue;
        const char *val = NULL;
        bool text = false;
        switch (c->type) {
        case HC_TEXT: case HC_PASSWORD: case HC_TEXTAREA: val = c->value; text = true; break;
        case HC_HIDDEN: val = c->value; break;
        case HC_CHECK: case HC_RADIO: if (c->checked) val = c->value; break;
        case HC_SELECT: if (c->nopt) val = d->opts[c->opt_first + c->sel].value; break;
        case HC_SUBMIT: if (i == submitter) val = c->value; break;
        default: break;
        }
        if (!val) continue;
        int mark = u.n;
        bool ok = ub_chr(&u, first ? '?' : '&') && ub_enc(&u, c->name, false) && ub_chr(&u, '=') && ub_enc(&u, val, text);
        // Hidden/auxiliary pairs must leave room for what the user typed later in the document.
        if (ok && !text && u.n + 40 > u.cap && c->type != HC_SUBMIT) ok = false;
        if (!ok) { u.n = mark; continue; }
        first = false;
    }
    out[u.n] = 0;
    return true;
}
