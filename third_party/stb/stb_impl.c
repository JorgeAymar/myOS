// stb_image configured for the myOS kernel (see README). Compiled without -Werror.
// The wrapper API at the bottom is consumed by kernel/gfx/img.c.
#include <stdint.h>
#include <stddef.h>
#include "mm.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(sz) kmalloc(sz)
#define STBI_REALLOC(p, sz) krealloc(p, sz)
#define STBI_FREE(p) kfree(p)
#include "stb_image.h"

int stbwrap_info(const void *d, size_t len, int *w, int *h) {
    int comp;
    if (len > 0x7fffffff) return 0;
    int ok = stbi_info_from_memory((const stbi_uc *)d, (int)len, w, h, &comp);
    // stb's info path reports top-down BMPs (negative biHeight) with a negative height.
    if (ok && *h < 0 && *h != (-2147483647 - 1)) *h = -*h;
    return ok;
}

// Returns w*h*4 bytes (R,G,B,A order) allocated with kmalloc, or NULL.
uint8_t *stbwrap_load(const void *d, size_t len, int *w, int *h) {
    int comp;
    if (len > 0x7fffffff) return NULL;
    return stbi_load_from_memory((const stbi_uc *)d, (int)len, w, h, &comp, 4);
}
