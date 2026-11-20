// Terminal: a text console window with scrollback, a line editor, command
// history and a small set of built-in commands (files, network, system info).
#include "../wm/wm.h"
#include "kprintf.h"
#include "string.h"
#include "mm.h"
#include "vfs.h"
#include "net.h"
#include "rtc.h"

#define FONT        (&font_8x16)
#define CW          8
#define CH          16
#define PAD         6
#define CLIENT_W    640
#define CLIENT_H    400
#define COLS        ((CLIENT_W - 2 * PAD) / CW)      // 78
#define ROWS        ((CLIENT_H - 2 * PAD) / CH)      // 24
#define SB_LINES    1000                              // scrollback ring size
#define ED_MAX      200
#define HIST_N      32
#define CAT_MAX     (64 * 1024)
#define FETCH_BUF   (16 * 1024)

#define C_BG        RGB(0x16, 0x17, 0x1b)
#define C_FG        RGB(0xd8, 0xda, 0xe0)
#define C_PROMPT    RGB(0x7e, 0xe7, 0x87)
#define C_ERR       RGB(0xff, 0x7b, 0x72)
#define C_INFO      RGB(0x79, 0xc0, 0xff)
#define C_DIM       RGB(0x8b, 0x90, 0x9a)

enum { K_NORMAL = 0, K_ERR, K_INFO };

static char *sb;                       // SB_LINES * COLS characters
static uint8_t *lenv;                  // per-line length
static uint8_t *kindv;                 // per-line colour class
static uint8_t *plenv;                 // per-line prompt prefix length (green)
static int head;                       // absolute index of the current (last) line
static int scroll;                     // lines scrolled back from the bottom
static int cur_kind;

static char cwd[VFS_PATH_MAX] = "/";
static char ed[ED_MAX + 1];
static int ed_len, ed_cur;
static int prompt_head, prompt_col;
static char hist[HIST_N][ED_MAX + 1];
static int hist_cnt, hist_pos;         // hist_pos == hist_cnt: editing a new line
static char draft[ED_MAX + 1];
static int cur_line_col;               // cursor position computed by redraw_input
static int cur_line_idx = -1;
static bool blink_on = true, focused = true;
static uint64_t last_blink = (uint64_t)-1;
static bool ready;

#define SLOT(n) ((n) % SB_LINES)

// ---------------------------------------------------------------- buffer
static void newline(void) {
    head++;
    int s = SLOT(head);
    lenv[s] = 0;
    kindv[s] = (uint8_t)cur_kind;
    plenv[s] = 0;
}

static void put(char c) {
    if (c == '\r') return;
    if (c == '\n') { newline(); return; }
    if (c == '\t') {
        int n = 4 - (lenv[SLOT(head)] % 4);
        while (n--) put(' ');
        return;
    }
    if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '.';
    int s = SLOT(head);
    if (lenv[s] >= COLS) { newline(); s = SLOT(head); }
    if (lenv[s] == 0) kindv[s] = (uint8_t)cur_kind;
    sb[s * COLS + lenv[s]++] = c;
}

static void puts_k(const char *str, int kind) {
    int old = cur_kind;
    cur_kind = kind;
    for (; *str; str++) put(*str);
    cur_kind = old;
}

