// Editor: placeholder until phase 11 fills it in (client_w == 0: not openable).
#include "../wm/wm.h"

static void glyph(struct surface *s, struct rect r) { icon_glyph_editor(s, r); }

struct app app_editor = {
    .name = "Editor",
    .icon_top = RGB(0xff, 0xff, 0xff), .icon_bottom = RGB(0xd8, 0xdc, 0xe4),
    .icon_glyph = glyph,
};
