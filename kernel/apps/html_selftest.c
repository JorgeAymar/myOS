// Boot self-check: adversarial documents through the Browser's layout engine.
// Asserts: returns, bounded run/line/link counts, sane run geometry, no leak,
// and reasonable emulated time per case.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "mm.h"
#include "arch.h"
#include "subsystems.h"
#include "html.h"

#define W 760
#define BUF (HTML_MAX_INPUT + 64 * 1024)

static char *buf;
static size_t blen;
static struct hdoc *doc;
static int ncases;
static uint64_t slowest, slowest_id;

static void put(const char *s, size_t n) {
    if (blen + n >= BUF) n = BUF - 1 - blen;
    memcpy(buf + blen, s, n);
    blen += n;
}
static void puts_(const char *s) { put(s, strlen(s)); }
static void rep(const char *s, int n) { size_t l = strlen(s); while (n-- > 0 && blen + l < BUF) put(s, l); }
static void reset(void) { blen = 0; buf[0] = 0; }

static uint32_t rng = 0x2545f491u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void check(int id, const char *name, int width) {
    buf[blen] = 0;
    uint64_t t0 = timer_ticks_ms();
    html_layout(doc, buf, width, false);
    uint64_t dt = timer_ticks_ms() - t0;
    ncases++;
    if (dt > slowest) { slowest = dt; slowest_id = (uint64_t)id; }
    if (dt > 500) kprintf("[WARN] html case %d (%s) took %lu ms\n", id, name, (unsigned long)dt);
    if (dt > 3000) panic("html case %d (%s) too slow: %lu ms", id, name, (unsigned long)dt);
    if (doc->nruns < 0 || doc->nruns > 9000 || doc->pool_len < 0 || doc->pool_len > 160 * 1024 ||
        doc->nlinks < 0 || doc->nlinks > HTML_MAX_LINKS || doc->height < 0)
        panic("html case %d (%s): counts out of range (runs=%d pool=%d links=%d h=%d)", id, name,
              doc->nruns, doc->pool_len, doc->nlinks, doc->height);
    for (int i = 0; i < doc->nruns; i++) {
        const struct hrun *r = &doc->runs[i];
        if (r->x < 0 || r->w < 0 || r->off >= (uint32_t)doc->pool_len || r->link >= doc->nlinks)
            panic("html case %d (%s): bad run %d x=%d w=%d off=%u link=%d", id, name, i, r->x, r->w,
                  (unsigned)r->off, r->link);
    }
}

static void dump(const char *name) {
    kprintf("---- html dump: %s (%d runs, h=%d) ----", name, doc->nruns, doc->height);
    int ly = -1000;
    for (int i = 0; i < doc->nruns && i < 90; i++) {
        const struct hrun *r = &doc->runs[i];
        if (r->y != ly) { kprintf("\n%4d: ", (int)r->y); ly = r->y; }
        kprintf("<%d>%s ", r->x, r->kind ? "----" : doc->pool + r->off);
    }
    kprintf("\n");
}

static const char *const frags[] = {
    "<", ">", "</", "<a href=\"", "\">", "<a href='x&amp;y'>", "</a>", "<p>", "<br>", "<hr>", "<div>", "</div>",
    "<table>", "<tr>", "<td>", "<th>", "</table>", "<ul>", "<ol>", "<li>", "</ul>", "<blockquote>", "<pre>",
    "</pre>", "<code>", "<b>", "</b>", "<i>", "<h1>", "</h1>", "<script>", "</script>", "<style>", "<head>",
    "<title>", "</title>", "<!--", "-->", "<!", "<![endif]-->", "&", "&amp;", "&#", "&#x", "&#xFFFFFFFF;",
    "&nbsp;", "&bogus;", "=", "\"", "'", "/", "<img src=x alt=\"pic\">", "<img width=1 height=1 alt=t>",
    "<input type=text placeholder=Search>", "<input type=submit value=Go>", "<button>", "</button>",
    "<select>", "<textarea>", "<svg>", "<template>", "<form>", "hello ", "world ", "\xc3\xa9", "\xff", "\xe2\x82",
    "\xc0\x80", "\xf4\x90\x80\x80", "\n", "\t", "  ", "javascript:", "<a href=mailto:x>", "<noscript>",
};
#define NFRAG ((int)(sizeof frags / sizeof frags[0]))