// Prints one formatted line (always newline-terminated).
static void outk(int kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outk(int kind, const char *fmt, ...) {
    char b[320];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    puts_k(b, kind);
    puts_k("\n", kind);
}
#define out(...)  outk(K_NORMAL, __VA_ARGS__)
#define err(...)  outk(K_ERR, __VA_ARGS__)

static void end_line(void) { if (lenv[SLOT(head)] > 0) newline(); }

static void buf_clear(void) {
    head = 0; scroll = 0;
    lenv[0] = 0; kindv[0] = 0; plenv[0] = 0;
}

static int max_scroll(void) {
    int first = head >= SB_LINES - 1 ? head - SB_LINES + 1 : 0;
    int m = head - (first + ROWS - 1);
    return m < 0 ? 0 : m;
}

// ---------------------------------------------------------------- painting
static void paint(struct window *w) {
    struct surface *s = &w->client;
    gfx_reset_clip(s);
    gfx_fill(s, (struct rect){ 0, 0, s->w, s->h }, C_BG);
    if (!ready) { wm_invalidate_all(w); return; }

    int first = head >= SB_LINES - 1 ? head - SB_LINES + 1 : 0;
    int bottom = head - scroll;
    for (int r = 0; r < ROWS; r++) {
        int ln = bottom - (ROWS - 1) + r;
        if (ln < first || ln > head) continue;
        int sl = SLOT(ln);
        int n = lenv[sl];
        int y = PAD + r * CH;
        color_t col = kindv[sl] == K_ERR ? C_ERR : kindv[sl] == K_INFO ? C_INFO : C_FG;
        int pl = plenv[sl] < n ? plenv[sl] : n;
        if (pl) gfx_text_n(s, FONT, PAD, y, &sb[sl * COLS], pl, C_PROMPT);
        if (n > pl) gfx_text_n(s, FONT, PAD + pl * CW, y, &sb[sl * COLS + pl], n - pl, col);

        if (ln == cur_line_idx && scroll == 0 && (blink_on || !focused)) {
            int cx = cur_line_col < COLS ? cur_line_col : COLS - 1;
            int x = PAD + cx * CW;
            if (focused) {
                gfx_fill(s, (struct rect){ x, y, CW, CH }, C_FG);
                char ch = cur_line_col < n ? sb[sl * COLS + cur_line_col] : ' ';
                gfx_glyph(s, FONT, x, y, (unsigned char)ch, C_BG);
            } else {
                gfx_fill(s, (struct rect){ x, y, CW, 1 }, C_DIM);
                gfx_fill(s, (struct rect){ x, y + CH - 1, CW, 1 }, C_DIM);
                gfx_fill(s, (struct rect){ x, y, 1, CH }, C_DIM);
                gfx_fill(s, (struct rect){ x + CW - 1, y, 1, CH }, C_DIM);
            }
        }
    }
    if (scroll > 0) {
        const char *m = "-- scrollback --";
        gfx_text(s, FONT, s->w - PAD - (int)strlen(m) * CW, 2, m, C_DIM);
    }
    wm_invalidate_all(w);
}

// Repaint and present right now (used before blocking network calls).
static void flush(struct window *w) {
    scroll = 0;
    cur_line_idx = -1;      // no cursor while a command runs
    paint(w);
    comp_render();
}

// Rebuilds the editable part of the current line(s) from ed[].
static void redraw_input(void) {
    head = prompt_head;
    lenv[SLOT(head)] = (uint8_t)prompt_col;
    int old = cur_kind;
    cur_kind = K_NORMAL;
    cur_line_idx = head;
    cur_line_col = prompt_col;
    for (int i = 0; i <= ed_len; i++) {
        if (i == ed_cur) {
            int s = SLOT(head);
            if (lenv[s] >= COLS) { newline(); s = SLOT(head); }
            cur_line_idx = head;
            cur_line_col = lenv[s];
        }
        if (i < ed_len) put(ed[i]);
    }
    cur_kind = old;
}

static void show_prompt(void) {
    end_line();
    char p[VFS_PATH_MAX + 16];
    ksnprintf(p, sizeof p, "myos:%s$ ", cwd);
    size_t plen = strlen(p);
    if (plen > 3 * COLS) {
        size_t cl = strlen(cwd);
        ksnprintf(p, sizeof p, "myos:...%s$ ", cwd + (cl > 40 ? cl - 40 : 0));
        plen = strlen(p);
    }
    int old = cur_kind;
    cur_kind = K_NORMAL;
    for (size_t i = 0; i < plen; i++) put(p[i]);
    cur_kind = old;
    int s = SLOT(head);
    prompt_head = head;
    prompt_col = lenv[s];
    plenv[s] = lenv[s];
    ed_len = ed_cur = 0;
    ed[0] = 0;
    hist_pos = hist_cnt;
    redraw_input();
}

// ---------------------------------------------------------------- helpers
static void perr(const char *cmd, const char *arg, int e) {
    if (e == VFS_EROFS) err("%s: %s: read-only file system", cmd, arg);
    else err("%s: %s: %s", cmd, arg, vfs_strerror(e));
}

static int resolve(const char *arg, char *outp) {
    return vfs_path_join(cwd, arg, outp);
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void pad_to(char *dst, size_t n, int width) {
    size_t l = strlen(dst);
    while ((int)l < width && l + 1 < n) dst[l++] = ' ';
    dst[l] = 0;
}

// ---------------------------------------------------------------- commands
static void cmd_help(void) {
    puts_k("Commands:\n", K_INFO);
    out("  help               this list");
    out("  ls [path]          list a directory (dirs end with /)");
    out("  cd [path]          change directory");
    out("  pwd                print the working directory");
    out("  cat <file>         print a file (first 64 KiB)");
    out("  echo text [> file] print text or write it to a file (/disk)");
    out("  mkdir <dir>        create a directory");
    out("  rm <path>          remove a file or empty directory");
    out("  cp <src> <dst>     copy a file");
    out("  mv <src> <dst>     move a file");
    out("  clear              clear the screen");
    out("  ifconfig           show network configuration");
    out("  ping <ip|host>     ICMP echo, 3 tries");
    out("  fetch <host> [path]  HTTP GET (port 80 only, no TLS)");
    out("  date               show the RTC date and time");
    out("  uname              system information");
    out("Keys: Up/Down history, PgUp/PgDn and mouse wheel scroll,");
    out("      Ctrl+A/E/U/L/C line editing.");
}

static void cmd_ls(int argc, char **argv) {
    char path[VFS_PATH_MAX];
    const char *a = argc > 1 ? argv[1] : ".";
    int e = resolve(a, path);
    if (e < 0) { perr("ls", a, e); return; }
    struct vfs_stat st;
    e = vfs_stat(path, &st);
    if (e < 0) { perr("ls", a, e); return; }
    if (st.type != VFS_TYPE_DIR) { out("%s  %llu bytes", path, (unsigned long long)st.size); return; }
    struct vfs_file *d;
    e = vfs_open(path, VFS_O_READ, &d);
    if (e < 0) { perr("ls", a, e); return; }
    struct vfs_dirent *de = kmalloc(sizeof *de);
    if (!de) { vfs_close(d); err("ls: out of memory"); return; }
    int n = 0;
    while (n < 2000 && vfs_readdir(d, de) == 1) {
        char line[VFS_NAME_MAX + 40];
        bool dir = de->type == VFS_TYPE_DIR;
        ksnprintf(line, sizeof line, "%s%s", de->name, dir ? "/" : "");
        pad_to(line, sizeof line, 28);
        if (dir) {
            outk(K_INFO, "%s", line);
        } else {
            size_t l = strlen(line);
            ksnprintf(line + l, sizeof line - l, "%llu", (unsigned long long)de->size);
            out("%s", line);
        }
        n++;
    }
    kfree(de);
    vfs_close(d);
    if (n == 0) puts_k("(empty)\n", K_INFO);
}

static void cmd_cd(int argc, char **argv) {
    char path[VFS_PATH_MAX];
    const char *a = argc > 1 ? argv[1] : "/";
    int e = resolve(a, path);
    if (e < 0) { perr("cd", a, e); return; }
    struct vfs_stat st;
    e = vfs_stat(path, &st);
    if (e < 0) { perr("cd", a, e); return; }
    if (st.type != VFS_TYPE_DIR) { err("cd: %s: not a directory", a); return; }
    strcpy(cwd, path);
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) { err("usage: cat <file>"); return; }
    char path[VFS_PATH_MAX];
    int e = resolve(argv[1], path);
    if (e < 0) { perr("cat", argv[1], e); return; }
    struct vfs_file *f;
    e = vfs_open(path, VFS_O_READ, &f);
    if (e < 0) { perr("cat", argv[1], e); return; }
    if (vfs_file_type(f) == VFS_TYPE_DIR) { vfs_close(f); err("cat: %s: is a directory", argv[1]); return; }
    char buf[512];
    int total = 0;
    bool trunc = false;
    for (;;) {
        int64_t n = vfs_read(f, buf, sizeof buf);
        if (n < 0) { end_line(); perr("cat", argv[1], (int)n); break; }
        if (n == 0) break;
        for (int i = 0; i < n; i++) {
            if (total >= CAT_MAX) { trunc = true; break; }
            put(buf[i]);
            total++;
        }
        if (trunc) break;
    }
    vfs_close(f);
    end_line();
    if (trunc) outk(K_INFO, "[output truncated at %d KiB]", CAT_MAX / 1024);
}

static void cmd_echo(int argc, char **argv) {
    int redir = -1;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], ">") == 0) { redir = i; break; }
    int end = redir >= 0 ? redir : argc;
    char text[ED_MAX + 2];
    size_t tl = 0;
    text[0] = 0;
    for (int i = 1; i < end; i++) {
        if (i > 1 && tl < sizeof text - 2) text[tl++] = ' ';
        for (const char *c = argv[i]; *c && tl < sizeof text - 2; c++) text[tl++] = *c;
        text[tl] = 0;
    }
    if (redir < 0) { out("%s", text); return; }
    if (redir + 1 >= argc) { err("echo: missing file name after >"); return; }
    if (redir + 2 < argc) { err("echo: too many arguments after >"); return; }
    char path[VFS_PATH_MAX];
    int e = resolve(argv[redir + 1], path);
    if (e < 0) { perr("echo", argv[redir + 1], e); return; }
    struct vfs_file *f;
    e = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &f);
    if (e < 0) { perr("echo", argv[redir + 1], e); return; }
    text[tl++] = '\n';
    int64_t n = vfs_write(f, text, tl);
    vfs_close(f);
    if (n < 0) perr("echo", argv[redir + 1], (int)n);
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { err("usage: mkdir <dir>"); return; }
    char path[VFS_PATH_MAX];
    int e = resolve(argv[1], path);
    if (e == 0) e = vfs_mkdir(path);
    if (e < 0) perr("mkdir", argv[1], e);
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) { err("usage: rm <path>"); return; }
    char path[VFS_PATH_MAX];
    int e = resolve(argv[1], path);
    if (e == 0) {
        if (strcmp(path, "/") == 0) { err("rm: refusing to remove /"); return; }
        if (strcmp(path, cwd) == 0) { err("rm: refusing to remove the current directory"); return; }
        e = vfs_unlink(path);
    }
    if (e < 0) perr("rm", argv[1], e);
}

