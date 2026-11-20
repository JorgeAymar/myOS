// Files: placeholder until phase 11 fills it in (client_w == 0: not openable).
#include "../wm/wm.h"

static void glyph(struct surface *s, struct rect r) { icon_glyph_files(s, r); }

struct app app_files = {
    .name = "Files",
    .icon_top = RGB(0x6c, 0xcf, 0xff), .icon_bottom = RGB(0x0a, 0x7a, 0xff),
    .icon_glyph = glyph,
};
