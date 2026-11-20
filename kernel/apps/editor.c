// Editor: a small plain-text editor. The buffer is a flat, heap-allocated char
// array (length + capacity); all state is static so editor_open() works before
// the window exists. Ctrl+S saves, Ctrl+N starts a new document.
#include "../wm/wm.h"
#include "apps.h"
#include "input.h"
#include "kprintf.h"
#include "mm.h"
#include "string.h"
#include "vfs.h"

#define HEADER_H   26
#define STATUS_H   22
#define PAD_X      8
#define PAD_Y      4
#define CW         8                  // font_8x16 cell
#define CH         16
#define MAX_FILE   (256 * 1024)
#define TAB_WIDTH  4
#define DEFAULT_SAVE_PATH "/disk/untitled.txt"

#define C_BAR       RGB(0xf6, 0xf6, 0xf6)
#define C_CURLINE   RGB(0xf3, 0xf7, 0xff)
#define C_MODIFIED  RGB(0xff, 0x9f, 0x0a)
#define C_CARET     RGB(0x00, 0x7a, 0xff)

static char *buf;                     // text, not NUL-terminated
static size_t len, cap;
static size_t cur;                    // cursor byte offset, 0..len
static size_t want_col;               // preferred column for Up/Down
static int top, left;                 // first visible line / column
static char path[VFS_PATH_MAX];       // "" = Untitled
static bool modified;
static char msg[64];                  // status message ("" = none)
static bool focused, blink_on;
static uint64_t blink_phase;

static int text_rows(const struct window *w) { return (w->client.h - HEADER_H - STATUS_H - PAD_Y) / CH; }
static int text_cols(const struct window *w) { return (w->client.w - 2 * PAD_X) / CW; }

static void set_msg(const char *m) { ksnprintf(msg, sizeof msg, "%s", m); }

// ---- buffer helpers ----
static size_t line_start(size_t p) {
    while (p > 0 && buf[p - 1] != '\n') p--;
    return p;
}
static size_t line_end(size_t p) {
    while (p < len && buf[p] != '\n') p++;
    return p;
}
static int count_lines(void) {
    int n = 1;
    for (size_t i = 0; i < len; i++) if (buf[i] == '\n') n++;
    return n;
}
// Offset of the start of line `n` (clamped to the last line).
static size_t offset_of_line(int n) {
    size_t p = 0;
    while (n > 0) {
        size_t e = line_end(p);
        if (e >= len) break;
        p = e + 1;
        n--;
    }
    return p;
}

static bool reserve(size_t need) {
    if (need <= cap) return true;
    if (need > MAX_FILE + 1) return false;
    size_t nc = cap ? cap : 1024;
    while (nc < need) nc *= 2;
    if (nc > MAX_FILE + 1) nc = MAX_FILE + 1;
    char *nb = krealloc(buf, nc);
    if (!nb) return false;
    buf = nb;
    cap = nc;
    return true;
}

static bool insert(const char *s, size_t n) {
    if (len + n > MAX_FILE) { set_msg("Document is full (256 KiB limit)"); return false; }
    if (!reserve(len + n + 1)) { set_msg("Out of memory"); return false; }
    memmove(buf + cur + n, buf + cur, len - cur);
    memcpy(buf + cur, s, n);
    len += n;
    cur += n;
    modified = true;
    msg[0] = 0;
    return true;
}

static void delete_range(size_t a, size_t b) {   // removes [a, b)
    if (a >= b || b > len) return;
    memmove(buf + a, buf + b, len - b);
    len -= b - a;
    cur = a;
    modified = true;
    msg[0] = 0;
}

static void reset_view(void) {
    cur = 0; want_col = 0; top = 0; left = 0;
    blink_on = true;
}

static void new_document(void) {
    kfree(buf);
    buf = NULL; len = cap = 0;
    path[0] = 0;
    modified = false;
    msg[0] = 0;
    reset_view();
}

// ---- view ----
static void ensure_visible(const struct window *w) {
    int rows = text_rows(w), cols = text_cols(w);
    if (rows < 1) rows = 1;
    if (cols < 2) cols = 2;
    int line = 0;
    for (size_t i = 0; i < cur; i++) if (buf[i] == '\n') line++;
    int col = (int)(cur - line_start(cur));
    if (line < top) top = line;
    if (line >= top + rows) top = line - rows + 1;
    if (col < left) left = col;
    if (col >= left + cols - 1) left = col - cols + 2;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
}

