#pragma once
// Image decoding (PNG, JPEG, GIF first frame, BMP) via vendored stb_image, integer-only downscale.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../gfx/gfx.h"

// px: w*h pixels, 0xAARRGGBB with straight (non-premultiplied) alpha; kmalloc'd.
struct img { int w, h; uint32_t *px; };

// Source images larger than IMG_MAX_SIDE per side or IMG_MAX_PIXELS total are rejected
// before decoding. max_w/max_h <= 0 mean "no limit". Aspect ratio is preserved; images
// are never enlarged. Returns false (and leaves *out zeroed) on any failure.
#define IMG_MAX_SIDE   4096
#define IMG_MAX_PIXELS (8 * 1024 * 1024)
bool img_decode(const void *data, size_t len, int max_w, int max_h, struct img *out);
void img_free(struct img *im);
// Alpha-blend onto a surface at (x, y), clipped to the surface clip rect.
void img_draw(struct surface *s, const struct img *im, int x, int y);
// Integer area-average downscale to exactly nw x nh (<= source); returns false on OOM/bad args.
bool img_scale(const struct img *src, int nw, int nh, struct img *out);
