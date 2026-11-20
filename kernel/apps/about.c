// About: placeholder until phase 11 fills it in (client_w == 0: not openable).
#include "../wm/wm.h"

static void glyph(struct surface *s, struct rect r) { icon_glyph_info(s, r); }

struct app app_about = {
    .name = "About",
    .icon_top = RGB(0xe0, 0xe0, 0xe6), .icon_bottom = RGB(0x9a, 0x9a, 0xa2),
    .icon_glyph = glyph,
};