// ---- forms ----
static int nform_cases;

static int ctl_named(const char *name, int type) {
    for (int i = 0; i < doc->nctl; i++)
        if (doc->ctl[i].type == type && strcmp(doc->ctl[i].name, name) == 0) return i;
    return -1;
}

static void set_val(int ci, const char *v) {
    strcpy(doc->ctl[ci].value, v);
    doc->ctl[ci].len = doc->ctl[ci].cur = (int16_t)strlen(v);
}

static void forms_selftest(void) {
    static const char big[] =
        "<form action=\"/s\" method=get><input type=hidden name=h value=H1><input type=text name=q>"
        "<input type=submit name=b1 value=One><input type=submit name=b2 value=Two>"
        "<input type=checkbox name=c value=on><select name=s><option value=a>A<option value=b selected>B</select>"
        "<textarea name=t>txt</textarea><input type=text value=nameless></form>";
    html_layout(doc, big, W, false);
    if (doc->nforms != 1 || doc->forms[0].post || strcmp(doc->forms[0].action, "/s") != 0) panic("html form: form record");
    if (doc->nctl != 8) panic("html form: expected 8 controls, got %d", doc->nctl);
    int q = ctl_named("q", HC_TEXT), b1 = ctl_named("b1", HC_SUBMIT), b2 = ctl_named("b2", HC_SUBMIT);
    int c = ctl_named("c", HC_CHECK), sel = ctl_named("s", HC_SELECT), ta = ctl_named("t", HC_TEXTAREA);
    if (q < 0 || b1 < 0 || b2 < 0 || c < 0 || sel < 0 || ta < 0 || ctl_named("h", HC_HIDDEN) < 0)
        panic("html form: control types");
    if (doc->ctl[sel].nopt != 2 || doc->ctl[sel].sel != 1) panic("html form: select state");
    if (strcmp(doc->ctl[ta].value, "txt") != 0 || doc->ctl[c].checked) panic("html form: textarea/checkbox");
    if (html_form_default_button(doc, 0) != b1) panic("html form: default button");
    char out[300];
    #define BUILD(sub, act) html_form_build_url(doc, 0, (sub), (act), out, sizeof out)
    #define EXPECT(str, what) do { nform_cases++; if (strcmp(out, (str)) != 0) panic("html form '%s': got '%s'", what, out); } while (0)
    BUILD(-1, "https://x.org/s?old=1#f"); EXPECT("https://x.org/s?h=H1&q=&s=b&t=txt", "unchecked box, old query dropped");
    set_val(q, "hello"); BUILD(-1, "/s"); EXPECT("/s?h=H1&q=hello&s=b&t=txt", "simple");
    set_val(q, "a b"); BUILD(-1, "/s"); EXPECT("/s?h=H1&q=a+b&s=b&t=txt", "space");
    set_val(q, "a&b=c+d%e"); BUILD(-1, "/s"); EXPECT("/s?h=H1&q=a%26b%3Dc%2Bd%25e&s=b&t=txt", "& = + %");
    set_val(q, "caf\xe9~-_."); BUILD(-1, "/s"); EXPECT("/s?h=H1&q=caf%E9~-_.&s=b&t=txt", "latin1 + unreserved");
    set_val(q, "x"); doc->ctl[c].checked = true; BUILD(b2, "/s");
    EXPECT("/s?h=H1&q=x&b2=Two&c=on&s=b&t=txt", "checked + clicked button b2 only");
    BUILD(b1, "/s"); EXPECT("/s?h=H1&q=x&b1=One&c=on&s=b&t=txt", "clicked button b1");
    html_ctl_toggle(doc, c); if (doc->ctl[c].checked) panic("html form: toggle");
    html_ctl_step(doc, sel, 1); BUILD(-1, "/s"); EXPECT("/s?h=H1&q=x&s=a&t=txt", "select stepped");
    set_val(q, "x"); doc->ctl[q].name[0] = 0; BUILD(-1, "/s"); EXPECT("/s?h=H1&s=a&t=txt", "missing name");
    doc->ctl[q].name[0] = 'q'; BUILD(-1, ""); EXPECT("?h=H1&q=x&s=a&t=txt", "empty action");
    for (int i = 0; i < HTML_VAL_LEN - 1; i++) doc->ctl[q].value[i] = '%';
    doc->ctl[q].value[HTML_VAL_LEN - 1] = 0; doc->ctl[q].len = HTML_VAL_LEN - 1;
    char small[64];
    nform_cases++;
    if (!html_form_build_url(doc, 0, -1, "/s", small, sizeof small) || strlen(small) >= sizeof small ||
        strncmp(small, "/s?h=H1&q=%25", 13) != 0 || small[strlen(small) - 1] == '%' ||
        (strlen(small) >= 3 && small[strlen(small) - 3] != '%'))
        panic("html form: overlong value '%s'", small);
    nform_cases++;
    if (html_form_build_url(doc, 0, -1, "/a-very-long-action-that-does-not-fit", small, 10) || small[0] != 0)
        panic("html form: tiny buffer");
    html_layout(doc, "<form method=POST action=/p><input name=a></form><form><input name=b></form>", W, false);
    nform_cases++;
    if (doc->nforms != 2 || !doc->forms[0].post || doc->forms[1].post) panic("html form: method parse");
    html_layout(doc, "<input name=r type=radio value=1><input name=r type=radio value=2 checked>", W, false);
    html_ctl_toggle(doc, 0);
    nform_cases++;
    if (!doc->ctl[0].checked || doc->ctl[1].checked) panic("html form: radio exclusivity");
    #undef BUILD
    #undef EXPECT
    kprintf("[OK] html forms (%d cases)\n", nform_cases);
}

