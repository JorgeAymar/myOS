#pragma once
// Tiny tolerant HTML layout engine for the Browser (streaming: no DOM).
// Text is wrapped to a fixed width into a flat list of positioned runs.
#include <stdint.h>
#include <stdbool.h>
#include "../gfx/gfx.h"

#define HTML_MAX_INPUT 262144
#define HTML_MAX_LINKS 128
#define HTML_LINK_LEN  192

struct hrun {
    int16_t x, w;
    int32_t y;            // document coordinates (line box top)
    uint32_t off;         // text offset into hdoc.pool
    color_t color;
    int16_t link;         // index into hdoc.links or -1
    uint8_t font, kind, ul;
};

struct hdoc {
    struct hrun *runs;
    int nruns;
    char *pool;
    int pool_len;
    char (*links)[HTML_LINK_LEN];   // raw (unresolved) hrefs
    int nlinks;
    int height;                     // total document height in pixels
    char title[96];
};

struct hdoc *html_new(void);                       // NULL on OOM
// Lays out `html` (NUL-terminated) for a page `width` px wide. plain: text/plain.
void html_layout(struct hdoc *d, const char *html, int width, bool plain);
// Draws the document into view (surface coordinates), scrolled by `scroll` px.
void html_draw(const struct hdoc *d, struct surface *s, struct rect view, int scroll);
// Link index under (x, y) in document coordinates, or -1.
int html_link_at(const struct hdoc *d, int x, int y);
