// Editor: a Notepad-style plain-text editor. The buffer is a flat, heap-allocated
// char array (length + capacity); all state is static so editor_open() works
// before the window exists. Features: selection, clipboard, undo/redo, Save As /
// Open / Find / Go to line prompts in the status bar, optional word wrap.
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
#define CLIP_MAX   (64 * 1024)
#define UNDO_MAX   256                // operations per stack
#define UNDO_BYTES (1024 * 1024)      // total text kept for undo
#define COALESCE_MAX 128
#define DBLCLICK_MS 500

#define C_BAR       RGB(0xf6, 0xf6, 0xf6)
#define C_CURLINE   RGB(0xf3, 0xf7, 0xff)
#define C_MODIFIED  RGB(0xff, 0x9f, 0x0a)
#define C_CARET     RGB(0x00, 0x7a, 0xff)
#define C_SEL       RGB(0xb5, 0xd6, 0xff)

enum prompt_kind { P_NONE, P_SAVEAS, P_OPEN, P_FIND, P_GOTO, P_OVERWRITE, P_DISCARD };
enum pending { PA_NEW, PA_OPEN_PROMPT, PA_LOAD };

struct op {                           // one undoable edit
    size_t pos, n;                    // where, how many bytes
    char *text;                       // inserted or deleted bytes
    uint32_t gid;                     // ops with the same gid undo together
    bool ins;
};

static char *buf;                     // text, not NUL-terminated
static size_t len, cap;
static size_t cur;                    // cursor byte offset, 0..len
static size_t anchor;                 // selection anchor (valid when sel_on)
static bool sel_on;
static size_t want_col;               // preferred column for Up/Down
static int top, left;                 // first visible visual row / column
static char path[VFS_PATH_MAX];       // "" = Untitled
static bool modified, wrap;
static bool cut_load;                 // file was larger than MAX_FILE: saving it in place would lose data
static char msg[96];                  // status message ("" = none)
static bool focused, blink_on;
static uint64_t blink_phase, now_ms;

static char *clip;                    // clipboard (shared with other apps)
static size_t clip_len;

static struct op undo_st[UNDO_MAX], redo_st[UNDO_MAX];
static int nundo, nredo;
static size_t undo_bytes;
static uint32_t gid_counter;
static bool coalesce_ok;              // next typed char may extend the top undo op

static enum prompt_kind prompt;
static enum pending pend;
static char pbuf[VFS_PATH_MAX];
static size_t plen;
static char pend_path[VFS_PATH_MAX];
static char find_q[64];
static char title_cache[VFS_PATH_MAX + 16];

static uint64_t last_click_ms;
static size_t last_click_cur = (size_t)-1;

static int text_rows(const struct window *w) { return (w->client.h - HEADER_H - STATUS_H - PAD_Y) / CH; }
static int text_cols(const struct window *w) {
    int c = (w->client.w - 2 * PAD_X) / CW;
    return c < 2 ? 2 : c;
}
static int cols_now(void) { return app_editor.win ? text_cols(app_editor.win) : 76; }

static void set_msg(const char *m) { ksnprintf(msg, sizeof msg, "%s", m); }

// ---- clipboard ----
void clipboard_set(const char *s, size_t n) {
    if (n > CLIP_MAX) n = CLIP_MAX;
    char *nb = kmalloc(n + 1);
    if (!nb) return;
    if (n && s) memcpy(nb, s, n);
    nb[n] = 0;
    kfree(clip);
    clip = nb;
    clip_len = n;
}

const char *clipboard_get(size_t *n) {
    if (n) *n = clip ? clip_len : 0;
    return clip ? clip : "";
}

