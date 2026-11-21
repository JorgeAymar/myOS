#pragma once
// Tiny tolerant HTML layout engine for the Browser (streaming: no DOM).
// Text is wrapped to a fixed width into a flat list of positioned runs.
#include <stdint.h>
#include <stdbool.h>
#include "../gfx/gfx.h"

#define HTML_MAX_INPUT 262144
#define HTML_MAX_LINKS 128
#define HTML_LINK_LEN  192

#define HTML_MAX_FORMS 8
#define HTML_MAX_CTLS  64
#define HTML_VAL_LEN   256
#define HTML_NAME_LEN  40
#define HTML_MAX_OPTS  256
#define HTML_SEL_OPTS  24

enum { HC_TEXT, HC_PASSWORD, HC_HIDDEN, HC_CHECK, HC_RADIO, HC_SUBMIT, HC_BUTTON, HC_SELECT, HC_TEXTAREA };

struct hform { char action[HTML_LINK_LEN]; uint8_t post; };
struct hopt { char value[48]; char label[40]; };
struct hctl {
    uint8_t type;
    bool checked, disabled, vis;    // vis: has a run on the page
    int8_t form;                    // index into hdoc.forms or -1
    uint8_t maxlen;                 // 0 = unlimited (up to HTML_VAL_LEN-1)
    int16_t len, cur;               // text length / caret (bytes)
    uint16_t opt_first; uint8_t nopt, sel;   // select: options live in hdoc.opts
    char name[HTML_NAME_LEN];
    char ph[40];                    // placeholder, or <button> label
    char value[HTML_VAL_LEN];       // raw bytes (as submitted)
};

struct hrun {
    int16_t x, w;
    int32_t y;            // document coordinates (line box top)
    uint32_t off;         // text offset into hdoc.pool
    color_t color;
    int16_t link;         // index into hdoc.links or -1
    int8_t ctl;           // index into hdoc.ctl (kind K_CTL) or -1
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
    struct hform forms[HTML_MAX_FORMS]; int nforms;
    struct hctl *ctl; int nctl;
    struct hopt *opts; int nopts;
    int focus;                      // focused control or -1 (set by the Browser)
    bool caret;                     // caret blink phase
};

struct hdoc *html_new(void);                       // NULL on OOM
void html_free(struct hdoc *d);
// Lays out `html` (NUL-terminated) for a page `width` px wide. plain: text/plain.
void html_layout(struct hdoc *d, const char *html, int width, bool plain);
// Draws the document into view (surface coordinates), scrolled by `scroll` px.
void html_draw(const struct hdoc *d, struct surface *s, struct rect view, int scroll);
// Link index under (x, y) in document coordinates, or -1.
int html_link_at(const struct hdoc *d, int x, int y);

// ---- forms ----
int html_ctl_at(const struct hdoc *d, int x, int y);                 // control under the point or -1
bool html_ctl_rect(const struct hdoc *d, int ci, struct rect *r);    // document coordinates
int html_ctl_next(const struct hdoc *d, int from, int dir);          // next focusable control (wraps) or -1
int html_ctl_caret_at(const struct hdoc *d, int ci, int relx);       // byte offset for a click at relx inside the box
void html_ctl_toggle(struct hdoc *d, int ci);                        // checkbox toggle / radio select
void html_ctl_step(struct hdoc *d, int ci, int dir);                 // <select>: next/previous option (wraps)
int html_form_default_button(const struct hdoc *d, int form);        // first submit control of a form or -1
// Builds `action?name=value&...` for a GET submission into out (outsz bytes). An existing query/fragment in
// `action` is dropped. submitter = clicked submit control or -1. Never overflows; a value that does not fit
// is truncated, hidden fields that do not fit are dropped. Returns false only if even the action does not fit.
bool html_form_build_url(const struct hdoc *d, int form, int submitter, const char *action, char *out, int outsz);