// cp/mv: if dst is an existing directory, the file keeps its name inside it.
static void cmd_cpmv(const char *cmd, int argc, char **argv) {
    if (argc < 3) { err("usage: %s <src> <dst>", cmd); return; }
    char src[VFS_PATH_MAX], dst[VFS_PATH_MAX], full[VFS_PATH_MAX + VFS_NAME_MAX];
    int e = resolve(argv[1], src);
    if (e < 0) { perr(cmd, argv[1], e); return; }
    e = resolve(argv[2], dst);
    if (e < 0) { perr(cmd, argv[2], e); return; }
    struct vfs_stat st;
    if (vfs_stat(dst, &st) == 0 && st.type == VFS_TYPE_DIR) {
        ksnprintf(full, sizeof full, "%s/%s", strcmp(dst, "/") == 0 ? "" : dst, base_name(src));
        e = resolve(full, dst);
        if (e < 0) { perr(cmd, argv[2], e); return; }
    }
    e = cmd[0] == 'c' ? vfs_copy(src, dst) : vfs_rename(src, dst);
    if (e < 0) perr(cmd, argv[2], e);
}

static void cmd_ifconfig(void) {
    struct net_ifconfig c;
    if (!net_ifconfig(&c)) { err("ifconfig: no network interface"); return; }
    char b[20];
    out("eth0  link %s, %s", c.up ? "up" : "down", c.configured ? "DHCP configured" : "not configured");
    out("      mac  %02x:%02x:%02x:%02x:%02x:%02x", c.mac[0], c.mac[1], c.mac[2], c.mac[3], c.mac[4], c.mac[5]);
    out("      ip   %s", ip_to_str(c.ip, b));
    out("      mask %s", ip_to_str(c.mask, b));
    out("      gw   %s", ip_to_str(c.gw, b));
    out("      dns  %s", ip_to_str(c.dns, b));
}

