// "Files": list-view file browser over the VFS.
// Toolbar (up button + path), sorted listing, status bar. Single selection;
// double click / Enter opens (directory -> navigate, file -> Editor).
#include "../wm/wm.h"
#include "vfs.h"
#include "mm.h"
#include "arch.h"
#include "input.h"
#include "string.h"
#include "kprintf.h"

// Provided by the Editor app (apps.h once it lands).
extern struct app app_editor;
void editor_open(const char *path);

#define MAX_ENTRIES   512
#define NAME_MAX_KEEP 96
#define TOOLBAR_H     38
#define STATUS_H      24
#define ROW_H         26
#define DBLCLICK_MS   400

#define C_TOOLBAR     RGB(0xf6, 0xf6, 0xf8)
#define C_BTN         RGB(0xe4, 0xe4, 0xe9)
#define C_BTN_BORDER  RGB(0xcf, 0xcf, 0xd5)
#define C_SEL_INACT   RGB(0xd9, 0xd9, 0xde)
#define C_FOLDER_TOP  RGB(0x6e, 0xc4, 0xff)
#define C_FOLDER_BOT  RGB(0x2e, 0x93, 0xf5)
#define C_FOLDER_TAB  RGB(0x4a, 0xa9, 0xf7)
#define C_PAGE        RGB(0xff, 0xff, 0xff)
#define C_PAGE_EDGE   RGB(0xb4, 0xb4, 0xbc)
#define C_PAGE_LINE   RGB(0xc8, 0xc8, 0xd0)

struct entry {
    char name[NAME_MAX_KEEP];
    bool dir;
    uint64_t size;
};

