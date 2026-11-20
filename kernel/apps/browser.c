// Browser: placeholder until the HTTP browser lands (client_w == 0: not openable).
#include "../wm/wm.h"

static void glyph(struct surface *s, struct rect r) { icon_glyph_browser(s, r); }

struct app app_browser = {
    .name = "Browser",
    .icon_top = RGB(0x5e, 0xe0, 0xf6), .icon_bottom = RGB(0x1a, 0x6c, 0xf0),
    .icon_glyph = glyph,
};