static void cmd_ping(struct window *w, int argc, char **argv) {
    if (argc < 2) { err("usage: ping <ip|host>"); return; }
    uint32_t ip;
    char b[20];
    if (!ip_parse(argv[1], &ip)) {
        out("resolving %s ...", argv[1]);
        flush(w);
        int e = dns_resolve(argv[1], &ip, 2000);
        if (e < 0) {
            err("ping: cannot resolve %s (%s)", argv[1],
                e == NET_ETIMEDOUT ? "timed out" : e == NET_ENOTFOUND ? "not found" : "error");
            return;
        }
    }
    out("PING %s (%s)", argv[1], ip_to_str(ip, b));
    for (int i = 1; i <= 3; i++) {
        flush(w);
        int r = net_ping(ip, 2000);
        if (r >= 0) out("reply from %s: seq=%d time=%d ms", ip_to_str(ip, b), i, r);
        else err("seq=%d: %s", i, r == NET_ETIMEDOUT ? "request timed out" : "failed");
    }
}

static void cmd_fetch(struct window *w, int argc, char **argv) {
    if (argc < 2) { err("usage: fetch <host> [path]   (HTTP on port 80 only, no HTTPS)"); return; }
    char host[96], path[160];
    const char *h = argv[1];
    if (strncmp(h, "https://", 8) == 0) { err("fetch: HTTPS is not supported (port 80 only)"); return; }
    if (strncmp(h, "http://", 7) == 0) h += 7;
    const char *sl = strchr(h, '/');
    size_t hl = sl ? (size_t)(sl - h) : strlen(h);
    if (hl == 0 || hl >= sizeof host) { err("fetch: bad host name"); return; }
    memcpy(host, h, hl);
    host[hl] = 0;
    if (strchr(host, ':')) { err("fetch: only port 80 is supported"); return; }
    const char *p = argc > 2 ? argv[2] : sl ? sl : "/";
    ksnprintf(path, sizeof path, "%s%s", p[0] == '/' ? "" : "/", p);

    char *buf = kmalloc(FETCH_BUF);
    if (!buf) { err("fetch: out of memory"); return; }
    out("GET http://%s%s ...", host, path);
    flush(w);
    int n = http_get_timeout(host, path, buf, FETCH_BUF, 10000);
    if (n < 0) {
        err("fetch: failed (%s)", n == NET_ETIMEDOUT ? "timed out" : n == NET_ENOTFOUND ? "host not found"
                                 : n == NET_ERESET ? "connection reset" : "error");
        kfree(buf);
        return;
    }
    buf[FETCH_BUF - 1] = 0;
    char *eol = strstr(buf, "\r\n");
    if (!eol) eol = strchr(buf, '\n');
    if (eol) *eol = 0;
    outk(K_INFO, "%s", buf);
    out("(%d bytes received)", n);
    char *body = eol ? strstr(eol + 1, "\r\n\r\n") : NULL;
    int skip = 4;
    if (!body && eol) { body = strstr(eol + 1, "\n\n"); skip = 2; }
    if (body) {
        body += skip;
        int lines = 0;
        for (char *q = body; *q && lines < 20; ) {
            char *e2 = strchr(q, '\n');
            if (e2) *e2 = 0;
            for (char *c = q; *c; c++) put(*c);
            put('\n');
            lines++;
            if (!e2) break;
            q = e2 + 1;
        }
    }
    kfree(buf);
}

