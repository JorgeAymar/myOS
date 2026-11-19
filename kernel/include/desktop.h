#pragma once
// Graphics + window manager + desktop shell. Owner: ui agent (kernel/gfx, kernel/wm).
#include <stdbool.h>

// Sets up the compositor on the Limine framebuffer and builds the desktop.
void desktop_init(void);
// Drains input events, updates state and composites one frame to the screen.
void desktop_frame(void);
// Main loop: desktop_frame() paced at ~60 fps, never returns.
__attribute__((noreturn)) void desktop_run(void);