static struct {
    char path[VFS_PATH_MAX];
    struct entry *ents;
    int count;
    int sel;
    int scroll;           // first visible row
    int err;              // vfs error of last listing (0 = ok)
    bool truncated;
    bool readonly;
    bool have_disk;
    int last_click;       // row of the previous click (-1 none)
    uint64_t last_click_ms;
} F;

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ci_cmp(const char *a, const char *b) {
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

static bool before(const struct entry *a, const struct entry *b) {
    if (a->dir != b->dir) return a->dir;
    int c = ci_cmp(a->name, b->name);
    if (c) return c < 0;
    return strcmp(a->name, b->name) < 0;
}

static void sort_entries(void) {
    // shell sort
    for (int gap = F.count / 2; gap > 0; gap /= 2) {
        for (int i = gap; i < F.count; i++) {
            struct entry t = F.ents[i];
            int j = i;
            while (j >= gap && before(&t, &F.ents[j - gap])) { F.ents[j] = F.ents[j - gap]; j -= gap; }
            F.ents[j] = t;
        }
    }
}

static void load(void) {
    F.count = 0; F.sel = -1; F.scroll = 0; F.err = 0; F.truncated = false; F.last_click = -1;
    struct vfs_stat st;
    F.readonly = vfs_stat(F.path, &st) == 0 && st.readonly;
    F.have_disk = vfs_stat("/disk", &st) == 0;
    if (!F.ents) {
        F.ents = kmalloc(sizeof(struct entry) * MAX_ENTRIES);
        if (!F.ents) { F.err = VFS_ENOMEM; return; }
    }
    struct vfs_file *d;
    int r = vfs_open(F.path, VFS_O_READ, &d);
    if (r < 0) { F.err = r; return; }
    static struct vfs_dirent de;
    while (vfs_readdir(d, &de) == 1) {
        if (F.count >= MAX_ENTRIES) { F.truncated = true; break; }
        struct entry *e = &F.ents[F.count++];
        size_t n = strlen(de.name);
        if (n >= NAME_MAX_KEEP) n = NAME_MAX_KEEP - 1;
        memcpy(e->name, de.name, n);
        e->name[n] = 0;
        e->dir = de.type == VFS_TYPE_DIR;
        e->size = de.size;
    }
    vfs_close(d);
    sort_entries();
    if (F.count) F.sel = 0;
}

static void go_up(void) {
    if (!strcmp(F.path, "/")) return;
    char *slash = F.path + strlen(F.path);
    while (slash > F.path && *slash != '/') slash--;
    if (slash == F.path) F.path[1] = 0; else *slash = 0;
    load();
}

static void open_sel(void) {
    if (F.sel < 0 || F.sel >= F.count) return;
    struct entry *e = &F.ents[F.sel];
    char np[VFS_PATH_MAX];
    int n = ksnprintf(np, sizeof np, "%s%s%s", F.path, strcmp(F.path, "/") ? "/" : "", e->name);
    if (n < 0 || n >= (int)sizeof np) { F.err = VFS_ENAMETOOLONG; return; }
    if (e->dir) {
        strcpy(F.path, np);
        load();
    } else {
        editor_open(np);
        wm_open(&app_editor);
    }
}

static int view_rows(struct surface *s) {
    int h = s->h - TOOLBAR_H - STATUS_H;
    return h > 0 ? h / ROW_H : 0;
}

static void clamp_scroll(struct surface *s) {
    int vr = view_rows(s);
    if (F.scroll > F.count - vr) F.scroll = F.count - vr;
    if (F.scroll < 0) F.scroll = 0;
}

static void ensure_visible(struct surface *s) {
    int vr = view_rows(s);
    if (F.sel < F.scroll) F.scroll = F.sel;
    else if (F.sel >= F.scroll + vr) F.scroll = F.sel - vr + 1;
    clamp_scroll(s);
}

static void human_size(char *buf, size_t n, uint64_t b) {
    if (b < 1024) ksnprintf(buf, n, "%u B", (unsigned)b);
    else if (b < 1024 * 1024) ksnprintf(buf, n, "%u.%u KB", (unsigned)(b >> 10), (unsigned)((b & 1023) * 10 >> 10));
    else if (b < 1024ull * 1024 * 1024) ksnprintf(buf, n, "%u.%u MB", (unsigned)(b >> 20), (unsigned)((b & 0xfffff) * 10 >> 20));
    else ksnprintf(buf, n, "%u.%u GB", (unsigned)(b >> 30), (unsigned)((b & 0x3fffffff) * 10 >> 30));
}

static void draw_folder(struct surface *s, int x, int y) {      // 18x15 at x,y
    gfx_fill_rrect(s, rect_make(x, y, 8, 6), 2, C_FOLDER_TAB);
    gfx_gradient_rrect(s, rect_make(x, y + 2, 18, 13), 3, C_FOLDER_TOP, C_FOLDER_BOT);
}

static void draw_file(struct surface *s, int x, int y) {        // 13x16 at x,y
    gfx_fill_rrect(s, rect_make(x, y, 13, 16), 2, C_PAGE_EDGE);
    gfx_fill_rrect(s, rect_make(x + 1, y + 1, 11, 14), 1, C_PAGE);
    for (int i = 0; i < 3; i++) gfx_hline(s, x + 3, y + 5 + i * 3, 7, C_PAGE_LINE);
}

static void paint(struct window *w) {
    struct surface *s = &w->client;
    gfx_reset_clip(s);
    gfx_fill(s, rect_make(0, 0, s->w, s->h), WIN_BG);

    // toolbar
    gfx_fill(s, rect_make(0, 0, s->w, TOOLBAR_H), C_TOOLBAR);
    gfx_hline(s, 0, TOOLBAR_H - 1, s->w, C_SEPARATOR);
    bool at_root = !strcmp(F.path, "/");
    struct rect up = rect_make(10, 7, 30, 24);
    gfx_fill_rrect(s, up, 6, C_BTN);
    gfx_stroke_rrect(s, up, 6, C_BTN_BORDER);
    color_t ac = at_root ? C_TEXT_INACTIVE : C_TEXT_2;
    int cx = up.x + 15, cy = up.y + 12;           // chevron pointing up
    for (int t = 0; t < 2; t++) {
        gfx_line(s, cx - 5, cy + 3 + t, cx, cy - 2 + t, ac);
        gfx_line(s, cx, cy - 2 + t, cx + 5, cy + 3 + t, ac);
    }
    struct rect pr = rect_make(50, 0, s->w - 60, TOOLBAR_H - 1);
    gfx_set_clip(s, pr);
    const char *title = at_root ? "/" : F.path;
    int tw = text_width(&afont_ui_bold, title);
    int tx = tw > pr.w ? pr.x + pr.w - tw : pr.x;     // keep the tail visible when long
    text_draw(s, &afont_ui_bold, tx, (TOOLBAR_H - afont_ui_bold.height) / 2, title, C_TEXT);
    gfx_reset_clip(s);

    // list
    struct rect lr = rect_make(0, TOOLBAR_H, s->w, s->h - TOOLBAR_H - STATUS_H);
    gfx_set_clip(s, lr);
    int vr = view_rows(s) + 1;
    if (F.err) {
        char m[96];
        ksnprintf(m, sizeof m, "Cannot read folder: %s", vfs_strerror(F.err));
        text_center(s, &afont_ui, 0, s->w, TOOLBAR_H + 40, m, C_TEXT_3);
    } else if (!F.count) {
        text_center(s, &afont_ui, 0, s->w, TOOLBAR_H + 40, "This folder is empty", C_TEXT_3);
    }
    bool focused = w == wm_focused();
    for (int i = 0; i < vr && F.scroll + i < F.count; i++) {
        int idx = F.scroll + i;
        struct entry *e = &F.ents[idx];
        int y = TOOLBAR_H + i * ROW_H;
        bool sel = idx == F.sel;
        color_t fg = C_TEXT, fg2 = C_TEXT_3;
        if (sel) {
            gfx_fill_rrect(s, rect_make(6, y + 1, s->w - 12, ROW_H - 2), 6, focused ? C_ACCENT : C_SEL_INACT);
            if (focused) fg = fg2 = RGB(0xff, 0xff, 0xff);
        }
        if (e->dir) draw_folder(s, 16, y + 6); else draw_file(s, 18, y + 5);
        int ty = y + (ROW_H - afont_ui.height) / 2;
        char sz[24] = "";
        int szw = 0;
        if (!e->dir) { human_size(sz, sizeof sz, e->size); szw = text_width(&afont_small, sz); }
        int name_r = s->w - 16 - (szw ? szw + 12 : 0);
        gfx_set_clip(s, rect_intersect(lr, rect_make(44, y, name_r - 44, ROW_H)));
        text_draw(s, &afont_ui, 44, ty, e->name, fg);
        gfx_set_clip(s, lr);
        if (szw) text_draw(s, &afont_small, s->w - 16 - szw, y + (ROW_H - afont_small.height) / 2, sz, fg2);
    }
    // scroll indicator
    int vis = view_rows(s);
    if (F.count > vis && vis > 0) {
        int th = lr.h * vis / F.count; if (th < 20) th = 20;
        int ty = lr.y + (lr.h - th) * F.scroll / (F.count - vis);
        gfx_fill_rrect(s, rect_make(s->w - 6, ty, 4, th), 2, RGBA(0x00, 0x00, 0x00, 0x40));
    }
    gfx_reset_clip(s);

    // status bar
    int sy = s->h - STATUS_H;
    gfx_fill(s, rect_make(0, sy, s->w, STATUS_H), C_TOOLBAR);
    gfx_hline(s, 0, sy, s->w, C_SEPARATOR);
    char st[64];
    ksnprintf(st, sizeof st, "%d item%s%s", F.count, F.count == 1 ? "" : "s", F.truncated ? "+ (truncated)" : "");
    int sty = sy + (STATUS_H - afont_small.height) / 2;
    text_draw(s, &afont_small, 12, sty, st, C_TEXT_3);
    const char *right = F.readonly ? "Read-only" : "";
    if (!F.have_disk) right = F.readonly ? "Read-only  |  No disk mounted" : "No disk mounted";
    int rw = text_width(&afont_small, right);
    text_draw(s, &afont_small, s->w - 12 - rw, sty, right, C_TEXT_3);

    wm_invalidate_all(w);
}

static void init(struct window *w) {
    (void)w;
    strcpy(F.path, "/");
    load();
    kprintf("[OK] files\n");
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    (void)buttons;
    struct surface *s = &w->client;
    if (wheel) {
        F.scroll -= wheel * 3;
        clamp_scroll(s);
        paint(w);
        return;
    }
    if (y < TOOLBAR_H) {
        if (x >= 10 && x < 40 && y >= 7 && y < 31) { go_up(); paint(w); }
        return;
    }
    if (y >= s->h - STATUS_H) return;
    int idx = F.scroll + (y - TOOLBAR_H) / ROW_H;
    if (idx < 0 || idx >= F.count) { F.sel = -1; F.last_click = -1; paint(w); return; }
    uint64_t now = timer_ticks_ms();
    if (idx == F.last_click && now - F.last_click_ms <= DBLCLICK_MS) {
        F.sel = idx;
        F.last_click = -1;
        open_sel();
    } else {
        F.sel = idx;
        F.last_click = idx;
        F.last_click_ms = now;
    }
    paint(w);
}

static void key(struct window *w, const struct input_event *ev) {
    if (!ev->pressed) return;
    struct surface *s = &w->client;
    switch (ev->keycode) {
    case KEY_UP:   if (F.count) { F.sel = F.sel <= 0 ? 0 : F.sel - 1; ensure_visible(s); } break;
    case KEY_DOWN: if (F.count) { F.sel = F.sel < 0 ? 0 : (F.sel + 1 < F.count ? F.sel + 1 : F.sel); ensure_visible(s); } break;
    case KEY_HOME: if (F.count) { F.sel = 0; ensure_visible(s); } break;
    case KEY_END:  if (F.count) { F.sel = F.count - 1; ensure_visible(s); } break;
    case KEY_ENTER: open_sel(); break;
    case KEY_BACKSPACE: go_up(); break;
    default: return;
    }
    F.last_click = -1;
    paint(w);
}

// Dock icon glyph: a small white folder.
static void glyph(struct surface *s, struct rect r) {
    int u = r.w / 8;
    gfx_fill_rrect(s, rect_make(r.x + 2 * u, r.y + 2 * u + u / 2, 2 * u + u / 2, u + 2), u / 2, RGB(0xff, 0xff, 0xff));
    gfx_fill_rrect(s, rect_make(r.x + 2 * u, r.y + 3 * u, 4 * u, 3 * u), u / 2, RGB(0xff, 0xff, 0xff));
}

struct app app_files = {
    .name = "Files",
    .client_w = 560, .client_h = 380,
    .icon_top = RGB(0x6c, 0xcf, 0xff), .icon_bottom = RGB(0x0a, 0x7a, 0xff),
    .icon_glyph = glyph,
    .init = init,
    .paint = paint,
    .mouse = mouse,
    .key = key,
};