// ---- buffer helpers ----
static size_t line_start(size_t p) {
    while (p > 0 && buf[p - 1] != '\n') p--;
    return p;
}
static size_t line_end(size_t p) {
    while (p < len && buf[p] != '\n') p++;
    return p;
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
static int line_of(size_t off) {
    int n = 0;
    for (size_t i = 0; i < off && i < len; i++) if (buf[i] == '\n') n++;
    return n;
}

// Visual rows a logical line of n bytes occupies. With wrap a line whose length
// is a multiple of the width gets one extra row so the end-of-line caret fits.
static int vrows(size_t n) { return wrap ? (int)(n / (size_t)cols_now()) + 1 : 1; }

static int total_vrows(void) {
    int t = 0;
    size_t p = 0;
    for (;;) {
        size_t e = line_end(p);
        t += vrows(e - p);
        if (e >= len) break;
        p = e + 1;
    }
    return t;
}

static int cursor_vrow(void) {
    int t = 0;
    size_t p = 0;
    for (;;) {
        size_t e = line_end(p);
        if (cur <= e) return t + (wrap ? (int)((cur - p) / (size_t)cols_now()) : 0);
        t += vrows(e - p);
        p = e + 1;
    }
}

// Maps a visual row to its logical line [*ls, *le) and sub-row (clamped).
static void locate(int vrow, size_t *ls, size_t *le, int *sub) {
    size_t p = 0;
    if (vrow < 0) vrow = 0;
    for (;;) {
        size_t e = line_end(p);
        int v = vrows(e - p);
        if (vrow < v || e >= len) {
            *ls = p; *le = e; *sub = vrow < v ? vrow : v - 1;
            return;
        }
        vrow -= v;
        p = e + 1;
    }
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

// ---- selection ----
static bool has_sel(void) { return sel_on && anchor != cur && anchor <= len; }
static size_t sel_lo(void) { return anchor < cur ? anchor : cur; }
static size_t sel_hi(void) { return anchor < cur ? cur : anchor; }

// ---- undo / redo ----
static void op_free(struct op *o) { kfree(o->text); o->text = NULL; }

static void clear_undo(void) {
    for (int i = 0; i < nundo; i++) op_free(&undo_st[i]);
    nundo = 0;
    undo_bytes = 0;
}
static void clear_redo(void) {
    for (int i = 0; i < nredo; i++) op_free(&redo_st[i]);
    nredo = 0;
}
static void clear_history(void) {
    clear_undo();
    clear_redo();
    coalesce_ok = false;
}

static void drop_oldest_undo(void) {
    if (nundo == 0) return;
    undo_bytes -= undo_st[0].n;
    op_free(&undo_st[0]);
    memmove(&undo_st[0], &undo_st[1], (size_t)(nundo - 1) * sizeof(struct op));
    nundo--;
}

// Records an edit that has just been applied. On any failure the history is
// dropped (never left inconsistent) and editing carries on without undo.
static void record(bool ins, size_t pos, const char *text, size_t n, uint32_t gid, bool coalesce) {
    clear_redo();
    if (n == 0) return;
    if (n > UNDO_BYTES) { clear_history(); return; }
    if (coalesce && coalesce_ok && nundo > 0) {
        struct op *t = &undo_st[nundo - 1];
        if (t->ins && ins && t->gid == gid && t->pos + t->n == pos && t->n + n <= COALESCE_MAX) {
            char *nt = krealloc(t->text, t->n + n);
            if (nt) {
                memcpy(nt + t->n, text, n);
                t->text = nt;
                t->n += n;
                undo_bytes += n;
                return;
            }
        }
    }
    while (nundo >= UNDO_MAX || undo_bytes + n > UNDO_BYTES) {
        if (nundo == 0) break;
        drop_oldest_undo();
    }
    char *copy = kmalloc(n);
    if (!copy) { clear_history(); return; }
    memcpy(copy, text, n);
    undo_st[nundo++] = (struct op){ pos, n, copy, gid, ins };
    undo_bytes += n;
    coalesce_ok = coalesce;
}

static bool raw_insert(size_t pos, const char *s, size_t n) {
    if (pos > len || len + n > MAX_FILE) return false;
    if (!reserve(len + n + 1)) return false;
    memmove(buf + pos + n, buf + pos, len - pos);
    memcpy(buf + pos, s, n);
    len += n;
    return true;
}
static void raw_delete(size_t a, size_t b) {     // removes [a, b)
    if (a >= b || b > len) return;
    memmove(buf + a, buf + b, len - b);
    len -= b - a;
}

static uint32_t new_gid(void) { return ++gid_counter; }

// Deletes [a, b) as an undoable edit and leaves the cursor at a.
static void ed_delete(size_t a, size_t b, uint32_t gid) {
    if (a >= b || b > len) return;
    char *saved = kmalloc(b - a);
    if (saved) memcpy(saved, buf + a, b - a);
    raw_delete(a, b);
    cur = a;
    modified = true;
    msg[0] = 0;
    if (saved) record(false, a, saved, b - a, gid, false);
    else clear_history();
    kfree(saved);
}

// Inserts at the cursor as an undoable edit.
static bool ed_insert(const char *s, size_t n, uint32_t gid, bool coalesce) {
    if (len + n > MAX_FILE) { set_msg("Document is full (256 KiB limit)"); return false; }
    size_t pos = cur;
    if (!raw_insert(pos, s, n)) { set_msg("Out of memory"); return false; }
    cur = pos + n;
    modified = true;
    msg[0] = 0;
    record(true, pos, s, n, gid, coalesce);
    return true;
}

// Replaces the selection (if any) with s as one undo step.
static void replace_sel(const char *s, size_t n, bool coalesce) {
    size_t removed = has_sel() ? sel_hi() - sel_lo() : 0;
    if (len - removed + n > MAX_FILE) { set_msg("Document is full (256 KiB limit)"); return; }
    uint32_t gid = new_gid();
    if (coalesce && !removed && coalesce_ok && nundo > 0) gid = undo_st[nundo - 1].gid;
    if (removed) { ed_delete(sel_lo(), sel_hi(), gid); coalesce_ok = false; }
    sel_on = false;
    if (n) ed_insert(s, n, gid, coalesce);
}

static void do_undo(bool redo) {
    struct op *from = redo ? redo_st : undo_st, *to = redo ? undo_st : redo_st;
    int *nf = redo ? &nredo : &nundo, *nt = redo ? &nundo : &nredo;
    coalesce_ok = false;
    sel_on = false;
    if (*nf == 0) { set_msg(redo ? "Nothing to redo" : "Nothing to undo"); return; }
    uint32_t gid = from[*nf - 1].gid;
    while (*nf > 0 && from[*nf - 1].gid == gid) {
        struct op o = from[--*nf];
        if (!redo) undo_bytes -= o.n;
        bool apply_ins = (o.ins == redo);    // undo of insert = delete, redo of insert = insert
        if (apply_ins) {
            if (!raw_insert(o.pos, o.text, o.n)) {
                op_free(&o);
                clear_history();
                set_msg("Undo failed: out of memory");
                return;
            }
            cur = o.pos + o.n;
        } else {
            raw_delete(o.pos, o.pos + o.n);
            cur = o.pos;
        }
        if (*nt < UNDO_MAX) {
            to[(*nt)++] = o;
            if (redo) undo_bytes += o.n;
        } else op_free(&o);
    }
    modified = true;
    msg[0] = 0;
}

static void reset_view(void) {
    cur = 0; want_col = 0; top = 0; left = 0; sel_on = false;
    blink_on = true;
}

static void new_document(void) {
    kfree(buf);
    buf = NULL; len = cap = 0;
    path[0] = 0;
    modified = false;
    cut_load = false;
    msg[0] = 0;
    clear_history();
    reset_view();
}

// ---- view ----
static void ensure_visible(const struct window *w) {
    int rows = text_rows(w), cols = text_cols(w);
    if (rows < 1) rows = 1;
    int vr = cursor_vrow();
    if (vr < top) top = vr;
    if (vr >= top + rows) top = vr - rows + 1;
    if (wrap) left = 0;
    else {
        int col = (int)(cur - line_start(cur));
        if (col < left) left = col;
        if (col >= left + cols - 1) left = col - cols + 2;
        if (left < 0) left = 0;
    }
    if (top < 0) top = 0;
}

static void make_title(char *out, size_t sz) {
    const char *base = "Untitled";
    if (path[0]) {
        base = strrchr(path, '/');
        base = (base && base[1]) ? base + 1 : path;
    }
    ksnprintf(out, sz, "%s%s - Editor", modified ? "*" : "", base);
}

static const char *title(struct window *w) {
    (void)w;
    make_title(title_cache, sizeof title_cache);
    return title_cache;
}

// Draws text clipped to the tail so the end (where typing happens) stays visible.
static void draw_tail(struct surface *s, int x, int y, const char *str, int maxw, color_t c) {
    size_t skip = 0, n = strlen(str);
    while (skip < n && text_width(&afont_small, str + skip) > maxw) skip++;
    text_draw(s, &afont_small, x, y, str + skip, c);
}

static void paint_prompt(struct surface *s, int ty) {
    const char *label = "";
    switch (prompt) {
    case P_SAVEAS: label = "Save as: "; break;
    case P_OPEN:   label = "Open: "; break;
    case P_FIND:   label = "Find: "; break;
    case P_GOTO:   label = "Go to line: "; break;
    default: break;
    }
    int x = PAD_X + 2, avail = s->w - 2 * PAD_X - 4;
    if (prompt == P_OVERWRITE || prompt == P_DISCARD) {
        char line[VFS_PATH_MAX + 32];
        if (prompt == P_OVERWRITE) ksnprintf(line, sizeof line, "Overwrite %s? (y/n)", pend_path);
        else ksnprintf(line, sizeof line, "Discard changes? (y/n)");
        draw_tail(s, x, ty, line, avail, C_TEXT);
        return;
    }
    int lw = text_width(&afont_small, label);
    text_draw(s, &afont_small, x, ty, label, C_TEXT_2);
    pbuf[plen] = 0;
    int room = avail - lw - 6;
    draw_tail(s, x + lw, ty, pbuf, room, C_TEXT);
    size_t skip = 0;
    while (skip < plen && text_width(&afont_small, pbuf + skip) > room) skip++;
    int cx = x + lw + text_width(&afont_small, pbuf + skip);
    if (focused && blink_on) gfx_fill(s, (struct rect){ cx + 1, ty - 1, 1, afont_small.height + 2 }, C_CARET);
}

static void paint(struct window *w) {
    struct surface *s = &w->client;
    gfx_reset_clip(s);
    gfx_fill(s, (struct rect){ 0, 0, s->w, s->h }, WIN_BG);

    int rows = text_rows(w), cols = text_cols(w);
    size_t C = (size_t)cols;
    int total = total_vrows();
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
    int line_cur = line_of(cur);
    bool sel = has_sel();
    size_t sa = sel ? sel_lo() : 0, sb = sel ? sel_hi() : 0;
    size_t ls, le;
    int sub;
    locate(top, &ls, &le, &sub);
    int y0 = HEADER_H + PAD_Y;
    gfx_set_clip(s, (struct rect){ 0, HEADER_H, s->w, s->h - HEADER_H - STATUS_H });
    for (int r = 0; r < rows; r++) {
        int y = y0 + r * CH;
        size_t seg0 = wrap ? ls + (size_t)sub * C : ls + (size_t)left;   // buffer offset of column 0
        if (seg0 > le) seg0 = le;
        size_t to = seg0 + C;
        if (to > le) to = le;
        bool last_sub = !wrap || sub == vrows(le - ls) - 1;
        bool cur_row = cur >= ls && cur <= le && (!wrap || (cur - ls) / C == (size_t)sub);
        if (cur_row) gfx_fill(s, (struct rect){ 0, y, s->w, CH }, C_CURLINE);
        if (sel) {
            size_t lo = sa > seg0 ? sa : seg0, hi = sb < to ? sb : to;
            if (lo < hi) gfx_fill(s, (struct rect){ PAD_X + (int)(lo - seg0) * CW, y, (int)(hi - lo) * CW, CH }, C_SEL);
            // selected newline: one extra cell past the end of the line
            if (last_sub && sa <= le && sb > le && le < len && le >= seg0 && le - seg0 < C)
                gfx_fill(s, (struct rect){ PAD_X + (int)(le - seg0) * CW, y, CW, CH }, C_SEL);
        }
        for (size_t i = seg0; i < to; i++) {
            unsigned char ch = (unsigned char)buf[i];
            if (ch == '\r' || ch == '\t' || ch == ' ') continue;
            if (ch < 32 || ch > 126) ch = '.';
            gfx_glyph(s, &font_8x16, PAD_X + (int)(i - seg0) * CW, y, ch, C_TEXT);
        }
        if (focused && blink_on && prompt == P_NONE && cur_row && cur >= seg0 && cur - seg0 <= C) {
            int cx = PAD_X + (int)(cur - seg0) * CW;
            gfx_fill(s, (struct rect){ cx, y + 1, 2, CH - 2 }, C_CARET);
        }
        // advance to the next visual row
        if (wrap && sub + 1 < vrows(le - ls)) sub++;
        else {
            if (le >= len) break;
            ls = le + 1;
            le = line_end(ls);
            sub = 0;
        }
    }
    gfx_reset_clip(s);

    // status line
    int sy = s->h - STATUS_H;
    gfx_fill(s, (struct rect){ 0, sy, s->w, STATUS_H }, C_BAR);
    gfx_hline(s, 0, sy, s->w, C_SEPARATOR);
    int ty = sy + (STATUS_H - afont_small.height) / 2;
    if (prompt != P_NONE) { paint_prompt(s, ty); wm_invalidate_all(w); return; }
    char st[96];
    ksnprintf(st, sizeof st, "Ln %d, Col %d   %u bytes%s", line_cur + 1, (int)(cur - line_start(cur)) + 1,
              (unsigned)len, wrap ? "   Wrap" : "");
    int tw = text_width(&afont_small, st);
    text_draw(s, &afont_small, s->w - PAD_X - tw, ty, st, C_TEXT_2);
    if (msg[0]) draw_tail(s, PAD_X + 2, ty, msg, s->w - tw - 2 * PAD_X - 12, C_TEXT);
    else text_draw(s, &afont_small, PAD_X + 2, ty, "Ctrl+S save  O open  F find  G go to  W wrap", C_TEXT_3);
    wm_invalidate_all(w);
}

static void repaint(void) {
    struct window *w = app_editor.win;
    if (!w || !w->open) return;
    ensure_visible(w);
    blink_on = true;
    blink_phase = ~0ull;
    if (!w->minimized) paint(w);
    char t[sizeof title_cache];
    make_title(t, sizeof t);
    if (strcmp(t, title_cache) != 0) {
        memcpy(title_cache, t, sizeof t);
        wm_paint_titlebar(w);
    }
}

// ---- file I/O ----
// Replaces the document with the file's contents; on error keeps the old one.
static bool load_path(const char *p) {
    void *data = NULL;
    int64_t n = vfs_read_all(p, &data);
    if (n < 0) {
        ksnprintf(msg, sizeof msg, "Cannot open: %s", vfs_strerror((int)n));
        return false;
    }
    new_document();
    ksnprintf(path, sizeof path, "%s", p);
    buf = data;
    len = (size_t)n;
    cap = len + 1;
    if (!buf) { len = cap = 0; }
    else if (len > MAX_FILE) {
        len = MAX_FILE;
        cut_load = true;
        set_msg("File too large: opened truncated at 256 KiB");
    }
    return true;
}

static void open_prompt(enum prompt_kind k, const char *initial) {
    prompt = k;
    plen = 0;
    if (initial) {
        size_t n = strlen(initial);
        if (n > sizeof pbuf - 1) n = sizeof pbuf - 1;
        memcpy(pbuf, initial, n);
        plen = n;
    }
    pbuf[plen] = 0;
    msg[0] = 0;
}

void editor_open(const char *p) {
    if (!p || !p[0]) {
        if (modified) { pend = PA_NEW; prompt = P_DISCARD; }
        else new_document();
        repaint();
        return;
    }
    if (modified) {
        ksnprintf(pend_path, sizeof pend_path, "%s", p);
        pend = PA_LOAD;
        prompt = P_DISCARD;
    } else {
        prompt = P_NONE;
        load_path(p);
    }
    repaint();
}

static bool write_file(const char *target) {
    struct vfs_file *f;
    if (cut_load && !strcmp(target, path)) {
        set_msg("Not saved: file was opened truncated. Use Save As");
        return false;
    }
    int r = vfs_open(target, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &f);
    if (r < 0) {
        if (r == VFS_EROFS) set_msg("Read-only file system");
        else ksnprintf(msg, sizeof msg, "Save failed: %s", vfs_strerror(r));
        return false;
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
        return false;
    }
    ksnprintf(path, sizeof path, "%s", target);
    modified = false;
    ksnprintf(msg, sizeof msg, "Saved %u bytes", (unsigned)len);
    return true;
}

// Saves to target, asking first if it would overwrite a different existing file.
static void save_to(const char *target) {
    if (target[0] != '/') { set_msg("Invalid path (must start with /)"); return; }
    struct vfs_stat st;
    if (strcmp(target, path) != 0 && vfs_stat(target, &st) == 0) {
        ksnprintf(pend_path, sizeof pend_path, "%s", target);
        prompt = P_OVERWRITE;
        return;
    }
    write_file(target);
}

static void save(void) {
    if (path[0]) write_file(path);
    else open_prompt(P_SAVEAS, DEFAULT_SAVE_PATH);
}

// ---- find / goto ----
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static void find_next(void) {
    size_t qn = strlen(find_q);
    if (qn == 0 || len == 0 || qn > len) { set_msg("Not found"); return; }
    size_t start = has_sel() ? sel_hi() : cur;
    if (start >= len) start = 0;
    for (size_t k = 0; k < len; k++) {
        size_t pos = start + k;
        if (pos >= len) pos -= len;
        if (pos + qn > len) continue;
        size_t j = 0;
        while (j < qn && lower(buf[pos + j]) == lower(find_q[j])) j++;
        if (j == qn) {
            anchor = pos; cur = pos + qn; sel_on = true;
            want_col = cur - line_start(cur);
            msg[0] = 0;
            return;
        }
    }
    set_msg("Not found");
}

static void goto_line(const char *s) {
    long n = 0;
    bool any = false;
    for (; *s >= '0' && *s <= '9' && n < 100000000; s++) { n = n * 10 + (*s - '0'); any = true; }
    if (!any || n < 1) { set_msg("Enter a line number"); return; }
    cur = offset_of_line((int)(n - 1));
    sel_on = false;
    want_col = 0;
    msg[0] = 0;
}

// ---- prompt input ----
static void run_pending(void) {
    prompt = P_NONE;
    switch (pend) {
    case PA_NEW: new_document(); break;
    case PA_OPEN_PROMPT: open_prompt(P_OPEN, path[0] ? path : "/disk/"); break;
    case PA_LOAD: load_path(pend_path); break;
    }
}

static void prompt_key(const struct input_event *ev) {
    if (prompt == P_OVERWRITE || prompt == P_DISCARD) {
        if (ev->ascii == 'y' || ev->ascii == 'Y') {
            if (prompt == P_OVERWRITE) { prompt = P_NONE; write_file(pend_path); }
            else run_pending();
        } else if (ev->ascii == 'n' || ev->ascii == 'N' || ev->keycode == KEY_ESC) {
            prompt = P_NONE;
            set_msg("Cancelled");
        }
        return;
    }
    switch (ev->keycode) {
    case KEY_ESC:
        prompt = P_NONE;
        return;
    case KEY_BACKSPACE:
        if (plen > 0) plen--;
        pbuf[plen] = 0;
        return;
    case KEY_ENTER: {
        pbuf[plen] = 0;
        enum prompt_kind k = prompt;
        if (k == P_FIND) {
            ksnprintf(find_q, sizeof find_q, "%s", pbuf);
            find_next();                 // prompt stays open for repeated Enter
            return;
        }
        prompt = P_NONE;
        if (k == P_SAVEAS) save_to(pbuf);
        else if (k == P_OPEN) { if (pbuf[0]) load_path(pbuf); }
        else if (k == P_GOTO) goto_line(pbuf);
        return;
    }
    default:
        if (ev->ascii >= 32 && ev->ascii < 127 && !(ev->mods & MOD_CTRL) && plen < sizeof pbuf - 1) {
            if (prompt == P_FIND && plen >= sizeof find_q - 1) return;
            pbuf[plen++] = ev->ascii;
            pbuf[plen] = 0;
        }
    }
}

// ---- input ----
static int char_class(char c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') return 1;
    return 2;
}
static size_t word_left(size_t p) {
    while (p > 0 && char_class(buf[p - 1]) == 0) p--;
    if (p > 0) { int c = char_class(buf[p - 1]); while (p > 0 && char_class(buf[p - 1]) == c) p--; }
    return p;
}
static size_t word_right(size_t p) {
    if (p < len && char_class(buf[p]) != 0) { int c = char_class(buf[p]); while (p < len && char_class(buf[p]) == c) p++; }
    while (p < len && char_class(buf[p]) == 0) p++;
    return p;
}

static void select_word_at(size_t p) {
    if (len == 0) return;
    if (p >= len || buf[p] == '\n') { if (p == 0) return; p--; }
    if (buf[p] == '\n') return;
    int c = char_class(buf[p]);
    size_t a = p, b = p + 1;
    while (a > 0 && buf[a - 1] != '\n' && char_class(buf[a - 1]) == c) a--;
    while (b < len && buf[b] != '\n' && char_class(buf[b]) == c) b++;
    anchor = a; cur = b; sel_on = true;
}

static void move_vertical(int dir) {
    size_t ls = line_start(cur);
    size_t C = (size_t)cols_now();
    size_t vc = wrap ? want_col % C : want_col;
    if (!wrap) {
        if (dir < 0) {
            if (ls == 0) return;
            size_t pls = line_start(ls - 1);
            size_t pn = ls - 1 - pls;
            cur = pls + (vc < pn ? vc : pn);
        } else {
            size_t le = line_end(cur);
            if (le >= len) return;
            size_t nls = le + 1, nn = line_end(nls) - nls;
            cur = nls + (vc < nn ? vc : nn);
        }
        return;
    }
    size_t le = line_end(cur), n = le - ls, sub = (cur - ls) / C;
    if (dir < 0) {
        if (sub > 0) cur = ls + (sub - 1) * C + vc;
        else {
            if (ls == 0) return;
            size_t pls = line_start(ls - 1), pn = ls - 1 - pls;
            size_t t = pls + (pn / C) * C + vc;
            cur = t > ls - 1 ? ls - 1 : t;
        }
    } else {
        if (sub < n / C) {
            size_t t = ls + (sub + 1) * C + vc;
            cur = t > le ? le : t;
        } else {
            if (le >= len) return;
            size_t nls = le + 1, nn = line_end(nls) - nls;
            cur = nls + (vc < nn ? vc : nn);
        }
    }
}

static void begin_move(bool shift) {
    if (shift) { if (!has_sel()) { anchor = cur; sel_on = true; } }
    else sel_on = false;
}

static void do_copy(bool cut) {
    if (!has_sel()) return;
    clipboard_set(buf + sel_lo(), sel_hi() - sel_lo());
    if (cut) replace_sel(NULL, 0, false);
}

static void do_paste(void) {
    size_t n;
    const char *c = clipboard_get(&n);
    if (n == 0) return;
    replace_sel(c, n, false);
    coalesce_ok = false;
}

static void ctrl_key(const struct input_event *ev) {
    bool shift = ev->mods & MOD_SHIFT;
    coalesce_ok = false;
    switch (ev->keycode) {
    case 's': if (shift) open_prompt(P_SAVEAS, path[0] ? path : DEFAULT_SAVE_PATH); else save(); break;
    case 'n': if (modified) { pend = PA_NEW; prompt = P_DISCARD; } else new_document(); break;
    case 'o':
        if (modified) { pend = PA_OPEN_PROMPT; prompt = P_DISCARD; }
        else open_prompt(P_OPEN, path[0] ? path : "/disk/");
        break;
    case 'f': open_prompt(P_FIND, find_q); break;
    case 'g': open_prompt(P_GOTO, NULL); break;
    case 'w': wrap = !wrap; left = 0; break;
    case 'a': anchor = 0; cur = len; sel_on = len > 0; break;
    case 'c': do_copy(false); break;
    case 'x': do_copy(true); break;
    case 'v': do_paste(); break;
    case 'z': do_undo(shift); break;
    case 'y': do_undo(true); break;
    case KEY_LEFT:  begin_move(shift); cur = word_left(cur); want_col = cur - line_start(cur); break;
    case KEY_RIGHT: begin_move(shift); cur = word_right(cur); want_col = cur - line_start(cur); break;
    case KEY_HOME:  begin_move(shift); cur = 0; want_col = 0; break;
    case KEY_END:   begin_move(shift); cur = len; want_col = cur - line_start(cur); break;
    default: break;
    }
}

static void key(struct window *w, const struct input_event *ev) {
    if (!ev->pressed) return;
    if (prompt != P_NONE) { prompt_key(ev); repaint(); return; }
    if (ev->mods & MOD_CTRL) { ctrl_key(ev); repaint(); return; }
    bool shift = ev->mods & MOD_SHIFT;
    bool horiz = false;   // cursor column changed by this key
    int page = text_rows(w) - 1;
    if (page < 1) page = 1;
    switch (ev->keycode) {
    case KEY_LEFT:
        if (!shift && has_sel()) { cur = sel_lo(); sel_on = false; }
        else { begin_move(shift); if (cur > 0) cur--; }
        horiz = true; coalesce_ok = false; break;
    case KEY_RIGHT:
        if (!shift && has_sel()) { cur = sel_hi(); sel_on = false; }
        else { begin_move(shift); if (cur < len) cur++; }
        horiz = true; coalesce_ok = false; break;
    case KEY_UP:    begin_move(shift); move_vertical(-1); coalesce_ok = false; break;
    case KEY_DOWN:  begin_move(shift); move_vertical(1); coalesce_ok = false; break;
    case KEY_PGUP:  begin_move(shift); for (int i = 0; i < page; i++) move_vertical(-1); coalesce_ok = false; break;
    case KEY_PGDN:  begin_move(shift); for (int i = 0; i < page; i++) move_vertical(1); coalesce_ok = false; break;
    case KEY_HOME:  begin_move(shift); cur = line_start(cur); horiz = true; coalesce_ok = false; break;
    case KEY_END:   begin_move(shift); cur = line_end(cur); horiz = true; coalesce_ok = false; break;
    case KEY_ESC:   sel_on = false; coalesce_ok = false; break;
    case KEY_ENTER: replace_sel("\n", 1, false); coalesce_ok = false; horiz = true; break;
    case KEY_TAB:   replace_sel("    ", TAB_WIDTH, false); coalesce_ok = false; horiz = true; break;
    case KEY_BACKSPACE:
        if (has_sel()) replace_sel(NULL, 0, false);
        else if (cur > 0) ed_delete(cur - 1, cur, new_gid());
        coalesce_ok = false; horiz = true; break;
    case KEY_DELETE:
        if (has_sel()) replace_sel(NULL, 0, false);
        else if (cur < len) ed_delete(cur, cur + 1, new_gid());
        coalesce_ok = false; horiz = true; break;
    default:
        if (ev->ascii >= 32 && ev->ascii < 127) { replace_sel(&ev->ascii, 1, true); horiz = true; }
        else return;
    }
    if (horiz) want_col = cur - line_start(cur);
    repaint();
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    if (wheel) {
        top -= wheel * 3;
        int total = total_vrows();
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
    size_t ls, le;
    int sub;
    locate(top + row, &ls, &le, &sub);
    size_t base = wrap ? (size_t)sub * (size_t)text_cols(w) : (size_t)left;
    size_t c = (size_t)(col < 0 ? 0 : col) + base;
    size_t ll = le - ls;
    cur = ls + (c < ll ? c : ll);
    want_col = cur - ls;
    coalesce_ok = false;
    sel_on = false;
    if (cur == last_click_cur && now_ms - last_click_ms < DBLCLICK_MS) {
        select_word_at(cur);
        last_click_cur = (size_t)-1;     // a third click starts over
    } else {
        last_click_cur = cur;
        last_click_ms = now_ms;
    }
    repaint();
}

static void tick(struct window *w, uint64_t t_ms, bool new_second) {
    (void)new_second;
    now_ms = t_ms;
    if (!focused || w->minimized) return;
    uint64_t phase = t_ms / 500;
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
    .title = title,
    .key = key,
    .mouse = mouse,
    .tick = tick,
    .focus = focus,
};