static void paint(struct window *w) {
    struct surface *s = &w->client;
    gfx_reset_clip(s);
    gfx_fill(s, (struct rect){ 0, 0, s->w, s->h }, WIN_BG);

    int rows = text_rows(w), cols = text_cols(w);
    int total = count_lines();
    if (top > total - 1) top = total - 1;
    if (top < 0) top = 0;

    // header: path + modified dot
    gfx_fill(s, (struct rect){ 0, 0, s->w, HEADER_H }, C_BAR);
    gfx_hline(s, 0, HEADER_H - 1, s->w, C_SEPARATOR);
    const char *name = path[0] ? path : "Untitled";
    int maxw = s->w - 100;
    size_t nl = strlen(name);
    size_t skip = 0;
    while (skip + 3 < nl && text_width(&afont_ui_med, name + skip) > maxw) skip++;
    char shown[VFS_PATH_MAX + 4];
    if (skip) ksnprintf(shown, sizeof shown, "...%s", name + skip + 3);
    else ksnprintf(shown, sizeof shown, "%s", name);
    text_draw(s, &afont_ui_med, PAD_X + 2, (HEADER_H - afont_ui_med.height) / 2, shown, C_TEXT_2);
    if (modified) {
        int tw = text_width(&afont_small, "Edited");
        text_draw(s, &afont_small, s->w - PAD_X - tw, (HEADER_H - afont_small.height) / 2, "Edited", C_TEXT_3);
        gfx_disc(s, (s->w - PAD_X - tw - 10) * SUBPX, (HEADER_H / 2) * SUBPX, 4 * SUBPX, C_MODIFIED);
    }

    // text area
    int line_cur = 0;
    for (size_t i = 0; i < cur; i++) if (buf[i] == '\n') line_cur++;
    size_t p = offset_of_line(top);
    int y0 = HEADER_H + PAD_Y;
    gfx_set_clip(s, (struct rect){ 0, HEADER_H, s->w, s->h - HEADER_H - STATUS_H });
    for (int r = 0; r < rows && top + r < total; r++) {
        size_t e = line_end(p);
        int y = y0 + r * CH;
        if (top + r == line_cur)
            gfx_fill(s, (struct rect){ 0, y, s->w, CH }, C_CURLINE);
        size_t from = p + (size_t)left;
        for (int c = 0; from + (size_t)c < e && c < cols; c++) {
            unsigned char ch = (unsigned char)buf[from + (size_t)c];
            if (ch == '\r' || ch == '\t' || ch == ' ') continue;
            if (ch < 32 || ch > 126) ch = '.';
            gfx_glyph(s, &font_8x16, PAD_X + c * CW, y, ch, C_TEXT);
        }
        if (top + r == line_cur && focused && blink_on) {
            int cx = PAD_X + ((int)(cur - p) - left) * CW;
            gfx_fill(s, (struct rect){ cx, y + 1, 2, CH - 2 }, C_CARET);
        }
        if (e >= len) break;
        p = e + 1;
    }
    gfx_reset_clip(s);

    // status line
    int sy = s->h - STATUS_H;
    gfx_fill(s, (struct rect){ 0, sy, s->w, STATUS_H }, C_BAR);
    gfx_hline(s, 0, sy, s->w, C_SEPARATOR);
    int ty = sy + (STATUS_H - afont_small.height) / 2;
    char st[64];
    ksnprintf(st, sizeof st, "Ln %d, Col %d", line_cur + 1, (int)(cur - line_start(cur)) + 1);
    int tw = text_width(&afont_small, st);
    text_draw(s, &afont_small, s->w - PAD_X - tw, ty, st, C_TEXT_2);
    if (msg[0]) text_draw(s, &afont_small, PAD_X + 2, ty, msg, C_TEXT);
    else {
        ksnprintf(st, sizeof st, "%u bytes   Ctrl+S save   Ctrl+N new", (unsigned)len);
        text_draw(s, &afont_small, PAD_X + 2, ty, st, C_TEXT_3);
    }
    wm_invalidate_all(w);
}

static void repaint(void) {
    struct window *w = app_editor.win;
    if (!w || !w->open) return;
    ensure_visible(w);
    blink_on = true;
    blink_phase = ~0ull;
    if (!w->minimized) paint(w);
}

// ---- file I/O ----
void editor_open(const char *p) {
    void *data = NULL;
    new_document();
    if (!p || !p[0]) { repaint(); return; }
    ksnprintf(path, sizeof path, "%s", p);
    int64_t n = vfs_read_all(p, &data);
    if (n < 0) {
        ksnprintf(msg, sizeof msg, "Cannot open: %s", vfs_strerror((int)n));
        path[0] = 0;
        repaint();
        return;
    }
    buf = data;
    len = (size_t)n;
    cap = len + 1;
    if (!buf) { len = cap = 0; }
    else if (len > MAX_FILE) {
        len = MAX_FILE;
        set_msg("File too large: opened truncated at 256 KiB");
    }
    repaint();
}

