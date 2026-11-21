// desktop_init / desktop_frame / desktop_run: input routing, timekeeping and
// the frame loop. Everything visual goes through comp_damage + comp_render.
#include "desktop.h"
#include "wm.h"
#include "arch.h"
#include "rtc.h"
#include "kprintf.h"
#include "net.h"

static bool ready;
static uint8_t buttons;
static struct window *drag_win;
static int drag_dx, drag_dy;
static struct window *btn_win;      // window whose title button is pressed
static int drawn_cx, drawn_cy;      // cursor position as of the last frame
static uint64_t last_rtc_ms;
static uint8_t last_second = 0xff;

static void set_hover_btn(struct window *w, int b) {
    if (w->hover_btn == b) return;
    w->hover_btn = b;
    wm_paint_titlebar(w);
}

static void mouse_down(int x, int y) {
    if (shell_mouse_down(x, y)) return;
    struct window *w = wm_window_at(x, y);
    if (!w) return;
    wm_focus(w);
    int b = wm_button_at(w, x, y);
    if (b) { w->press_btn = b; btn_win = w; wm_paint_titlebar(w); return; }
    if (wm_in_titlebar(w, x, y)) {
        drag_win = w;
        drag_dx = x - w->frame.x;
        drag_dy = y - w->frame.y;
        return;
    }
    if (w->app->mouse)
        w->app->mouse(w, x - w->frame.x, y - w->frame.y - WIN_TITLE_H, buttons, 0);
}

static void mouse_up(int x, int y) {
    drag_win = 0;
    if (btn_win) {
        struct window *w = btn_win;
        int b = w->press_btn;
        w->press_btn = BTN_NONE;
        btn_win = 0;
        if (w->open && wm_button_at(w, x, y) == b) {
            w->hover_btn = BTN_NONE;
            if (b == BTN_CLOSE) wm_close(w);
            else if (b == BTN_MIN) wm_minimize(w);
            else wm_move(w, (scr.w - w->frame.w) / 2, (scr.h - w->frame.h) / 2 - 20);  // zoom: recenter
        } else if (w->open) {
            wm_paint_titlebar(w);
        }
    }
}

static void mouse_moved(int x, int y) {
    if (drag_win) { wm_move(drag_win, x - drag_dx, y - drag_dy); return; }
    shell_mouse_move(x, y);
    // title button hover on the window under the cursor only
    struct window *top = wm_window_at(x, y);
    for (int i = 0; i < nwindows; i++) {
        struct window *w = zorder[i];
        set_hover_btn(w, w == top ? wm_button_at(w, x, y) : BTN_NONE);
    }
}

static void handle(const struct input_event *ev) {
    if (ev->type == INPUT_MOUSE) {
        int x = cursor_x + ev->dx, y = cursor_y + ev->dy;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > scr.w - 1) x = scr.w - 1;
        if (y > scr.h - 1) y = scr.h - 1;
        bool moved = x != cursor_x || y != cursor_y;
        cursor_x = x; cursor_y = y;
        if (moved) mouse_moved(x, y);
        uint8_t pressed = ev->buttons & ~buttons, released = buttons & ~ev->buttons;
        buttons = ev->buttons;
        if (pressed & MOUSE_LEFT) mouse_down(x, y);
        if (released & MOUSE_LEFT) mouse_up(x, y);
        if (ev->wheel) {
            struct window *w = wm_window_at(x, y);
            if (w && w->app->mouse)
                w->app->mouse(w, x - w->frame.x, y - w->frame.y - WIN_TITLE_H, buttons, ev->wheel);
        }
    } else if (ev->type == INPUT_KEY) {
        struct window *w = wm_focused();
        if (w && w->app->key) w->app->key(w, ev);
    }
}

void desktop_init(void) {
    if (!comp_init()) {
        kprintf("[WARN] desktop: compositor unavailable, running headless\n");
        return;
    }
    shell_init();
    // A second, smaller window behind the welcome window shows inactive styling.
    struct window *n = wm_open(&app_notes);
    if (n) { wm_move(n, scr.w / 2 - 470, scr.h / 2 - 250); n->drawn = n->frame; }
    struct window *w = wm_open(&app_welcome);
    if (w) { wm_move(w, (scr.w - w->frame.w) / 2 + 40, (scr.h - w->frame.h) / 2 - 30); w->drawn = w->frame; }
    drawn_cx = cursor_x; drawn_cy = cursor_y;
    comp_damage_all();
    ready = true;
    kprintf("[OK] desktop %dx%d, compositor + wm + shell, %d window(s)\n", scr.w, scr.h, nwindows);
}

void desktop_frame(void) {
    if (!ready) return;
    net_job_poll();
    net_poll_try();   // keep the network stack serviced (ARP/ICMP replies, TCP) after boot
    struct input_event ev;
    for (int n = 0; n < 512 && input_poll(&ev); n++) handle(&ev);

    uint64_t now = timer_ticks_ms();
    bool new_second = false;
    if (last_second == 0xff || now - last_rtc_ms >= 200) {
        last_rtc_ms = now;
        struct rtc_time t;
        rtc_read(&t);
        if (t.second != last_second) {
            last_second = t.second;
            new_second = true;
            shell_clock_update();
        }
    }
    for (int i = 0; i < nwindows; i++) {
        struct window *w = zorder[i];
        if (w->open && w->app->tick) w->app->tick(w, now, new_second);
    }

    wm_sync_damage();
    if (cursor_x != drawn_cx || cursor_y != drawn_cy) {
        comp_damage(cursor_rect(drawn_cx, drawn_cy));
        comp_damage(cursor_rect(cursor_x, cursor_y));
        drawn_cx = cursor_x; drawn_cy = cursor_y;
    }
    comp_render();
}

void desktop_run(void) {
    uint64_t next = timer_ticks_ms();
    for (;;) {
        desktop_frame();
        next += 16;
        uint64_t now = timer_ticks_ms();
        if (now < next) timer_sleep_ms(next - now);
        else next = now;   // running behind: don't try to catch up
    }
}
