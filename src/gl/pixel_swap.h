/* pixel_swap.h - GL_UNPACK_SWAP_BYTES / GL_PACK_SWAP_BYTES on an ES backend, which has neither.
 *
 * Shared by GL4ES (src/gl) and GLADIATOR (src/gl/GLADIATOR); header-only, so each layer builds its own copy.
 * Mesa does the same in one place (_mesa_swap_bytes_2d_image, applied while unpacking).
 *
 * With swapping on, every element larger than one byte has its bytes reversed (GL 4.6, 8.4.4.1 and table 8.7):
 *   - non-packed types (SHORT, UNSIGNED_SHORT, HALF_FLOAT, INT, UNSIGNED_INT, FLOAT): each component;
 *   - packed types (5_6_5, 4_4_4_4, 8_8_8_8, 2_10_10_10_REV, 10F_11F_11F_REV, 24_8, ...): the whole pixel;
 *   - FLOAT_32_UNSIGNED_INT_24_8_REV: each of its two 32-bit words.
 * Byte types (and GL_BITMAP) are left alone.
 *
 * The layout is the application's one (alignment, row length, skips, image height); the helpers keep it, so a swapped
 * copy can be handed to the next stage with the same unpack state. Every element starts at a multiple of its size from
 * the base pointer (row and image strides are multiples of the element size by construction), so only the pixels
 * proper are touched and padding is never reinterpreted. */
#ifndef LORICA_PIXEL_SWAP_H
#define LORICA_PIXEL_SWAP_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int align;          /* GL_*_ALIGNMENT */
    int row_length;     /* GL_*_ROW_LENGTH (0 = width) */
    int image_height;   /* GL_*_IMAGE_HEIGHT (0 = height); only for 3D transfers */
    int skip_pixels, skip_rows;
    int skip_images;    /* only for 3D transfers */
} pxs_layout_t;

/* bytes swapped as one unit (1 = nothing to swap) */
static inline int pxs_elem_size(unsigned type)
{
    switch (type) {
        case 0x1402: case 0x1403:                   /* SHORT, UNSIGNED_SHORT */
        case 0x140B: case 0x8D61:                   /* HALF_FLOAT, HALF_FLOAT_OES */
        case 0x8363: case 0x8364:                   /* 5_6_5, 5_6_5_REV */
        case 0x8033: case 0x8365:                   /* 4_4_4_4, 4_4_4_4_REV */
        case 0x8034: case 0x8366:                   /* 5_5_5_1, 1_5_5_5_REV */
            return 2;
        case 0x1404: case 0x1405: case 0x1406:      /* INT, UNSIGNED_INT, FLOAT */
        case 0x8035: case 0x8367:                   /* 8_8_8_8, 8_8_8_8_REV */
        case 0x8036: case 0x8368:                   /* 10_10_10_2, 2_10_10_10_REV */
        case 0x84FA:                                /* UNSIGNED_INT_24_8 */
        case 0x8C3B: case 0x8C3E:                   /* 10F_11F_11F_REV, 5_9_9_9_REV */
        case 0x8DAD:                                /* FLOAT_32_UNSIGNED_INT_24_8_REV (two words) */
            return 4;
    }
    return 1;
}

static inline int pxs_ncomp(unsigned format)
{
    switch (format) {
        case 0x1900: case 0x1901: case 0x1902:      /* COLOR_INDEX, STENCIL_INDEX, DEPTH_COMPONENT */
        case 0x1903: case 0x1904: case 0x1905: case 0x1906:   /* RED, GREEN, BLUE, ALPHA */
        case 0x1909: case 0x8049:                   /* LUMINANCE, INTENSITY */
        case 0x8D94: case 0x8D95: case 0x8D96: case 0x8D97:   /* RED/GREEN/BLUE/ALPHA_INTEGER */
            return 1;
        case 0x190A: case 0x8227: case 0x8228: case 0x84F9:   /* LUMINANCE_ALPHA, RG, RG_INTEGER, DEPTH_STENCIL */
            return 2;
        case 0x1907: case 0x80E0: case 0x8D98: case 0x8D9A:   /* RGB, BGR, RGB_INTEGER, BGR_INTEGER */
            return 3;
        case 0x1908: case 0x80E1: case 0x8D99: case 0x8D9B: case 0x8000:   /* RGBA, BGRA, *_INTEGER, ABGR_EXT */
            return 4;
    }
    return 0;
}