void html_selftest(void) {
    uint64_t free0 = pmm_free_bytes();
    buf = kmalloc(BUF);
    doc = html_new();
    if (!buf || !doc) panic("html selftest: out of memory");
    int id = 0;

    reset(); check(id++, "empty", W);
    reset(); puts_("<"); check(id++, "lone <", W);
    reset(); puts_("<a href=\"x"); check(id++, "unterminated attr", W);
    reset(); puts_("hi <b"); check(id++, "unterminated tag", W);
    reset(); puts_("hi <!-- never closed <b>x"); check(id++, "unterminated comment", W);
    reset(); puts_("a &amp b &#12 c &#x d &"); check(id++, "unterminated entities", W);
    reset(); rep("<div>", 100000); check(id++, "100k div", W);
    reset(); rep("<blockquote>", 100000); puts_("deep text"); check(id++, "100k blockquote", W);
    reset(); rep("<ul><li>", 100000); puts_("deep text"); check(id++, "100k ul", W);
    reset(); rep("<ol><li>x</ol></ol>", 20000); rep("</ul>", 500); puts_("after"); check(id++, "ul unbalanced", W);
    reset(); rep("<table><tr><td>", 100000); check(id++, "100k table", W);
    reset(); rep("<head>", 50000); puts_("body text"); check(id++, "50k head", W);
    reset(); rep("<title>", 30000); check(id++, "30k title", W);
    reset(); rep("a", 300000); check(id++, "300KB a", W);
    reset(); rep("<b>", 100000); puts_("x"); check(id++, "300KB <b>", W);
    reset(); puts_("<a href=\""); rep("x", 200000); puts_("\">link</a>"); check(id++, "huge attr", W);
    reset(); for (int i = 0; i < 300; i++) { puts_("<a href=\""); rep("h", 150 + i); puts_("\">k</a> "); }
    check(id++, "long hrefs", W);
    reset(); rep("<a href=\"/p&amp;q=1\">l</a> ", 5000); check(id++, "5000 links", W);
    reset(); puts_("<script>var a=1;"); rep("x", 1000); check(id++, "script no end", W);
    reset(); puts_("<style>"); rep("a{}", 1000);
    puts_("</style>after<svg><circle/>in svg</svg><template>t</template>ok"); check(id++, "style/svg/template", W);
    reset(); puts_("a"); put("\0", 1); puts_("b<b>c"); check(id++, "NUL bytes", W);
    reset(); rep("\xff\xfe\xc0\x80\xe0\x80\x80\xf8\x88\x80\x80\x80\x80\xe2\x82\xf4\x90\x80\x80", 5000);
    check(id++, "invalid utf8", W);
    reset(); rep("&#xFFFFFFFF;&#99999999999999;&#0;&#x110000;&#xD800;&#x0000000041;", 3000);
    check(id++, "numeric entities", W);
    reset(); rep("<br>", 5000); puts_("end"); check(id++, "5000 br", W);
    reset(); puts_("<table><tr>"); rep("<td>cell text", 500); puts_("</tr></table>"); check(id++, "500 columns", W);
    reset(); puts_("<table>"); rep("<tr><th>H<td>a<td>b", 400); puts_("</table>"); check(id++, "table rows", W);
    reset(); puts_("<pre>"); rep("<b>x</b>", 50000); puts_("</pre>"); check(id++, "pre inline mess", W);
    reset(); puts_("<pre>"); rep("line of code with some text in it that is long\n", 6000); check(id++, "pre long", W);
    reset(); rep("word ", 60000); check(id++, "many words (truncation)", W);
    if (doc->nruns < 2 || strcmp(doc->pool + doc->runs[doc->nruns - 1].off, "truncated]") != 0 ||
        strcmp(doc->pool + doc->runs[doc->nruns - 2].off, "[page") != 0)
        panic("html: missing [page truncated] notice");
    reset(); puts_("short page"); check(id++, "no notice", W);
    if (strcmp(doc->pool + doc->runs[doc->nruns - 1].off, "page") != 0) panic("html: spurious notice");
    reset(); rep("<p>para text here</p>", 20000); check(id++, "many paragraphs", W);
    reset(); rep("<img alt=\"i\"><input><button>", 8000); check(id++, "forms flood", W);
    reset(); rep("<select><option>a", 3000); rep("<textarea>", 3000); check(id++, "select/textarea unclosed", W);
    reset(); puts_("<p>x"); check(id++, "narrow width", 1);
    reset(); rep("<blockquote><ul><li>narrow text words ", 50); check(id++, "narrow nested", 130);
    reset(); rep("<hr>", 4000); check(id++, "hr flood", W);

    // Byte soup: deterministic pseudo-random documents of 1-4 KB.
    for (int k = 0; k < 200; k++) {
        reset();
        size_t target = 1024 + rnd() % 3072;
        while (blen < target) {
            uint32_t r = rnd();
            if (r % 8 == 0) { char c = (char)(rnd() & 0xff); put(&c, 1); }
            else puts_(frags[r % NFRAG]);
        }
        check(id++, "soup", k & 1 ? W : 140 + (int)(rnd() % 400));
    }

    // Textual dumps of three representative documents (eyeball over serial).
    reset();
    puts_("<html><head><title>T &amp; t</title><style>x{}</style></head><body><h1>Heading</h1>"
          "<p>Para with <a href=\"/a?x=1&amp;y=2\">a link</a>, <b>bold</b> and caf\xc3\xa9 &eacute;.</p>"
          "<ul><li>one<ul><li>nested</ul><li>two</ul><ol><li>first<li>second</ol>"
          "<table><tr><th>Name<th>Qty<tr><td>apple<td>3<tr><td>pear<td>12</table></body></html>");
    check(id++, "dump1", W); dump("lists+table");
    reset();
    puts_("<form><input type=hidden name=h><input type=text placeholder=\"Search the web\"> <input type=submit value=Go>"
          "<button>Click</button><select><option>A<option>B</select><textarea>old</textarea> <input type=checkbox> opt"
          "<br><img src=x.png alt=\"A logo\"><img src=t.gif width=1 height=1 alt=px><img src=y alt=\"\">"
          "<a href=\"javascript:void(0)\">js</a> <a href=\"mailto:a@b\">mail</a> <a href=\"/ok\">ok</a></form>");
    check(id++, "dump2", W); dump("forms+images+links");
    reset();
    puts_("<pre>  int main() {\n\treturn 0;\n}\n</pre><p>Text <code>code</code> <br>after br<hr>after hr"
          "<blockquote>quoted <blockquote>again</blockquote></blockquote><!--[if IE]><p>ie</p><![endif]-->"
          "<dl><dt>Term<dd>Definition</dl><noscript>enable js</noscript>");
    check(id++, "dump3", W); dump("pre+quote+dl");

    forms_selftest();
    html_free(doc);
    kfree(buf);
    uint64_t free1 = pmm_free_bytes();
    if (free0 > free1 + 256 * 1024)
        panic("html selftest leaked %lu bytes", (unsigned long)(free0 - free1));
    kprintf("[OK] html robustness (%d cases, slowest #%lu %lu ms)\n", ncases, (unsigned long)slowest_id,
            (unsigned long)slowest);
}
