// Boot self-check for the CSS engine: semantics, malformed input, limits, fuzz, leaks, timing.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "mm.h"
#include "arch.h"
#include "subsystems.h"
#include "css.h"

static int ncases;
static uint64_t slowest;
static struct css_style st;
static int nrules_last;

static void tick(const char *what, uint64_t t0) {
    uint64_t dt = timer_ticks_ms() - t0;
    if (dt > slowest) slowest = dt;
    if (dt > 500) kprintf("[WARN] css %s took %lu ms\n", what, (unsigned long)dt);
    if (dt > 5000) panic("css %s too slow: %lu ms", what, (unsigned long)dt);
}

// Parses `css` (len bytes) into a fresh sheet, computes el, frees the sheet. Result in `st`.
static void sty_n(const char *css, size_t len, const struct css_elem *el, const char *inl, const struct css_style *par) {
    uint64_t t0 = timer_ticks_ms();
    struct css_sheet *sh = css_sheet_new();
    if (!sh) panic("css selftest: out of memory");
    css_parse(sh, css, len);
    nrules_last = css_rule_count(sh);
    css_compute(sh, el, inl, par, &st);
    css_free(sh);
    tick("case", t0);
}
static void sty(const char *css, const char *tag, const char *id, const char *cls, const char *inl) {
    struct css_elem e = { tag, id, cls, NULL };
    sty_n(css, strlen(css), &e, inl, NULL);
}

#define T(name, cond) do { ncases++; if (!(cond)) panic("css case %d failed: %s (line %d)", ncases, name, __LINE__); } while (0)

// deterministic xorshift
static uint32_t rng = 0x9e3779b9u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static const char *frag[] = {
    "p", "div", ".a", ".b", "#x", "a", "ul li", " > ", ">", ",", "{", "}", ";", ":", "color", "red", "#fff", "#12345",
    "rgb(", "rgba(1,2,3,.5)", ")", "(", "\"", "'", "/*", "*/", "@media", "@media print{", "@import", "!important",
    "display:none", "display:block;", "margin:0 auto", "margin:99999999999px", "font-size:2em", "font-size:-3px",
    "width:50%", "\\", "\n", " ", "\t", "0", "-1", "1.5", "px", "em", "%", "[", "]", "::before", ":hover", "+", "~",
    "background:url(", "var(--x)", "calc(1px+2px)", "{{{{", "}}}}", "((((", "\xff\xfe", "\xe2\x82", "a:link", "*",
    "list-style:none", "position:fixed", "float:left", "text-align:center", "font-weight:700", "text-decoration:underline",
    "white-space:pre", "overflow:hidden", "text-transform:uppercase", "visibility:hidden", "!", "--", "-->", "<!--",
};
#define NFRAG ((int)(sizeof frag / sizeof frag[0]))

static void check_style_sane(const struct css_style *s) {
    if (s->mt > 64 || s->mb > 64 || s->pt > 64 || s->pb > 64 || s->width > 4096 || s->maxw > 4096 ||
        s->font_px < 1 || s->font_bucket > 3 || s->display > CSS_DISPLAY_TABLE_CELL || s->align > 2 ||
        s->deco > 2 || s->text_transform > 3 || s->float_ > 2 || (s->width_pct && s->width > 100))
        panic("css fuzz: style out of range");
}

static char *big;
#define BIG (400 * 1024)

