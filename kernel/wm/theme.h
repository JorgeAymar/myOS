#pragma once
// myOS visual identity: palette, spacing and metrics in one place.
// Current look: light, desktop-classic (frosted menu bar, floating dock,
// three-light window controls). All artwork is procedural and original.
#include "../gfx/gfx.h"

// ---- accent ----
#define C_ACCENT          RGB(0x00, 0x7a, 0xff)   // blue

// ---- wallpaper: layered flowing waves, blue -> purple -> pink ----
#define WP_SKY_TOP        RGB(0x2b, 0x3f, 0xb8)
#define WP_SKY_BOTTOM     RGB(0x8e, 0x6c, 0xe6)
#define WP_GLOW           RGB(0xff, 0xc6, 0xe6)
#define WP_WAVE_COUNT     5

// ---- text ----
#define C_TEXT            RGB(0x1d, 0x1d, 0x1f)
#define C_TEXT_2          RGB(0x4d, 0x4d, 0x4d)   // secondary / window titles
#define C_TEXT_3          RGB(0x86, 0x86, 0x8b)   // tertiary / placeholders
#define C_TEXT_INACTIVE   RGB(0xa8, 0xa8, 0xac)
#define C_SEPARATOR       RGB(0xe5, 0xe5, 0xea)

// ---- frosted glass (blurred wallpaper + white tint, precomputed) ----
#define BLUR_RADIUS       9
#define BLUR_PASSES       3
#define MENUBAR_TINT      RGBA(0xff, 0xff, 0xff, 0xb8)   // ~72% white
#define DOCK_TINT         RGBA(0xff, 0xff, 0xff, 0x73)   // ~45% white

// ---- menu bar ----
#define MENUBAR_H         24
#define MENUBAR_LINE      RGBA(0x00, 0x00, 0x00, 0x14)
#define MENUBAR_PAD       14
#define MENUBAR_ITEM_GAP  20
#define MENUBAR_SEL       RGBA(0x00, 0x00, 0x00, 0x1a)   // pressed title

// ---- dropdown menu ----
#define MENU_W            220
#define MENU_ITEM_H       22
#define MENU_RADIUS       8
#define MENU_BG           RGBA(0xf6, 0xf6, 0xf6, 0xf4)
#define MENU_BORDER       RGBA(0x00, 0x00, 0x00, 0x26)
#define MENU_HOVER        C_ACCENT

// ---- dock ----
#define DOCK_ICON         48
#define DOCK_GAP          8
#define DOCK_PAD          7
#define DOCK_RADIUS       18
#define DOCK_BOTTOM       8        // distance from the screen edge
#define DOCK_BAND_H       80       // frosted band cached behind the dock
#define DOCK_MAG_PCT      40       // hovered icon grows by 40%
#define DOCK_BORDER       RGBA(0xff, 0xff, 0xff, 0x80)
#define DOCK_OUTLINE      RGBA(0x00, 0x00, 0x00, 0x1c)
#define DOCK_DOT          RGB(0x1d, 0x1d, 0x1f)
#define DOCK_SEP          RGBA(0x00, 0x00, 0x00, 0x26)
#define ICON_RADIUS       11       // at 48 px, scales with size
#define TOOLTIP_BG        RGBA(0xf2, 0xf2, 0xf2, 0xf0)

// ---- windows ----
#define WIN_RADIUS        10
#define WIN_TITLE_H       28
#define WIN_BG            RGB(0xff, 0xff, 0xff)
#define WIN_TITLE_TOP     RGB(0xec, 0xec, 0xec)
#define WIN_TITLE_BOTTOM  RGB(0xe3, 0xe3, 0xe3)
#define WIN_TITLE_IDLE    RGB(0xf6, 0xf6, 0xf6)
#define WIN_TITLE_SEP     RGB(0xd1, 0xd1, 0xd1)
#define WIN_TITLE_SEP_IDLE RGB(0xe4, 0xe4, 0xe4)
#define WIN_BORDER        RGBA(0x00, 0x00, 0x00, 0x26)   // 15% black
#define WIN_BORDER_IDLE   RGBA(0x00, 0x00, 0x00, 0x1c)
#define WIN_SHADOW        26       // blur spread
#define WIN_SHADOW_Y      10
#define WIN_SHADOW_A      0x5a     // ~35% black at contact
#define WIN_SHADOW_IDLE_A 0x36

// ---- window controls (three lights, left side) ----
#define LIGHT_D           12       // diameter
#define LIGHT_GAP         8
#define LIGHT_X0          12       // left edge of the first light
#define LIGHT_CLOSE       RGB(0xff, 0x5f, 0x57)
#define LIGHT_MIN         RGB(0xfe, 0xbc, 0x2e)
#define LIGHT_ZOOM        RGB(0x28, 0xc8, 0x40)
#define LIGHT_RING_DARKEN 0x30     // ring = light color darkened
#define LIGHT_IDLE        RGB(0xd0, 0xd0, 0xd0)
#define LIGHT_GLYPH       RGBA(0x00, 0x00, 0x00, 0x99)

// ---- cursor ----
#define CURSOR_FG         RGB(0x00, 0x00, 0x00)
#define CURSOR_OUTLINE    RGB(0xff, 0xff, 0xff)

// ---- dark palette (kept for a future appearance switch) ----
#define DARK_TEXT         RGB(0xf5, 0xf5, 0xf7)
#define DARK_WIN_BG       RGB(0x1e, 0x1e, 0x1e)
#define DARK_TITLE_TOP    RGB(0x3a, 0x3a, 0x3a)
#define DARK_TITLE_BOTTOM RGB(0x32, 0x32, 0x32)
#define DARK_MENUBAR_TINT RGBA(0x1e, 0x1e, 0x1e, 0xb8)
#define DARK_DOCK_TINT    RGBA(0x28, 0x28, 0x28, 0x99)
#define DARK_BORDER       RGBA(0xff, 0xff, 0xff, 0x26)