static void cmd_date(void) {
    static const char *const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct rtc_time t;
    rtc_read(&t);
    out("%s %s %d %02d:%02d:%02d %d", wd[t.weekday % 7], mo[(t.month + 11) % 12], t.day,
        t.hour, t.minute, t.second, t.year);
}

// ---------------------------------------------------------------- dispatch
static int tokenize(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (argc >= max) break;
        char q = (*p == '"' || *p == '\'') ? *p++ : 0;
        argv[argc++] = p;
        while (*p && (q ? *p != q : *p != ' ')) p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

static void run(struct window *w, char *line) {
    char *argv[16];
    int argc = tokenize(line, argv, 16);
    if (argc == 0) return;
    const char *c = argv[0];
    if (!strcmp(c, "help") || !strcmp(c, "?")) cmd_help();
    else if (!strcmp(c, "ls") || !strcmp(c, "dir")) cmd_ls(argc, argv);
    else if (!strcmp(c, "cd")) cmd_cd(argc, argv);
    else if (!strcmp(c, "pwd")) out("%s", cwd);
    else if (!strcmp(c, "cat")) cmd_cat(argc, argv);
    else if (!strcmp(c, "echo")) cmd_echo(argc, argv);
    else if (!strcmp(c, "mkdir")) cmd_mkdir(argc, argv);
    else if (!strcmp(c, "rm")) cmd_rm(argc, argv);
    else if (!strcmp(c, "cp")) cmd_cpmv("cp", argc, argv);
    else if (!strcmp(c, "mv")) cmd_cpmv("mv", argc, argv);
    else if (!strcmp(c, "clear")) buf_clear();
    else if (!strcmp(c, "ifconfig")) cmd_ifconfig();
    else if (!strcmp(c, "ping")) cmd_ping(w, argc, argv);
    else if (!strcmp(c, "fetch")) cmd_fetch(w, argc, argv);
    else if (!strcmp(c, "date")) cmd_date();
    else if (!strcmp(c, "uname")) out("myOS 0.1 x86_64 (hobby kernel, Limine boot)");
    else err("%s: command not found (try 'help')", c);
}

// ---------------------------------------------------------------- input
static void ed_set(const char *s) {
    ed_len = (int)strlen(s);
    if (ed_len > ED_MAX) ed_len = ED_MAX;
    memcpy(ed, s, (size_t)ed_len);
    ed[ed_len] = 0;
    ed_cur = ed_len;
}

static void ed_insert(char c) {
    if (ed_len >= ED_MAX) return;
    memmove(ed + ed_cur + 1, ed + ed_cur, (size_t)(ed_len - ed_cur + 1));
    ed[ed_cur++] = c;
    ed_len++;
}

static void ed_delete(int at) {
    if (at < 0 || at >= ed_len) return;
    memmove(ed + at, ed + at + 1, (size_t)(ed_len - at));
    ed_len--;
}

static void submit(struct window *w) {
    redraw_input();
    cur_line_idx = -1;
    newline();
    char line[ED_MAX + 1];
    strcpy(line, ed);
    if (line[0] && (hist_cnt == 0 || strcmp(hist[(hist_cnt - 1) % HIST_N], line) != 0)) {
        strcpy(hist[hist_cnt % HIST_N], line);
        hist_cnt++;
    }
    run(w, line);
    show_prompt();
}

static void key(struct window *w, const struct input_event *ev) {
    if (!ready || !ev->pressed) return;
    int k = ev->keycode;
    bool changed = true;
    bool skip_redraw = false;

    if (ev->mods & MOD_CTRL) {
        switch (k) {
        case 'a': ed_cur = 0; break;
        case 'e': ed_cur = ed_len; break;
        case 'u': ed_len = ed_cur = 0; ed[0] = 0; break;
        case 'l': buf_clear(); show_prompt(); skip_redraw = true; break;
        case 'c':
            redraw_input();
            cur_line_idx = -1;
            puts_k("^C\n", K_NORMAL);
            show_prompt();
            skip_redraw = true;
            break;
        default: changed = false; break;
        }
    } else switch (k) {
    case KEY_ENTER: submit(w); skip_redraw = true; break;
    case KEY_BACKSPACE: if (ed_cur > 0) { ed_delete(ed_cur - 1); ed_cur--; } break;
    case KEY_DELETE: ed_delete(ed_cur); break;
    case KEY_LEFT: if (ed_cur > 0) ed_cur--; break;
    case KEY_RIGHT: if (ed_cur < ed_len) ed_cur++; break;
    case KEY_HOME: ed_cur = 0; break;
    case KEY_END: ed_cur = ed_len; break;
    case KEY_UP:
        if (hist_pos > 0 && hist_pos > hist_cnt - HIST_N) {
            if (hist_pos == hist_cnt) strcpy(draft, ed);
            hist_pos--;
            ed_set(hist[hist_pos % HIST_N]);
        }
        break;
    case KEY_DOWN:
        if (hist_pos < hist_cnt) {
            hist_pos++;
            ed_set(hist_pos == hist_cnt ? draft : hist[hist_pos % HIST_N]);
        }
        break;
    case KEY_PGUP: case KEY_PGDN:
        scroll += k == KEY_PGUP ? ROWS - 1 : -(ROWS - 1);
        if (scroll > max_scroll()) scroll = max_scroll();
        if (scroll < 0) scroll = 0;
        paint(w);
        return;
    default:
        if ((unsigned char)ev->ascii >= 32 && (unsigned char)ev->ascii < 127) ed_insert(ev->ascii);
        else changed = false;
        break;
    }
    if (!changed) return;
    if (!skip_redraw) redraw_input();
    scroll = 0;
    blink_on = true;
    last_blink = (uint64_t)-1;
    paint(w);
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    (void)x; (void)y; (void)buttons;
    if (!ready || !wheel) return;
    scroll += wheel * 3;
    if (scroll > max_scroll()) scroll = max_scroll();
    if (scroll < 0) scroll = 0;
    paint(w);
}

static void tick(struct window *w, uint64_t now, bool new_second) {
    (void)new_second;
    if (!ready) return;
    uint64_t phase = now / 500;
    if (phase == last_blink) return;
    last_blink = phase;
    blink_on = (phase & 1) == 0;
    paint(w);
}

static void focus(struct window *w, bool f) {
    focused = f;
    if (ready) paint(w);
}

static void init(struct window *w) {
    sb = kmalloc((size_t)SB_LINES * COLS);
    lenv = kmalloc(SB_LINES * 3);
    if (!sb || !lenv) { kprintf("[FAIL] terminal: out of memory\n"); return; }
    kindv = lenv + SB_LINES;
    plenv = kindv + SB_LINES;
    memset(sb, ' ', (size_t)SB_LINES * COLS);
    memset(lenv, 0, SB_LINES * 3);
    buf_clear();
    strcpy(cwd, "/");
    cur_kind = K_NORMAL;
    puts_k("myOS terminal. Type 'help' for a list of commands.\n", K_INFO);
    ready = true;
    show_prompt();
    paint(w);
    kprintf("[OK] terminal\n");
}

static void glyph(struct surface *s, struct rect r) { icon_glyph_terminal(s, r); }

struct app app_terminal = {
    .name = "Terminal",
    .client_w = CLIENT_W, .client_h = CLIENT_H,
    .icon_top = RGB(0x5a, 0x5a, 0x60), .icon_bottom = RGB(0x1c, 0x1c, 0x1e),
    .icon_glyph = glyph,
    .init = init, .paint = paint, .key = key, .mouse = mouse, .tick = tick, .focus = focus,
};
