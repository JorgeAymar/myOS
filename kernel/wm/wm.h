#pragma once
// myOS compositor + window manager internals (shared by kernel/wm and kernel/apps).
#include <stdint.h>
#include <stdbool.h>
#include "input.h"
#include "../gfx/gfx.h"
#include "../gfx/font.h"
#include "theme.h"

struct window;

// An in-kernel application. Each app has at most one window, created lazily
// on first open and reused afterwards (its buffer is never freed, which keeps
// the bump allocator of isolated builds happy and makes reopening instant).
struct app {
    const char *name;
    int client_w, client_h;
    color_t icon_top, icon_bottom;                         // icon gradient
    void (*icon_glyph)(struct surface *s, struct rect r);  // draws on top of the gradient
    void (*init)(struct window *w);                        // once, after the window exists
    void (*paint)(struct window *w);                       // full repaint of w->client
    void (*key)(struct window *w, const struct input_event *ev);
    void (*mouse)(struct window *w, int x, int y, uint8_t buttons, int wheel);
    void (*tick)(struct window *w, uint64_t now_ms, bool new_second);
    void (*focus)(struct window *w, bool focused);
    const char *(*title)(struct window *w);                // optional: title bar text (default: name); call wm_paint_titlebar(w) after it changes
    struct window *win;
};

enum { BTN_NONE = 0, BTN_CLOSE, BTN_MIN, BTN_ZOOM };

struct window {
    struct app *app;
    struct rect frame;        // on screen, including title bar
    struct rect drawn;        // frame as of the last composite (for damage)
    struct surface surf;      // whole window, opaque
    struct surface client;    // view below the title bar
    bool open, minimized;
    int hover_btn, press_btn;    // hover_btn != 0: pointer over the control group
    bool drawn_focused;
};

// ---- compositor (compositor.c) ----
struct screen {
    int w, h;
    struct surface back;      // back buffer
    struct surface wall;      // cached wallpaper
    struct surface frost_top; // blurred + tinted wallpaper behind the menu bar
    struct surface frost_dock;// blurred + tinted wallpaper band behind the dock
    int frost_dock_y;         // screen y of frost_dock row 0
    uint8_t *fb;
    long fb_pitch;
};
extern struct screen scr;
bool comp_init(void);
void comp_damage(struct rect r);
void comp_damage_all(void);
void comp_render(void);                  // composites + presents every dirty rect
void wallpaper_render(struct surface *s); // wallpaper.c

// ---- windows (window.c) ----
#define MAX_WINDOWS 12
extern struct window *zorder[MAX_WINDOWS];   // bottom .. top
extern int nwindows;
struct window *wm_open(struct app *a);        // create or restore + focus
void wm_close(struct window *w);
void wm_minimize(struct window *w);
void wm_focus(struct window *w);              // raise + focus
struct window *wm_focused(void);              // top-most visible window or NULL
struct window *wm_window_at(int x, int y);
void wm_invalidate(struct window *w, struct rect client_r);  // client coordinates
void wm_invalidate_all(struct window *w);
void wm_paint_titlebar(struct window *w);
int  wm_button_at(struct window *w, int x, int y);           // screen coords
bool wm_in_titlebar(struct window *w, int x, int y);
void wm_move(struct window *w, int x, int y);
void wm_draw(struct surface *b, struct window *w);
struct rect wm_bounds(struct window *w);      // frame + shadow
void wm_sync_damage(void);                    // damage moved windows

// ---- shell: top bar, menu, dock, cursor (shell.c) ----
void shell_init(void);
void shell_draw(struct surface *b);
void shell_draw_cursor(struct surface *b);
bool shell_mouse_down(int x, int y);          // true if consumed
void shell_mouse_move(int x, int y);
void shell_clock_update(void);                // called when the RTC second changes
void shell_windows_changed(void);             // dock indicators / title
void app_icon_paint(struct surface *s, struct rect r, const struct app *a);
extern int cursor_x, cursor_y;
struct rect cursor_rect(int x, int y);

// ---- apps (kernel/apps) ----
// Apps with client_w == 0 are dock placeholders (shown as "coming soon").
extern struct app app_welcome, app_notes, app_clock;
extern struct app app_terminal, app_files, app_editor, app_settings, app_about, app_browser;
// Shared icon glyph painters (shell.c)
extern void (*const icon_glyph_welcome)(struct surface *, struct rect);
extern void (*const icon_glyph_notes)(struct surface *, struct rect);
extern void (*const icon_glyph_clock)(struct surface *, struct rect);
extern void (*const icon_glyph_files)(struct surface *, struct rect);
extern void (*const icon_glyph_terminal)(struct surface *, struct rect);
extern void (*const icon_glyph_editor)(struct surface *, struct rect);
extern void (*const icon_glyph_settings)(struct surface *, struct rect);
extern void (*const icon_glyph_info)(struct surface *, struct rect);
extern void (*const icon_glyph_browser)(struct surface *, struct rect);
