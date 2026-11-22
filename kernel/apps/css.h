#pragma once
// Small bounded CSS engine for the Browser. Integer-only (fixed point 1/100), no recursion
// on input, all allocation via kmalloc, everything released by css_free().
//
// Usage from the HTML layer:
//   struct css_sheet *sh = css_sheet_new();
//   css_parse(sh, style_text, style_len);          // once per <style> block, in document order
//   // per element (parent first):
//   struct css_elem e = { .tag = "div", .id = id_or_NULL, .classes = "a b", .parent = &parent_e };
//   struct css_style s;
//   css_compute(sh, &e, style_attr_or_NULL, parent_style_or_NULL, &s);
//   if (s.has & CSS_H_DISPLAY && s.display == CSS_DISPLAY_NONE) skip subtree; ...
//   css_free(sh);
//
// A field of css_style is valid only if its CSS_H_* bit is set in `has` (the engine then keeps
// its own tag default otherwise). Exception: font_px/font_bucket are always valid.
// Inherited properties (color, bold, italic, font size, align, visibility, white-space,
// text-transform, list-style) are already filled from `parent` by css_compute().
#include <stdint.h>
#include <stddef.h>

#define CSS_MAX_RULES      1500
#define CSS_MAX_TEXT       (64 * 1024)
#define CSS_MAX_PARTS      4
#define CSS_MAX_DECLS      24     // per rule / per inline style
#define CSS_MAX_INPUT      (128 * 1024)  // per sheet (total over all css_parse calls)

enum { CSS_DISPLAY_INLINE, CSS_DISPLAY_BLOCK, CSS_DISPLAY_NONE, CSS_DISPLAY_INLINE_BLOCK,
       CSS_DISPLAY_LIST_ITEM, CSS_DISPLAY_TABLE, CSS_DISPLAY_TABLE_ROW, CSS_DISPLAY_TABLE_CELL };
enum { CSS_ALIGN_LEFT, CSS_ALIGN_CENTER, CSS_ALIGN_RIGHT };
enum { CSS_DECO_NONE, CSS_DECO_UNDERLINE, CSS_DECO_LINE_THROUGH };
enum { CSS_TT_NONE, CSS_TT_UPPER, CSS_TT_LOWER, CSS_TT_CAPITALIZE };
enum { CSS_FLOAT_NONE, CSS_FLOAT_LEFT, CSS_FLOAT_RIGHT };

// Bits of css_style.has
#define CSS_H_DISPLAY    (1u << 0)
#define CSS_H_VISIBILITY (1u << 1)
#define CSS_H_COLOR      (1u << 2)
#define CSS_H_BG         (1u << 3)
#define CSS_H_BOLD       (1u << 4)
#define CSS_H_ITALIC     (1u << 5)
#define CSS_H_FONTSIZE   (1u << 6)   // explicit or inherited font-size (font_px is always valid)
#define CSS_H_ALIGN      (1u << 7)
#define CSS_H_DECO       (1u << 8)
#define CSS_H_WHITESPACE (1u << 9)
#define CSS_H_MT         (1u << 10)
#define CSS_H_MB         (1u << 11)
#define CSS_H_PT         (1u << 12)
#define CSS_H_PB         (1u << 13)
#define CSS_H_WIDTH      (1u << 14)
#define CSS_H_MAXW       (1u << 15)
#define CSS_H_LISTSTYLE  (1u << 16)
#define CSS_H_FLOAT      (1u << 17)
#define CSS_H_POSITION   (1u << 18)
#define CSS_H_OVERFLOW   (1u << 19)
#define CSS_H_TRANSFORM  (1u << 20)

struct css_style {
    uint32_t has;            // CSS_H_* bits: which fields below are valid
    uint32_t color;          // 0xRRGGBB
    uint32_t bg;             // 0xRRGGBB (alpha blended against white); `transparent` clears CSS_H_BG
    uint8_t  display;        // CSS_DISPLAY_*  (flex/grid => BLOCK)
    uint8_t  hidden;         // visibility:hidden (display:none is display == NONE)
    uint8_t  bold;           // font-weight >= 600 / bold / bolder
    uint8_t  italic;
    uint8_t  font_px;        // computed font size in px, always valid (default 16)
    uint8_t  font_bucket;    // 0 small(<=12px) 1 normal(13..17) 2 large(18..23) 3 x-large(>=24)
    uint8_t  align;          // CSS_ALIGN_*
    uint8_t  deco;           // CSS_DECO_* (not inherited)
    uint8_t  white_pre;      // white-space: pre / pre-wrap
    uint8_t  text_transform; // CSS_TT_*
    uint8_t  list_none;      // list-style: none
    uint8_t  float_;         // CSS_FLOAT_*
    uint8_t  position_absolute;
    uint8_t  position_fixed; // position:fixed (engine may hide fixed headers/overlays)
    uint8_t  overflow_hidden;
    uint8_t  mt, mb, pt, pb; // margin/padding top/bottom px, 0..64
    uint8_t  width_pct, maxw_pct;  // width / max-width given in % (value then 0..100)
    uint16_t width, maxw;    // px 0..4096 (or percent, see above). Valid iff CSS_H_WIDTH / CSS_H_MAXW
};

// Element description supplied by the caller. Strings are NUL-terminated, may be NULL.
// tag should be lower-case. classes is the raw class attribute (space separated).
struct css_elem {
    const char *tag;
    const char *id;
    const char *classes;
    const struct css_elem *parent;   // ancestor chain (NULL at the root)
};

struct css_sheet;

struct css_sheet *css_sheet_new(void);                       // NULL on out of memory
void css_free(struct css_sheet *sh);                         // NULL-safe
// Adds rules from `css` (any garbage tolerated, NULs ok). Never fails loudly; stops at limits.
void css_parse(struct css_sheet *sh, const char *css, size_t len);
// Computes the style of `el`. parent may be NULL, inline_style (style="" attribute) may be NULL.
void css_compute(const struct css_sheet *sh, const struct css_elem *el, const char *inline_style,
                 const struct css_style *parent, struct css_style *out);
// Fills the inherited properties of `child` that are not set from `parent`.
void css_inherit(const struct css_style *parent, struct css_style *child);
int  css_rule_count(const struct css_sheet *sh);