// Writes ".rN{color:red}" and returns its length.
static size_t put_rule(char *d, int n) {
    size_t o = 0;
    d[o++] = '.'; d[o++] = 'r';
    char t[8]; int k = 0;
    do { t[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k) d[o++] = t[--k];
    memcpy(d + o, "{color:red}", 11);
    return o + 11;
}

void css_selftest(void) {
    uint64_t free0 = pmm_free_bytes();
    big = kmalloc(BIG);
    if (!big) panic("css selftest: out of memory");

    // ---- selectors & cascade
    sty("p{color:red}", "p", NULL, NULL, NULL);          T("tag", st.has & CSS_H_COLOR && st.color == 0xff0000);
    sty("p{color:red}", "div", NULL, NULL, NULL);        T("tag miss", !(st.has & CSS_H_COLOR));
    sty(".x{color:blue}", "p", NULL, "k x y", NULL);     T("class", st.color == 0x0000ff);
    sty(".x{color:blue}", "p", NULL, "xx", NULL);        T("class whole word", !(st.has & CSS_H_COLOR));
    sty("#i{color:lime}", "p", "i", NULL, NULL);         T("id", st.color == 0x00ff00);
    sty("p.x{color:navy}", "p", NULL, "x", NULL);        T("tag.class", st.color == 0x000080);
    sty("p.x{color:navy}", "div", NULL, "x", NULL);      T("tag.class miss", !(st.has & CSS_H_COLOR));
    sty("*{color:#123456}", "span", NULL, NULL, NULL);   T("universal", st.color == 0x123456);
    sty("h1,h2,p{color:teal}", "p", NULL, NULL, NULL);   T("comma list", st.color == 0x008080);
    sty("h1,h2,p{color:teal}", "p", NULL, NULL, NULL);   T("comma rules", nrules_last == 3);
    sty("#i{color:red}.c{color:blue}p{color:lime}", "p", "i", "c", NULL); T("id beats class", st.color == 0xff0000);
    sty(".c{color:blue}p{color:lime}", "p", NULL, "c", NULL); T("class beats tag", st.color == 0x0000ff);
    sty("p{color:red}p{color:blue}", "p", NULL, NULL, NULL);  T("later wins", st.color == 0x0000ff);
    sty("#i{color:red}p{color:blue}", "p", "i", NULL, NULL);  T("spec beats order", st.color == 0xff0000);
    sty("p{color:red!important}#i{color:blue}", "p", "i", NULL, NULL); T("!important", st.color == 0xff0000);
    sty("p{color:red ! IMPORTANT}#i{color:blue}", "p", "i", NULL, NULL); T("!important spaced", st.color == 0xff0000);
    sty("p{color:red}", "p", NULL, NULL, "color:green");    T("inline beats sheet", st.color == 0x008000);
    sty("p{color:red!important}", "p", NULL, NULL, "color:green"); T("sheet !important beats inline", st.color == 0xff0000);
    sty("p{color:red!important}", "p", NULL, NULL, "color:green!important"); T("inline !important", st.color == 0x008000);
    sty("p{color:red}", "p", NULL, NULL, "color:blue;color:green"); T("inline last wins", st.color == 0x008000);

    {   // descendant / child
        struct css_elem body = { "body", NULL, NULL, NULL };
        struct css_elem div = { "div", NULL, "box", &body };
        struct css_elem ul = { "ul", NULL, NULL, &div };
        struct css_elem li = { "li", NULL, NULL, &ul };
        const char *c1 = "div li{color:red}", *c2 = "div > li{color:red}", *c3 = "ul > li{color:red}";
        sty_n(c1, strlen(c1), &li, NULL, NULL);  T("descendant", st.color == 0xff0000);
        sty_n(c2, strlen(c2), &li, NULL, NULL);  T("child needs direct parent", !(st.has & CSS_H_COLOR));
        sty_n(c3, strlen(c3), &li, NULL, NULL);  T("child", st.color == 0xff0000);
        const char *c4 = "body>div.box ul>li{color:blue}";
        sty_n(c4, strlen(c4), &li, NULL, NULL);  T("4 parts mixed", st.color == 0x0000ff);
        const char *c5 = "html body div ul li{color:blue}";
        sty_n(c5, strlen(c5), &li, NULL, NULL);  T("5 parts rejected", !(st.has & CSS_H_COLOR));
        const char *c6 = "span li{color:red}";
        sty_n(c6, strlen(c6), &li, NULL, NULL);  T("descendant miss", !(st.has & CSS_H_COLOR));
        const char *c7 = ".box ul li{color:blue}.box li{color:red}";
        sty_n(c7, strlen(c7), &li, NULL, NULL);  T("descendant specificity", st.color == 0x0000ff);
    }

    // ---- display / visibility
    sty("p{display:none}", "p", NULL, NULL, NULL);       T("display none", st.has & CSS_H_DISPLAY && st.display == CSS_DISPLAY_NONE);
    sty("p{display:flex}", "p", NULL, NULL, NULL);       T("flex=block", st.display == CSS_DISPLAY_BLOCK);
    sty("p{display:inline-block}", "p", NULL, NULL, NULL); T("inline-block", st.display == CSS_DISPLAY_INLINE_BLOCK);
    sty("p{display:table-cell}", "p", NULL, NULL, NULL); T("table-cell", st.display == CSS_DISPLAY_TABLE_CELL);
    sty("p{display:list-item}", "p", NULL, NULL, NULL);  T("list-item", st.display == CSS_DISPLAY_LIST_ITEM);
    sty("p{display:bogus}", "p", NULL, NULL, NULL);      T("display bogus", !(st.has & CSS_H_DISPLAY));
    sty("p{visibility:hidden}", "p", NULL, NULL, NULL);  T("visibility hidden", st.hidden == 1 && (st.has & CSS_H_VISIBILITY));

    // ---- colors
    sty("p{color:#abc}", "p", NULL, NULL, NULL);                T("#rgb", st.color == 0xaabbcc);
    sty("p{color:#A0b1C2}", "p", NULL, NULL, NULL);             T("#rrggbb", st.color == 0xa0b1c2);
    sty("p{color:rgb(10, 20,30)}", "p", NULL, NULL, NULL);      T("rgb()", st.color == 0x0a141e);
    sty("p{color:rgb(100% 0% 50%)}", "p", NULL, NULL, NULL);    T("rgb() pct", st.color == 0xff007f);
    sty("p{color:rgb(300,-5,0)}", "p", NULL, NULL, NULL);       T("rgb() clamp", st.color == 0xff0000);
    sty("p{background-color:rgba(0,0,0,.5)}", "p", NULL, NULL, NULL); T("rgba blend", (st.has & CSS_H_BG) && st.bg == 0x7f7f7f);
    sty("p{background:rgba(0,0,0,0)}", "p", NULL, NULL, NULL);  T("rgba alpha 0", !(st.has & CSS_H_BG));
    sty("p{background:#fff}.a{background:transparent}", "p", NULL, "a", NULL); T("transparent clears", !(st.has & CSS_H_BG));
    sty("p{background:url(x.png) no-repeat #eee}", "p", NULL, NULL, NULL);     T("background shorthand", st.bg == 0xeeeeee);
    sty("p{color:ORANGE}", "p", NULL, NULL, NULL);              T("named color", st.color == 0xffa500);
    sty("p{color:notacolor}", "p", NULL, NULL, NULL);           T("bad color", !(st.has & CSS_H_COLOR));

    // ---- font / text
    sty("p{font-weight:700}", "p", NULL, NULL, NULL);    T("weight 700", st.bold == 1 && (st.has & CSS_H_BOLD));
    sty("p{font-weight:400}", "p", NULL, NULL, NULL);    T("weight 400", st.bold == 0 && (st.has & CSS_H_BOLD));
    sty("p{font-weight:bold}", "p", NULL, NULL, NULL);   T("weight bold", st.bold == 1);
    sty("p{font-style:italic}", "p", NULL, NULL, NULL);  T("italic", st.italic == 1);
    sty("p{font-size:11px}", "p", NULL, NULL, NULL);     T("size 11px", st.font_bucket == 0 && st.font_px == 11);
    sty("p{font-size:16px}", "p", NULL, NULL, NULL);     T("size 16px", st.font_bucket == 1);
    sty("p{font-size:20px}", "p", NULL, NULL, NULL);     T("size 20px", st.font_bucket == 2);
    sty("p{font-size:2em}", "p", NULL, NULL, NULL);      T("size 2em", st.font_px == 32 && st.font_bucket == 3);
    sty("p{font-size:small}", "p", NULL, NULL, NULL);    T("size small", st.font_bucket == 0);
    sty("p{font-size:x-large}", "p", NULL, NULL, NULL);  T("size x-large", st.font_bucket == 3);
    sty("p{font-size:99999999999px}", "p", NULL, NULL, NULL); T("size huge", st.font_px == 200);
    sty("p{font-size:50%}", "p", NULL, NULL, NULL);      T("size 50%", st.font_px == 8);
    sty("p{text-align:center}", "p", NULL, NULL, NULL);  T("align center", st.align == CSS_ALIGN_CENTER);
    sty("p{text-align:right}", "p", NULL, NULL, NULL);   T("align right", st.align == CSS_ALIGN_RIGHT);
    sty("a{text-decoration:none}", "a", NULL, NULL, NULL); T("deco none", (st.has & CSS_H_DECO) && st.deco == CSS_DECO_NONE);
    sty("a{text-decoration:underline}", "a", NULL, NULL, NULL); T("deco underline", st.deco == CSS_DECO_UNDERLINE);
    sty("a{text-decoration:line-through red}", "a", NULL, NULL, NULL); T("deco line-through", st.deco == CSS_DECO_LINE_THROUGH);
    sty("p{white-space:pre}", "p", NULL, NULL, NULL);    T("white-space pre", st.white_pre == 1);
    sty("p{text-transform:uppercase}", "p", NULL, NULL, NULL); T("uppercase", st.text_transform == CSS_TT_UPPER);
    sty("p{text-transform:capitalize}", "p", NULL, NULL, NULL); T("capitalize", st.text_transform == CSS_TT_CAPITALIZE);

    // ---- box
    sty("p{margin:10px}", "p", NULL, NULL, NULL);        T("margin 1", st.mt == 10 && st.mb == 10);
    sty("p{margin:3px 40px}", "p", NULL, NULL, NULL);    T("margin 2", st.mt == 3 && st.mb == 3);
    sty("p{margin:1px 2px 5px}", "p", NULL, NULL, NULL); T("margin 3", st.mt == 1 && st.mb == 5);
    sty("p{margin:1px 2px 5px 9px}", "p", NULL, NULL, NULL); T("margin 4", st.mt == 1 && st.mb == 5);
    sty("p{margin:0 auto}", "p", NULL, NULL, NULL);      T("margin auto", st.mt == 0 && (st.has & CSS_H_MT));
    sty("p{margin-top:1em;margin-bottom:.5em}", "p", NULL, NULL, NULL); T("margin em", st.mt == 16 && st.mb == 8);
    sty("p{padding:200px}", "p", NULL, NULL, NULL);      T("padding clamp", st.pt == 64 && st.pb == 64);
    sty("p{padding-top:99999999999px}", "p", NULL, NULL, NULL); T("padding huge", st.pt == 64);
    sty("p{padding-top:-5px}", "p", NULL, NULL, NULL);   T("padding neg", st.pt == 0);
    sty("p{margin-top:12px}", "p", NULL, NULL, "margin:4px"); T("inline margin", st.mt == 4);
    sty("p{width:300px;max-width:50%}", "p", NULL, NULL, NULL); T("width px/max%", st.width == 300 && !st.width_pct && st.maxw == 50 && st.maxw_pct);
    sty("p{width:70%}", "p", NULL, NULL, NULL);          T("width %", st.width == 70 && st.width_pct);
    sty("p{width:70%}.a{width:auto}", "p", NULL, "a", NULL); T("width auto clears", !(st.has & CSS_H_WIDTH));
    sty("p{width:99999px}", "p", NULL, NULL, NULL);      T("width clamp", st.width == 4096);
    sty("ul{list-style:none}", "ul", NULL, NULL, NULL);  T("list-style none", st.list_none == 1);
    sty("p{float:right}", "p", NULL, NULL, NULL);        T("float", st.float_ == CSS_FLOAT_RIGHT);
    sty("p{position:fixed}", "p", NULL, NULL, NULL);     T("position fixed", st.position_fixed == 1 && !st.position_absolute);
    sty("p{position:absolute}", "p", NULL, NULL, NULL);  T("position abs", st.position_absolute == 1);
    sty("p{overflow:hidden}", "p", NULL, NULL, NULL);    T("overflow hidden", st.overflow_hidden == 1);

    // ---- inheritance
    {
        struct css_style ps;
        struct css_elem pe = { "div", NULL, NULL, NULL }, ce = { "span", NULL, NULL, &pe };
        const char *c = "div{color:red;font-size:20px;font-weight:bold;text-align:center;display:none;background:#fff}";
        struct css_sheet *sh = css_sheet_new();
        css_parse(sh, c, strlen(c));
        css_compute(sh, &pe, NULL, NULL, &ps);
        css_compute(sh, &ce, NULL, &ps, &st);
        T("inherit color", st.color == 0xff0000 && (st.has & CSS_H_COLOR));
        T("inherit font", st.font_px == 20 && st.bold == 1);
        T("inherit align", st.align == CSS_ALIGN_CENTER);
        T("no inherit display/bg", !(st.has & (CSS_H_DISPLAY | CSS_H_BG)));
        css_compute(sh, &ce, "color:inherit;font-size:smaller", &ps, &st);
        T("smaller of parent", st.font_px == 17);
        css_compute(sh, &ce, "color:blue;font-size:50%", &ps, &st);
        T("child overrides + % of parent", st.color == 0x0000ff && st.font_px == 10);
        css_free(sh);
    }

    // ---- at-rules, comments, pseudo
    sty("@media print{p{color:red}@media x{p{color:red}}} p{margin:7px}", "p", NULL, NULL, NULL);
    T("@media nested skipped", !(st.has & CSS_H_COLOR) && st.mt == 7);
    sty("@import url(a.css);@font-face{font-family:x;src:url(y)}p{color:red}", "p", NULL, NULL, NULL); T("@import/@font-face", st.color == 0xff0000);
    sty("@keyframes k{from{color:red}to{color:blue}}@supports (a:b){p{color:red}}p{color:lime}", "p", NULL, NULL, NULL); T("@keyframes/@supports", st.color == 0x00ff00);
    sty("p/*x*/{/*y*/color:/*z*/red}", "p", NULL, NULL, NULL); T("comments", st.color == 0xff0000);
    sty("<!-- p{color:red} -->", "p", NULL, NULL, NULL);     T("CDO/CDC", st.color == 0xff0000);
    sty("a:hover{color:red}", "a", NULL, NULL, NULL);        T("pseudo skipped", !(st.has & CSS_H_COLOR));
    sty("a:link,a:visited{color:red}", "a", NULL, NULL, NULL); T(":link ok", st.color == 0xff0000);
    sty("a[href]{color:red}b::before{color:red}a+b{color:red}a~b{color:red}", "a", NULL, NULL, NULL); T("attr/::/+/~ skipped", nrules_last == 0);
    sty("p{color:var(--c)}", "p", NULL, NULL, NULL);         T("var() ignored", !(st.has & CSS_H_COLOR));

    // ---- malformed
    sty("p{color:red", "p", NULL, NULL, NULL);               T("unterminated brace", st.color == 0xff0000);
    sty("p{content:\"abc;color:red}\np{margin:2px}", "p", NULL, NULL, NULL); T("unterminated string", st.mt <= 64);
    sty("p{color:red;content:\"abc\n}p{margin:2px}", "p", NULL, NULL, NULL); T("string ends at newline", st.color == 0xff0000 && st.mt == 2);
    sty("p{color:red} /* never closed p{color:blue}", "p", NULL, NULL, NULL); T("unterminated comment", st.color == 0xff0000);
    sty("}}};;;p{color:red}", "p", NULL, NULL, NULL);     T("stray braces", st.color == 0xff0000);
    sty("p{margin:99999999999999999999px}", "p", NULL, NULL, NULL); T("huge number", st.mt == 64);
    sty("p{margin:1.5.5.5px;color:#12}", "p", NULL, NULL, NULL);    T("bad numbers", !(st.has & CSS_H_COLOR));
    {
        static const char nul[] = "p{col\0or:red;margin:3px}\0p{color:blue}";
        struct css_elem e = { "p", NULL, NULL, NULL };
        sty_n(nul, sizeof nul - 1, &e, NULL, NULL);         T("NUL bytes", st.mt == 3);
        sty_n(NULL, 5, &e, NULL, NULL);                     T("NULL css", st.has == 0);
        sty_n("", 0, &e, NULL, NULL);                       T("empty css", st.has == 0);
        css_free(NULL); css_parse(NULL, "p{}", 3);
        T("NULL-safe", css_rule_count(NULL) == 0);
    }

    // ---- limits
    {   // 5000 rules -> capped at 1500; the first ones still work
        size_t o = 0;
        for (int i = 0; i < 5000; i++) o += put_rule(big + o, i);
        struct css_elem e = { "p", NULL, "r7 r4999", NULL };
        sty_n(big, o, &e, NULL, NULL);
        T("5000 rules capped", nrules_last == CSS_MAX_RULES);
        T("rule 7 applies", st.color == 0xff0000);
        struct css_elem e2 = { "p", NULL, "r4999", NULL };
        sty_n(big, o, &e2, NULL, NULL);
        T("rule 4999 dropped", !(st.has & CSS_H_COLOR));
    }
    {   // 300 KB junk, then a valid rule beyond the input cap
        size_t o = 0;
        uint32_t r = 12345;
        while (o < 300 * 1024) { r = r * 1103515245u + 12345u; big[o++] = (char)(32 + (r >> 16) % 95); }
        memcpy(big + o, "p{color:red}", 12);
        struct css_elem e = { "p", NULL, NULL, NULL };
        sty_n(big, o + 12, &e, NULL, NULL);
        T("300KB junk survives", nrules_last <= CSS_MAX_RULES);
        T("rule past input cap ignored", !(st.has & CSS_H_COLOR));
        memset(big, '{', 300 * 1024);
        sty_n(big, 300 * 1024, &e, NULL, NULL);        T("300KB open braces", nrules_last == 0);
        memset(big, '(', 300 * 1024);
        sty_n(big, 300 * 1024, &e, NULL, NULL);        T("300KB open parens", nrules_last == 0);
        memset(big, '"', 300 * 1024);
        sty_n(big, 300 * 1024, &e, NULL, NULL);        T("300KB quotes", nrules_last == 0);
        for (size_t i = 0; i + 5 < 300 * 1024; i += 5) memcpy(big + i, "a b>c", 5);
        sty_n(big, 300 * 1024, &e, NULL, NULL);        T("300KB selector soup", nrules_last == 0);
        for (size_t i = 0; i + 13 < 300 * 1024; i += 13) memcpy(big + i, "p{margin:1px}", 13);
        sty_n(big, 300 * 1024, &e, NULL, NULL);        T("cap of valid rules", nrules_last > 0 && nrules_last <= CSS_MAX_RULES);
        size_t q = 2;
        memcpy(big, "p{", 2);
        for (int i = 0; i < 100; i++) { memcpy(big + q, "margin-top:3px;", 15); q += 15; }
        big[q++] = '}';
        sty_n(big, q, &e, NULL, NULL);                 T("100 decls in one rule", st.mt == 3);
    }
    {   // many ancestors with descendant chains (backtracking budget)
        struct css_elem chain[200];
        for (int i = 0; i < 200; i++) { chain[i].tag = "div"; chain[i].id = NULL; chain[i].classes = "q"; chain[i].parent = i ? &chain[i - 1] : NULL; }
        const char *c = "div div div p{color:red}.q .q .q > .z{color:red}";
        struct css_elem leaf = { "p", NULL, NULL, &chain[199] };
        sty_n(c, strlen(c), &leaf, NULL, NULL);
        T("deep chain matches", st.color == 0xff0000);
    }

    // ---- fuzz: 300 random CSS soups, all invariants checked
    {
        struct css_elem root = { "html", NULL, NULL, NULL }, mid = { "div", "x", "a b", &root };
        struct css_elem leaf = { "p", NULL, "a", &mid };
        for (int n = 0; n < 300; n++) {
            size_t o = 0;
            int parts = 1 + (int)(rnd() % 80);
            for (int i = 0; i < parts && o < 3000; i++) {
                const char *f = frag[rnd() % (uint32_t)NFRAG];
                size_t l = strlen(f);
                memcpy(big + o, f, l); o += l;
                if (rnd() % 4 == 0) big[o++] = ' ';
            }
            if (rnd() % 8 == 0 && o) big[rnd() % o] = (char)rnd();
            big[o] = 0;
            uint64_t t0 = timer_ticks_ms();
            struct css_sheet *sh = css_sheet_new();
            css_parse(sh, big, o);
            struct css_style a, b;
            css_compute(sh, &mid, NULL, NULL, &a);
            check_style_sane(&a);
            css_compute(sh, &leaf, big, &a, &b);   // the soup as inline style too
            check_style_sane(&b);
            css_free(sh);
            tick("fuzz", t0);
            ncases++;
        }
    }

    // ---- leak check: repeated parse/free must not eat memory
    {
        const char *c = "p,.a,#b,div>p.q{color:red;margin:2px 3px;font-size:1.2em}@media x{p{a:b}}";
        struct css_elem e = { "p", NULL, "q", NULL };
        sty_n(c, strlen(c), &e, NULL, NULL);          // warm up the heap
        uint64_t before = pmm_free_bytes();
        for (int i = 0; i < 400; i++) sty_n(c, strlen(c), &e, NULL, NULL);
        uint64_t after = pmm_free_bytes();
        T("no leak in 400 cycles", before <= after + 64 * 1024);
    }

    kfree(big);
    uint64_t free1 = pmm_free_bytes();
    if (free0 > free1 + 512 * 1024) panic("css selftest leaked %lu bytes", (unsigned long)(free0 - free1));
    kprintf("[OK] css (%d cases, slowest %lu ms)\n", ncases, (unsigned long)slowest);
}