static void save(void) {
    const char *target = path[0] ? path : DEFAULT_SAVE_PATH;
    struct vfs_file *f;
    int r = vfs_open(target, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &f);
    if (r < 0) {
        if (r == VFS_EROFS) set_msg("Read-only file system");
        else ksnprintf(msg, sizeof msg, "Save failed: %s", vfs_strerror(r));
        return;
    }
    size_t done = 0;
    while (done < len) {
        int64_t k = vfs_write(f, buf + done, len - done);
        if (k <= 0) { r = k < 0 ? (int)k : VFS_EIO; break; }
        done += (size_t)k;
    }
    vfs_close(f);
    if (r < 0 || done < len) {
        ksnprintf(msg, sizeof msg, "Save failed: %s", vfs_strerror(r < 0 ? r : VFS_EIO));
        return;
    }
    if (!path[0]) ksnprintf(path, sizeof path, "%s", target);
    modified = false;
    ksnprintf(msg, sizeof msg, "Saved %u bytes", (unsigned)len);
}

// ---- input ----
static void move_vertical(int dir) {
    size_t ls = line_start(cur);
    if (dir < 0) {
        if (ls == 0) return;
        size_t pls = line_start(ls - 1);
        size_t plen = ls - 1 - pls;
        cur = pls + (want_col < plen ? want_col : plen);
    } else {
        size_t le = line_end(cur);
        if (le >= len) return;
        size_t nls = le + 1, nle = line_end(nls);
        size_t nlen = nle - nls;
        cur = nls + (want_col < nlen ? want_col : nlen);
    }
}

static void key(struct window *w, const struct input_event *ev) {
    if (!ev->pressed) return;
    bool edit = false;   // cursor column changed by this key
    if (ev->mods & MOD_CTRL) {
        if (ev->keycode == 's') save();
        else if (ev->keycode == 'n') new_document();
        repaint();
        return;
    }
    int page = text_rows(w) - 1;
    if (page < 1) page = 1;
    switch (ev->keycode) {
    case KEY_LEFT:  if (cur > 0) cur--; edit = true; break;
    case KEY_RIGHT: if (cur < len) cur++; edit = true; break;
    case KEY_UP:    move_vertical(-1); break;
    case KEY_DOWN:  move_vertical(1); break;
    case KEY_PGUP:  for (int i = 0; i < page; i++) move_vertical(-1); break;
    case KEY_PGDN:  for (int i = 0; i < page; i++) move_vertical(1); break;
    case KEY_HOME:  cur = line_start(cur); edit = true; break;
    case KEY_END:   cur = line_end(cur); edit = true; break;
    case KEY_ENTER: insert("\n", 1); edit = true; break;
    case KEY_TAB:   insert("    ", TAB_WIDTH); edit = true; break;
    case KEY_BACKSPACE: if (cur > 0) delete_range(cur - 1, cur); edit = true; break;
    case KEY_DELETE:    if (cur < len) delete_range(cur, cur + 1); edit = true; break;
    default:
        if (ev->ascii >= 32 && ev->ascii < 127) { insert(&ev->ascii, 1); edit = true; }
        else return;
    }
    if (edit) want_col = cur - line_start(cur);
    repaint();
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    if (wheel) {
        top -= wheel * 3;
        int total = count_lines();
        if (top > total - 1) top = total - 1;
        if (top < 0) top = 0;
        blink_on = true;
        paint(w);
        return;
    }
    if (!(buttons & MOUSE_LEFT)) return;
    int y0 = HEADER_H + PAD_Y;
    if (y < HEADER_H || y >= w->client.h - STATUS_H) return;
    int row = (y - y0) / CH;
    if (y < y0) row = 0;
    int col = (x - PAD_X + CW / 2) / CW;
    if (x < PAD_X) col = 0;
    int total = count_lines();
    int line = top + row;
    if (line > total - 1) line = total - 1;
    size_t ls = offset_of_line(line);
    size_t ll = line_end(ls) - ls;
    size_t c = (size_t)(col < 0 ? 0 : col) + (size_t)left;
    cur = ls + (c < ll ? c : ll);
    want_col = cur - ls;
    repaint();
}

static void tick(struct window *w, uint64_t now_ms, bool new_second) {
    (void)new_second;
    if (!focused || w->minimized) return;
    uint64_t phase = now_ms / 500;
    if (phase == blink_phase) return;
    blink_phase = phase;
    blink_on = !(phase & 1);
    paint(w);
}

static void focus(struct window *w, bool f) {
    focused = f;
    blink_on = true;
    blink_phase = ~0ull;
    if (w->open && !w->minimized) paint(w);
}

static void init(struct window *w) {
    (void)w;
    blink_on = true;
    blink_phase = ~0ull;
    kprintf("[OK] editor\n");
}

static void glyph(struct surface *s, struct rect r) { icon_glyph_editor(s, r); }

struct app app_editor = {
    .name = "Editor",
    .client_w = 640, .client_h = 420,
    .icon_top = RGB(0xff, 0xff, 0xff), .icon_bottom = RGB(0xd8, 0xdc, 0xe4),
    .icon_glyph = glyph,
    .init = init,
    .paint = paint,
    .key = key,
    .mouse = mouse,
    .tick = tick,
    .focus = focus,
};
