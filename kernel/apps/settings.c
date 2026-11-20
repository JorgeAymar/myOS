// Settings: placeholder until phase 11 fills it in (client_w == 0: not openable).
#include "../wm/wm.h"

static void glyph(struct surface *s, struct rect r) { icon_glyph_settings(s, r); }

struct app app_settings = {
    .name = "Settings",
    .icon_top = RGB(0xa8, 0xa8, 0xae), .icon_bottom = RGB(0x5e, 0x5e, 0x64),
    .icon_glyph = glyph,
};
