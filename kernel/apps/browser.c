// Browser: a tiny HTTP/1.x text browser (no HTTPS yet, no images, CSS or JS).
// The chrome (toolbar, address bar, status line, scrollbar) lives here; HTML
// tokenizing and layout live in html.c.
#include "../wm/wm.h"
#include "html.h"
#include "kprintf.h"
#include "string.h"
#include "mm.h"
#include "net.h"

#define CLIENT_W   860
#define CLIENT_H   560
#define TB_H       40
#define SB_H       22
#define SCROLLBAR  14
#define URL_MAX    256
#define RESP_MAX   (256 * 1024)
#define HIST_N     32
#define MAX_REDIR  5

static struct hdoc *doc;
static char cur_url[URL_MAX];
static char addr[URL_MAX];
static int addr_len, addr_cur, addr_off;
static bool addr_focus, caret_on, loading, addr_sel;   // addr_sel: whole text selected
static int scroll;
static char status[URL_MAX + 32];
static char page[3072];
static char hist[HIST_N][URL_MAX];
static int hist_n, hist_i;

static const char HOME_HTML[] =
    "<html><head><title>Start Page</title></head><body>"
    "<h1>Welcome to the myOS Browser</h1>"
    "<p>A small web browser written from scratch. It speaks plain HTTP and renders "
    "headings, paragraphs, lists, links, tables as text and preformatted blocks. "
    "There are no images, style sheets or scripts.</p>"
    "<h3>Suggested sites</h3>"
    "<ul>"
    "<li><a href=\"http://example.com/\">example.com</a> &mdash; the classic example page</li>"
    "<li><a href=\"http://neverssl.com/\">neverssl.com</a> &mdash; a page that is guaranteed to be plain HTTP</li>"
    "<li><a href=\"http://info.cern.ch/\">info.cern.ch</a> &mdash; the first web site</li>"
    "</ul>"
    "<hr>"
    "<h3>Tips</h3>"
    "<ol>"
    "<li>Click the address bar (or press Ctrl+L), type an address and press Enter.</li>"
    "<li>Scroll with the mouse wheel, arrow keys, Page Up/Down, Home and End.</li>"
    "<li>HTTPS pages need TLS, which arrives with phase 13 of myOS.</li>"
    "</ol>"
    "</body></html>";

// ---- small string helpers ----

