// Terminal: placeholder until phase 11 fills it in (client_w == 0: not openable).
#include "../wm/wm.h"

static void glyph(struct surface *s, struct rect r) { icon_glyph_terminal(s, r); }

struct app app_terminal = {
    .name = "Terminal",
    .icon_top = RGB(0x5a, 0x5a, 0x60), .icon_bottom = RGB(0x1c, 0x1c, 0x1e),
    .icon_glyph = glyph,
};
