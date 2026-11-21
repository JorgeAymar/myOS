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
enum { K_TEXT, K_HR };

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
    int bold, ital, mono, pre, hf, quote, link;
    int depth;
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
    if (d->nruns >= MAX_RUNS || d->pool_len + len + 1 > POOL_SZ) { L->stop = true; return; }
    struct hrun *r = &d->runs[d->nruns++];
    r->x = (int16_t)x; r->w = (int16_t)w; r->y = L->y;
    r->off = (uint32_t)d->pool_len;
    r->color = c; r->link = (int16_t)(kind == K_TEXT ? L->link : -1);
    r->font = (uint8_t)font; r->kind = (uint8_t)kind;
    r->ul = (uint8_t)(kind == K_TEXT && L->link >= 0);
    memcpy(d->pool + d->pool_len, t, (size_t)len);
    d->pool[d->pool_len + len] = 0;
    d->pool_len += len + 1;
    if (kind == K_TEXT) {
        L->maxasc = imax(L->maxasc, f_ascent(font));
        L->maxdesc = imax(L->maxdesc, f_height(font) - f_ascent(font));
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

static void emit_cp(struct lay *L, unsigned cp) {
    if (cp == 0xa0) cp = ' ';
    if (cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t') {
        if (L->pre) pre_char(L, (char)cp);
        else { put_word(L); L->space = true; }
        return;
    }
    if (cp < 0x20 || cp == 0x7f) return;
    char one[2] = { 0, 0 };
    if (cp < 0x7f) one[0] = (char)cp;
    else {
        // Font slots: U+00A0..U+00FF map 1:1, typographic extras live at 0x80..0x8a.
        switch (cp) {
        case 0xb7: one[0] = 0x7f; break;
        case 0x2022: one[0] = (char)0x81; break;
        case 0x2014: one[0] = (char)0x82; break;
        case 0x20ac: one[0] = (char)0x83; break;
        case 0x2013: one[0] = (char)0x84; break;
        case 0x2026: one[0] = (char)0x85; break;
        case 0x2018: one[0] = (char)0x86; break;
        case 0x2019: one[0] = (char)0x87; break;
        case 0x201c: one[0] = (char)0x88; break;
        case 0x201d: one[0] = (char)0x89; break;
        case 0x2122: one[0] = (char)0x8a; break;
        default: one[0] = (cp > 0xa0 && cp <= 0xff) ? (char)cp : '?'; break;
        }
    }
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
                    if (*c == '&' && ve - c >= 5 && memcmp(c, "&amp;", 5) == 0) { out[o++] = '&'; c += 4; }
                    else if (*c != '\n' && *c != '\r' && *c != '\t') out[o++] = *c;
                }
                out[o] = 0;
                return true;
            }
        } else if (al == 0) p++;
    }
    return false;
}

static bool is(const char *a, const char *b) { return strcmp(a, b) == 0; }

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

    if (is(nm, "script") || is(nm, "style") || is(nm, "title") || is(nm, "head") || is(nm, "noscript")) {
        if (closing) return NULL;
        char needle[24] = "</";
        strcat(needle, nm);
        if (L->no_head_end && is(nm, "head")) return NULL;
        const char *q = find_ci(e, L->end, needle);
        if (!q) {
            if (!is(nm, "head")) return L->end;
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
        is(nm, "nav") || is(nm, "main") || is(nm, "form") || is(nm, "center") || is(nm, "figure") ||
        is(nm, "address") || is(nm, "fieldset") || is(nm, "details") || is(nm, "summary") ||
        is(nm, "dl") || is(nm, "dt") || is(nm, "dd") || is(nm, "tr") || is(nm, "caption")) {
        block_break(L, is(nm, "dd") || is(nm, "dt") ? 3 : 0);
        return NULL;
    }
    if (is(nm, "ul") || is(nm, "ol")) {
        if (!closing) {
            block_break(L, L->depth == 0 ? 8 : 2);
            if (L->depth < 8) { L->lord[L->depth] = is(nm, "ol"); L->lnum[L->depth] = 0; L->depth++; L->lm += 26; }
        } else {
            block_break(L, L->depth == 1 ? 8 : 2);
            if (L->depth > 0) { L->depth--; L->lm -= 26; }
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
        if (get_attr(s, e, "href", href, sizeof href) && href[0] && strncmp(href, "javascript:", 11) != 0 &&
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
        if (!closing) { L->lm += 28; L->quote++; }
        else if (L->quote > 0) { L->lm -= 28; L->quote--; }
        return NULL;
    }
    if (is(nm, "hr")) {
        block_break(L, 6);
        begin_content(L);
        add_run(L, "", 0, F_UI, 0, L->lm, K_HR, L->width - L->lm);
        if (L->d->nruns > 0) L->d->runs[L->d->nruns - 1].y = L->y + 4;
        L->maxasc = 8; L->maxdesc = 1;
        end_line(L);
        L->gap = 6;
        return NULL;
    }
    if (is(nm, "table")) { block_break(L, 8); return NULL; }
    if (is(nm, "td") || is(nm, "th")) {
        if (is(nm, "th")) adj(&L->bold, closing);
        if (!closing && L->has) { L->x += 20; L->space = false; }
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
    if (!d->runs || !d->pool || !d->links) {
        kfree(d->runs); kfree(d->pool); kfree(d->links); kfree(d);
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
    else { *pp += 1; return 0xfffd; }
    const char *q = *pp + 1;
    for (int i = 0; i < extra; i++, q++) {
        if (q >= end || (((unsigned char)*q) & 0xc0) != 0x80) { *pp = q; return 0xfffd; }
        cp = (cp << 6) | (((unsigned char)*q) & 0x3f);
    }
    *pp = q;
    return cp;
}

void html_layout(struct hdoc *d, const char *html, int width, bool plain) {
    d->nruns = 0; d->pool_len = 0; d->nlinks = 0; d->height = 0; d->title[0] = 0;
    size_t n = 0;
    while (n < HTML_MAX_INPUT && html[n]) n++;
    const char *end = html + n;

    struct lay L;
    memset(&L, 0, sizeof L);
    L.d = d; L.end = end; L.width = width; L.lm = 28; L.x = 28; L.y = 14;
    L.hf = -1; L.link = -1;
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
    d->height = L.y + 16;
}

void html_draw(const struct hdoc *d, struct surface *s, struct rect view, int scroll) {
    gfx_set_clip(s, view);
    for (int i = 0; i < d->nruns; i++) {
        const struct hrun *r = &d->runs[i];
        int sy = view.y + r->y - scroll;
        int h = r->kind == K_HR ? 1 : f_height(r->font);
        if (sy + h < view.y || sy > view.y + view.h) continue;
        if (r->kind == K_HR) { gfx_hline(s, view.x + r->x, sy, r->w, RGB(0xd0, 0xd0, 0xd5)); continue; }
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