static bool starts_ci(const char *s, const char *pre) {
    for (; *pre; s++, pre++) {
        char a = *s, b = *pre;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    return true;
}

// Appends src to dst (capacity n), escaping HTML specials.
static void esc(char *dst, int n, const char *src) {
    int o = 0;
    for (; *src && o < n - 7; src++) {
        const char *r = NULL;
        switch (*src) {
        case '&': r = "&amp;"; break;
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '"': r = "&quot;"; break;
        default: break;
        }
        if (r) { strcpy(dst + o, r); o += (int)strlen(r); }
        else dst[o++] = *src;
    }
    dst[o] = 0;
}

static void set_status(const char *s) {
    strncpy(status, s, sizeof status - 1);
    status[sizeof status - 1] = 0;
}

// ---- URLs ----

// Splits http(s)://host[:port]/path. Fragment is dropped.
static bool split_url(const char *url, bool *https, char *host, int hn, char *path, int pn) {
    const char *p;
    if (starts_ci(url, "http://")) { *https = false; p = url + 7; }
    else if (starts_ci(url, "https://")) { *https = true; p = url + 8; }
    else return false;
    int o = 0;
    while (*p && *p != '/' && *p != '?' && *p != '#' && o < hn - 1) host[o++] = *p++;
    host[o] = 0;
    if (o == 0) return false;
    o = 0;
    if (*p != '/') path[o++] = '/';
    while (*p && *p != '#' && o < pn - 1) path[o++] = *p++;
    path[o] = 0;
    return true;
}

// Removes "." / ".." segments from an absolute path (query kept).
// cap = bytes available at path (it may sit inside a larger buffer).
static void norm_path(char *path, size_t cap) {
    char qs[128]; qs[0] = 0;
    char *q = strchr(path, '?');
    if (q) { strncpy(qs, q, sizeof qs - 1); qs[sizeof qs - 1] = 0; *q = 0; }
    size_t len = strlen(path);
    bool trail = len > 0 && path[len - 1] == '/';
    char out[URL_MAX]; int o = 0;
    const char *p = path;
    while (*p == '/') {
        const char *s = p + 1, *e = s;
        while (*e && *e != '/') e++;
        int l = (int)(e - s);
        if (l == 1 && s[0] == '.') trail = true;
        else if (l == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            trail = true;
        } else if (o + l + 1 < URL_MAX) {
            out[o++] = '/';
            memcpy(out + o, s, (size_t)l); o += l;
            if (l) trail = false;
        }
        p = e;
    }
    if (o == 0 || (trail && out[o - 1] != '/')) { if (o + 1 < URL_MAX) out[o++] = '/'; }
    out[o] = 0;
    size_t ol = strlen(out);
    if (ol >= cap) ol = cap - 1;
    memcpy(path, out, ol);
    path[ol] = 0;
    if (qs[0] && ol + strlen(qs) < cap) strcat(path, qs);
}

// Resolves href against base into out (URL_MAX). False for unsupported links.
static bool resolve(const char *base, const char *href, char *out) {
    char tmp[URL_MAX];
    if (starts_ci(href, "http://") || starts_ci(href, "https://")) {
        strncpy(tmp, href, URL_MAX - 1); tmp[URL_MAX - 1] = 0;
    } else if (href[0] == '/' && href[1] == '/') {
        ksnprintf(tmp, sizeof tmp, "http:%s", href);
    } else {
        for (const char *c = href; *c && *c != '/' && *c != '?' && *c != '#'; c++)
            if (*c == ':') return false;               // mailto:, ftp:, ...
        bool https;
        char host[96], path[URL_MAX];
        if (!split_url(base, &https, host, sizeof host, path, sizeof path)) return false;
        char *q = strchr(path, '?');
        if (href[0] == '#') {
            ksnprintf(tmp, sizeof tmp, "%s://%s%s", https ? "https" : "http", host, path);
            strcpy(out, tmp);
            return true;
        } else if (href[0] == '/') {
            ksnprintf(tmp, sizeof tmp, "%s://%s%s", https ? "https" : "http", host, href);
        } else if (href[0] == '?') {
            if (q) *q = 0;
            ksnprintf(tmp, sizeof tmp, "%s://%s%s%s", https ? "https" : "http", host, path, href);
        } else {
            if (q) *q = 0;
            char *sl = strrchr(path, '/');
            if (sl) sl[1] = 0;
            ksnprintf(tmp, sizeof tmp, "%s://%s%s%s", https ? "https" : "http", host, path, href);
        }
    }
    // normalise the path part and drop the fragment
    char *h = strchr(tmp, '#');
    if (h) *h = 0;
    char *pre = strstr(tmp, "://");
    if (!pre) return false;
    char *ps = strchr(pre + 3, '/');
    if (ps) norm_path(ps, URL_MAX - (size_t)(ps - tmp));
    if (strlen(tmp) >= URL_MAX - 1) return false;
    strcpy(out, tmp);
    return true;
}

// ---- page display ----

static void paint(struct window *w);

static void show_html(struct window *w, const char *html, bool plain) {
    if (!doc) return;
    html_layout(doc, html, w->client.w - SCROLLBAR - 2, plain);
    scroll = 0;
    wm_paint_titlebar(w);
}

static void set_addr(const char *url) {
    strncpy(addr, url, sizeof addr - 1);
    addr[sizeof addr - 1] = 0;
    addr_len = addr_cur = (int)strlen(addr);
    addr_off = 0;
}

static void error_page(struct window *w, const char *title, const char *msg, const char *detail) {
    char t[160], m[600], d[600];
    esc(t, sizeof t, title); esc(m, sizeof m, msg); esc(d, sizeof d, detail);
    ksnprintf(page, sizeof page,
              "<html><head><title>%s</title></head><body><h2>%s</h2><p>%s</p><p>%s</p>"
              "<hr><p><a href=\"about:home\">Back to the start page</a></p></body></html>",
              t, t, m, d);
    show_html(w, page, false);
}

static void https_page(struct window *w, const char *url) {
    char u[600], plain_url[URL_MAX + 8];
    esc(u, sizeof u, url);
    ksnprintf(plain_url, sizeof plain_url, "http://%s", url + 8);
    char pu[600];
    esc(pu, sizeof pu, plain_url);
    ksnprintf(page, sizeof page,
              "<html><head><title>Secure pages not available</title></head><body>"
              "<h2>HTTPS is not available yet</h2>"
              "<p>This browser cannot open <b>%s</b> because secure connections need TLS, "
              "which arrives with phase 13 of myOS.</p>"
              "<p>You can try the unencrypted version of the page instead:</p>"
              "<p><a href=\"%s\">%s</a></p>"
              "<hr><p><a href=\"about:home\">Back to the start page</a></p></body></html>",
              u, pu, pu);
    show_html(w, page, false);
}

// Case-insensitive header lookup in [hs, he). Copies the trimmed value to out.
static bool hdr_get(const char *hs, const char *he, const char *name, char *out, int n) {
    size_t nl = strlen(name);
    const char *p = hs;
    while (p < he) {
        const char *e = p;
        while (e < he && *e != '\n') e++;
        if ((size_t)(e - p) > nl && p[nl] == ':' && starts_ci(p, name)) {
            const char *v = p + nl + 1;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            int o = 0;
            while (v < e && *v != '\r' && o < n - 1) {
                char c = *v++;
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                out[o++] = c;
            }
            out[o] = 0;
            return true;
        }
        p = e + 1;
    }
    return false;
}

// Like hdr_get but keeps the original case (for Location).
static bool hdr_get_raw(const char *hs, const char *he, const char *name, char *out, int n) {
    size_t nl = strlen(name);
    const char *p = hs;
    while (p < he) {
        const char *e = p;
        while (e < he && *e != '\n') e++;
        if ((size_t)(e - p) > nl && p[nl] == ':' && starts_ci(p, name)) {
            const char *v = p + nl + 1;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            int o = 0;
            while (v < e && *v != '\r' && o < n - 1) out[o++] = *v++;
            out[o] = 0;
            return true;
        }
        p = e + 1;
    }
    return false;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes a chunked body in place; returns the new length (NUL-terminated).
static int dechunk(char *body, int len) {
    char *r = body, *end = body + len, *o = body;
    while (r < end) {
        unsigned sz = 0; int nd = 0, hv;
        while (r < end && (hv = hexval(*r)) >= 0 && sz < 0x1000000) { sz = sz * 16 + (unsigned)hv; r++; nd++; }
        if (!nd) break;
        while (r < end && *r != '\n') r++;
        if (r < end) r++;
        if (sz == 0) break;
        if (sz > (unsigned)(end - r)) sz = (unsigned)(end - r);
        memmove(o, r, sz);
        o += sz; r += sz;
        if (r < end && *r == '\r') r++;
        if (r < end && *r == '\n') r++;
    }
    *o = 0;
    return (int)(o - body);
}

// Fetches and displays url (http only), following redirects.
static void load(struct window *w, const char *url_in) {
    char url[URL_MAX];
    strncpy(url, url_in, sizeof url - 1);
    url[sizeof url - 1] = 0;

    for (int hop = 0; hop <= MAX_REDIR; hop++) {
        strcpy(cur_url, url);
        set_addr(url);
        addr_focus = false; addr_sel = false;
        if (strcmp(url, "about:home") == 0) {
            set_status("about:home");
            show_html(w, HOME_HTML, false);
            return;
        }
        bool https;
        char host[96], path[URL_MAX];
        if (!split_url(url, &https, host, sizeof host, path, sizeof path)) {
            set_status("Unsupported address");
            error_page(w, "Unsupported address", "This browser only opens http:// addresses.", url);
            return;
        }
        if (https) { set_status("HTTPS not supported"); https_page(w, url); return; }
        if (strchr(host, ':')) {
            const char *c = strchr(host, ':');
            if (strcmp(c, ":80") != 0) {
                set_status("Unsupported port");
                error_page(w, "Unsupported port", "Only port 80 is supported.", url);
                return;
            }
            host[c - host] = 0;
        }
        struct net_ifconfig nc;
        if (!net_ifconfig(&nc) || !nc.configured) {
            set_status("No network");
            error_page(w, "No network connection",
                       "The network interface is not configured (no DHCP lease).", url);
            return;
        }
        char *buf = kmalloc(RESP_MAX);
        if (!buf) { set_status("Out of memory"); error_page(w, "Out of memory", "Could not allocate the receive buffer.", url); return; }

        ksnprintf(status, sizeof status, "Loading %s ...", url);
        loading = true;
        paint(w);
        comp_render();
        int n = http_get_timeout(host, path, buf, RESP_MAX, 5000);
        loading = false;
        if (n < 0) {
            kfree(buf);
            const char *why = n == NET_ETIMEDOUT ? "The server did not answer in time. It may only accept HTTPS, which myOS does not support yet."
                            : n == NET_ENOTFOUND ? "The server name could not be found (DNS lookup failed)."
                            : n == NET_ERESET    ? "The connection was reset by the server. It may only accept HTTPS, which myOS does not support yet."
                                                 : "A network error occurred.";
            set_status("Load failed");
            error_page(w, "Cannot open the page", why, url);
            return;
        }
        buf[n < RESP_MAX ? n : RESP_MAX - 1] = 0;

        // status line + headers
        char *hend = strstr(buf, "\r\n\r\n");
        int sep = 4;
        if (!hend) { hend = strstr(buf, "\n\n"); sep = 2; }
        if (!hend || !starts_ci(buf, "http/")) {
            kfree(buf);
            set_status("Bad response");
            error_page(w, "Invalid response", "The server sent something that is not valid HTTP.", url);
            return;
        }
        char *body = hend + sep;
        const char *sp = strchr(buf, ' ');
        int code = sp ? atoi(sp + 1) : 0;

        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            char loc[URL_MAX];
            char next[URL_MAX];
            if (hdr_get_raw(buf, hend, "location", loc, sizeof loc) && resolve(url, loc, next)) {
                kfree(buf);
                strcpy(url, next);
                continue;
            }
        }

        char ctype[96], enc[64];
        bool have_ct = hdr_get(buf, hend, "content-type", ctype, sizeof ctype);
        int blen = n - (int)(body - buf);
        if (blen < 0) blen = 0;
        if (hdr_get(buf, hend, "transfer-encoding", enc, sizeof enc) && strstr(enc, "chunked"))
            blen = dechunk(body, blen);

        if (!have_ct || strstr(ctype, "text/html") || strstr(ctype, "application/xhtml")) {
            show_html(w, body, false);
        } else if (strstr(ctype, "text/plain")) {
            show_html(w, body, true);
        } else {
            char ct[120];
            ksnprintf(ct, sizeof ct, "Content type: %s", ctype);
            error_page(w, "Unsupported content type",
                       "This browser can only display HTML and plain text.", ct);
        }
        (void)blen;
        kfree(buf);
        ksnprintf(status, sizeof status, "%s", url);
        if (code >= 400 && !(doc && doc->nruns)) set_status("Error");
        return;
    }
    set_status("Too many redirects");
    error_page(w, "Too many redirects", "The server redirected the browser too many times.", url);
}

// ---- navigation with history ----

static void hist_sync(void) {
    if (hist_n > 0) strcpy(hist[hist_i], cur_url);
}

static void navigate(struct window *w, const char *url) {
    if (hist_n > 0 && hist_i < hist_n - 1) hist_n = hist_i + 1;
    if (hist_n >= HIST_N) {
        memmove(hist[0], hist[1], (size_t)(HIST_N - 1) * URL_MAX);
        hist_n = HIST_N - 1;
    }
    strcpy(hist[hist_n], url);
    hist_i = hist_n++;
    load(w, url);
    hist_sync();
    paint(w);
}

static void go_to(struct window *w, int idx) {
    if (idx < 0 || idx >= hist_n) return;
    hist_i = idx;
    char u[URL_MAX];
    strcpy(u, hist[idx]);
    load(w, u);
    hist_sync();
    paint(w);
}

static void reload(struct window *w) {
    char u[URL_MAX];
    strcpy(u, cur_url);
    load(w, u);
    hist_sync();
    paint(w);
}

// Turns what the user typed into a URL. Returns false if the page was an error.
static void go_typed(struct window *w, const char *in) {
    while (*in == ' ') in++;
    char t[URL_MAX];
    strncpy(t, in, sizeof t - 1); t[sizeof t - 1] = 0;
    int l = (int)strlen(t);
    while (l > 0 && t[l - 1] == ' ') t[--l] = 0;
    if (!l) return;
    if (starts_ci(t, "about:")) { navigate(w, "about:home"); return; }
    if (strstr(t, "://")) { navigate(w, t); return; }
    char hostpart[96]; int o = 0;
    for (const char *c = t; *c && *c != '/' && *c != '?' && o < 95; c++) hostpart[o++] = *c;
    hostpart[o] = 0;
    if (!strchr(hostpart, '.') && strcmp(hostpart, "localhost") != 0) {
        strcpy(cur_url, t);
        set_status("Server not found");
        error_page(w, "Server not found", "There is no search engine here: enter a full address such as example.com.", t);
        paint(w);
        return;
    }
    char u[URL_MAX];
    ksnprintf(u, sizeof u, "http://%s", t);
    navigate(w, u);
}

// ---- painting ----

static struct rect view_rect(const struct window *w) {
    return (struct rect){ 0, TB_H, w->client.w, w->client.h - TB_H - SB_H };
}

static int max_scroll(const struct window *w) {
    int m = doc ? doc->height - view_rect(w).h : 0;
    return m > 0 ? m : 0;
}
static void clamp_scroll(const struct window *w) {
    int m = max_scroll(w);
    if (scroll > m) scroll = m;
    if (scroll < 0) scroll = 0;
}

static int prefix_w(int n) {
    char t[URL_MAX];
    if (n > addr_len) n = addr_len;
    memcpy(t, addr, (size_t)n); t[n] = 0;
    return text_width(&afont_ui, t);
}

static void chevron(struct surface *s, int cx, int cy, int dir, color_t c) {
    for (int k = 0; k < 2; k++) {
        gfx_line(s, cx + dir * 3 + k, cy - 6, cx - dir * 3 + k, cy, c);
        gfx_line(s, cx - dir * 3 + k, cy, cx + dir * 3 + k, cy + 6, c);
    }
}

static void paint_toolbar(struct window *w) {
    struct surface *s = &w->client;
    gfx_reset_clip(s);
    gfx_fill(s, (struct rect){ 0, 0, s->w, TB_H }, RGB(0xf5, 0xf5, 0xf7));
    gfx_hline(s, 0, TB_H - 1, s->w, RGB(0xd8, 0xd8, 0xdc));
    color_t on = C_TEXT_2, off = C_TEXT_INACTIVE;
    chevron(s, 24, 20, 1, hist_i > 0 ? on : off);
    chevron(s, 56, 20, -1, hist_i < hist_n - 1 ? on : off);
    gfx_ring(s, 88 * 16, 20 * 16, 7 * 16, 5 * 16, on);
    gfx_line(s, 91, 12, 96, 12, on); gfx_line(s, 96, 12, 96, 17, on);
    gfx_line(s, 92, 13, 96, 12, on);

    struct rect f = { 112, 7, s->w - 112 - 12, 26 };
    gfx_fill_rrect(s, f, 8, addr_focus ? RGB(0xff, 0xff, 0xff) : RGB(0xe8, 0xe8, 0xec));
    if (addr_focus) gfx_stroke_rrect(s, f, 8, C_ACCENT);
    int avail = f.w - 20;
    if (addr_focus) {
        int cp = prefix_w(addr_cur);
        if (cp - addr_off > avail - 4) addr_off = cp - (avail - 4);
        if (cp - addr_off < 0) addr_off = cp;
        if (addr_off < 0) addr_off = 0;
    } else addr_off = 0;
    gfx_set_clip(s, (struct rect){ f.x + 10, f.y + 1, avail, f.h - 2 });
    if (addr_focus && addr_sel && addr_len)
        gfx_fill(s, (struct rect){ f.x + 10 - addr_off, f.y + 4, prefix_w(addr_len), 18 }, RGB(0xb3, 0xd7, 0xff));
    if (addr_len == 0 && !addr_focus)
        text_draw(s, &afont_ui, f.x + 10, f.y + 5, "Search or enter address", C_TEXT_3);
    else
        text_draw(s, &afont_ui, f.x + 10 - addr_off, f.y + 5, addr, C_TEXT);
    if (addr_focus && caret_on && !addr_sel)
        gfx_vline(s, f.x + 10 + prefix_w(addr_cur) - addr_off, f.y + 5, 16, C_ACCENT);
    gfx_reset_clip(s);
}

static void paint_statusbar(struct window *w) {
    struct surface *s = &w->client;
    int y = s->h - SB_H;
    gfx_fill(s, (struct rect){ 0, y, s->w, SB_H }, RGB(0xf5, 0xf5, 0xf7));
    gfx_hline(s, 0, y, s->w, RGB(0xd8, 0xd8, 0xdc));
    gfx_set_clip(s, (struct rect){ 8, y + 1, s->w - 16, SB_H - 1 });
    text_draw(s, &afont_small, 10, y + 4, status, loading ? C_ACCENT : C_TEXT_3);
    gfx_reset_clip(s);
}

static void paint(struct window *w) {
    struct surface *s = &w->client;
    gfx_reset_clip(s);
    struct rect v = view_rect(w);
    gfx_fill(s, v, WIN_BG);
    if (doc) {
        clamp_scroll(w);
        html_draw(doc, s, v, scroll);
        int m = max_scroll(w);
        if (m > 0) {
            int th = v.h * v.h / doc->height;
            if (th < 28) th = 28;
            int ty = v.y + (int)((long)scroll * (v.h - th) / m);
            gfx_fill_rrect(s, (struct rect){ s->w - 9, ty + 2, 6, th - 4 }, 3, RGBA(0, 0, 0, 0x55));
        }
    }
    paint_toolbar(w);
    paint_statusbar(w);
    wm_invalidate_all(w);
}

static void repaint_toolbar(struct window *w) {
    paint_toolbar(w);
    wm_invalidate(w, (struct rect){ 0, 0, w->client.w, TB_H });
}

// ---- input ----

static void scroll_by(struct window *w, int d) {
    int old = scroll;
    scroll += d;
    clamp_scroll(w);
    if (scroll != old) paint(w);
}

static void addr_unselect_clear(void) {
    if (addr_sel) { addr[0] = 0; addr_len = addr_cur = 0; addr_sel = false; }
}

static void addr_insert(char c) {
    addr_unselect_clear();
    if (addr_len >= URL_MAX - 2) return;
    memmove(addr + addr_cur + 1, addr + addr_cur, (size_t)(addr_len - addr_cur + 1));
    addr[addr_cur++] = c;
    addr_len++;
}

static void key(struct window *w, const struct input_event *ev) {
    if (!ev->pressed) return;
    int k = ev->keycode;
    int vh = view_rect(w).h;

    if (ev->mods & MOD_CTRL) {
        if (k == 'l') { addr_focus = true; caret_on = true; addr_cur = addr_len; addr_sel = addr_len > 0; repaint_toolbar(w); }
        else if (k == 'r') reload(w);
        return;
    }
    if ((ev->mods & MOD_ALT) && k == KEY_LEFT) { go_to(w, hist_i - 1); return; }
    if ((ev->mods & MOD_ALT) && k == KEY_RIGHT) { go_to(w, hist_i + 1); return; }
    if (k == KEY_F5) { reload(w); return; }

    switch (k) {
    case KEY_UP: scroll_by(w, -40); return;
    case KEY_DOWN: scroll_by(w, 40); return;
    case KEY_PGUP: scroll_by(w, -(vh - 40)); return;
    case KEY_PGDN: scroll_by(w, vh - 40); return;
    default: break;
    }

    if (addr_focus) {
        caret_on = true;
        if (addr_sel && (k == KEY_BACKSPACE || k == KEY_DELETE)) { addr_unselect_clear(); repaint_toolbar(w); return; }
        if (addr_sel && k != KEY_ENTER && k != KEY_ESC && !(ev->ascii >= 0x20 && ev->ascii < 0x7f)) addr_sel = false;
        switch (k) {
        case KEY_ENTER: { char t[URL_MAX]; strcpy(t, addr); addr_focus = false; go_typed(w, t); return; }
        case KEY_ESC: addr_focus = false; set_addr(cur_url); break;
        case KEY_BACKSPACE:
            if (addr_cur > 0) {
                memmove(addr + addr_cur - 1, addr + addr_cur, (size_t)(addr_len - addr_cur + 1));
                addr_cur--; addr_len--;
            }
            break;
        case KEY_DELETE:
            if (addr_cur < addr_len) {
                memmove(addr + addr_cur, addr + addr_cur + 1, (size_t)(addr_len - addr_cur));
                addr_len--;
            }
            break;
        case KEY_LEFT: if (addr_cur > 0) addr_cur--; break;
        case KEY_RIGHT: if (addr_cur < addr_len) addr_cur++; break;
        case KEY_HOME: addr_cur = 0; break;
        case KEY_END: addr_cur = addr_len; break;
        default:
            if (ev->ascii >= 0x20 && ev->ascii < 0x7f) addr_insert(ev->ascii);
            else return;
            break;
        }
        repaint_toolbar(w);
        return;
    }
    switch (k) {
    case KEY_HOME: scroll = 0; paint(w); break;
    case KEY_END: scroll = max_scroll(w); paint(w); break;
    case ' ': scroll_by(w, (ev->mods & MOD_SHIFT) ? -(vh - 40) : vh - 40); break;
    case KEY_BACKSPACE: go_to(w, hist_i - 1); break;
    default: break;
    }
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    if (wheel) { scroll_by(w, -wheel * 48); return; }
    if (!(buttons & MOUSE_LEFT)) return;
    struct rect v = view_rect(w);

    if (y < TB_H) {
        if (y >= 6 && y < 34) {
            if (x >= 10 && x < 38) { go_to(w, hist_i - 1); return; }
            if (x >= 42 && x < 70) { go_to(w, hist_i + 1); return; }
            if (x >= 74 && x < 102) { reload(w); return; }
        }
        if (x >= 112 && x < w->client.w - 12) {
            bool was = addr_focus;
            addr_focus = true; caret_on = true; addr_sel = !was && addr_len > 0;
            int rel = x - 122 + addr_off, i = 0;
            while (i < addr_len && prefix_w(i + 1) - (prefix_w(i + 1) - prefix_w(i)) / 2 < rel) i++;
            addr_cur = i;
            repaint_toolbar(w);
        }
        return;
    }
    if (addr_focus) { addr_focus = false; addr_sel = false; set_addr(cur_url); repaint_toolbar(w); }
    if (y < v.y || y >= v.y + v.h || !doc) return;

    int m = max_scroll(w);
    if (x >= w->client.w - SCROLLBAR) {
        if (m > 0) {
            scroll = (int)((long)(y - v.y) * doc->height / v.h) - v.h / 2;
            clamp_scroll(w);
            paint(w);
        }
        return;
    }
    int li = html_link_at(doc, x, y - v.y + scroll);
    if (li >= 0) {
        char href[HTML_LINK_LEN], u[URL_MAX];
        strcpy(href, doc->links[li]);
        if (strcmp(href, "about:home") == 0) { navigate(w, "about:home"); return; }
        if (resolve(cur_url, href, u)) navigate(w, u);
        else { set_status("Unsupported link"); paint(w); }
    }
}

static void tick(struct window *w, uint64_t now_ms, bool new_second) {
    (void)now_ms;
    if (new_second && addr_focus) { caret_on = !caret_on; repaint_toolbar(w); }
}

static void focus(struct window *w, bool focused) {
    (void)w;
    if (!focused) caret_on = true;
}

static const char *title(struct window *w) {
    (void)w;
    return doc && doc->title[0] ? doc->title : "Browser";
}

static void init(struct window *w) {
    if (!doc) doc = html_new();
    if (!doc) { kprintf("[FAIL] browser: out of memory\n"); return; }
    hist_n = hist_i = 0;
    loading = false; scroll = 0;
    navigate(w, "about:home");
    addr_focus = true; caret_on = true;
    addr_cur = addr_len; addr_sel = true;
    kprintf("[OK] browser\n");
}

static void glyph(struct surface *s, struct rect r) { icon_glyph_browser(s, r); }

struct app app_browser = {
    .name = "Browser",
    .client_w = CLIENT_W, .client_h = CLIENT_H,
    .icon_top = RGB(0x5e, 0xe0, 0xf6), .icon_bottom = RGB(0x1a, 0x6c, 0xf0),
    .icon_glyph = glyph,
    .init = init, .paint = paint, .key = key, .mouse = mouse, .tick = tick, .focus = focus,
    .title = title,
};