/* bytes per pixel group (0 = unknown combination) */
static inline size_t pxs_group(unsigned format, unsigned type)
{
    int es = pxs_elem_size(type), n = pxs_ncomp(format);
    switch (type) {
        case 0x8DAD: return 8;
        case 0x8363: case 0x8364: case 0x8033: case 0x8365: case 0x8034: case 0x8366:
        case 0x8035: case 0x8367: case 0x8036: case 0x8368: case 0x84FA: case 0x8C3B: case 0x8C3E:
            return (size_t)es;                       /* packed: one element per pixel */
    }
    return n ? (size_t)n * (size_t)es : 0;
}

/* group size, row stride and image stride of a w x h transfer; 0 when there is nothing to swap */
static inline int pxs_strides(int w, int h, unsigned format, unsigned type, const pxs_layout_t* L,
                              size_t* group, size_t* row, size_t* image)
{
    int es = pxs_elem_size(type);
    size_t g = pxs_group(format, type), rl, a;
    if (es <= 1 || !g || w <= 0 || h <= 0) return 0;
    rl = (size_t)(L->row_length > 0 ? L->row_length : w) * g;
    a = (size_t)(L->align > 0 ? L->align : 4);
    *group = g;
    *row = ((size_t)es >= a) ? rl : (rl + a - 1) / a * a;
    *image = (size_t)(L->image_height > 0 ? L->image_height : h) * *row;
    return es;
}

/* bytes from the base pointer to the end of the last pixel (0 = nothing to swap) */
static inline size_t pxs_span(int w, int h, int d, unsigned format, unsigned type, const pxs_layout_t* L)
{
    size_t g, row, img;
    if (d <= 0 || !pxs_strides(w, h, format, type, L, &g, &row, &img)) return 0;
    return ((size_t)L->skip_images + (size_t)d - 1) * img + ((size_t)L->skip_rows + (size_t)h - 1) * row +
           ((size_t)L->skip_pixels + (size_t)w) * g;
}

/* swaps the pixels of the transfer in place (padding and skipped regions are not touched) */
static inline void pxs_swap_pixels(void* base, int w, int h, int d, unsigned format, unsigned type, const pxs_layout_t* L)
{
    size_t g, row, img, n, i;
    int es, y, z, k;
    if (!base || d <= 0 || !(es = pxs_strides(w, h, format, type, L, &g, &row, &img))) return;
    n = (size_t)w * g / (size_t)es;                  /* elements per row */
    for (z = 0; z < d; z++)
        for (y = 0; y < h; y++) {
            unsigned char* p = (unsigned char*)base + ((size_t)L->skip_images + z) * img + ((size_t)L->skip_rows + y) * row +
                               (size_t)L->skip_pixels * g;
            for (i = 0; i < n; i++, p += es)
                for (k = 0; k < es / 2; k++) { unsigned char t = p[k]; p[k] = p[es - 1 - k]; p[es - 1 - k] = t; }
        }
}

/* a copy of the application's span with the pixels swapped, same layout; NULL if there is nothing to swap or no memory */
static inline void* pxs_swapped_copy(const void* base, int w, int h, int d, unsigned format, unsigned type, const pxs_layout_t* L)
{
    size_t span = base ? pxs_span(w, h, d, format, type, L) : 0;
    void* out;
    if (!span || !(out = malloc(span))) return NULL;
    memcpy(out, base, span);
    pxs_swap_pixels(out, w, h, d, format, type, L);
    return out;
}

#endif /* LORICA_PIXEL_SWAP_H */
