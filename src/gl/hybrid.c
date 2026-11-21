/* hybrid.c - GL4ES side of the hybrid-mode seam (compatibility GL >= 3.2 on an ES 3.x backend).
 *
 * In a hybrid context GL4ES owns the objects (textures, buffers, programs, queries) and the draw path, and GLADIATOR
 * serves what GL4ES does not implement. A GLADIATOR-only entry point therefore sees GL4ES's objects, not its own, and
 * several things fall in the gap between the two layers. This file closes the ones that are about GL4ES state:
 *
 *   - texture targets GL4ES has no model for (2D/1D arrays, real 3D, cube map arrays, multisample, buffer) and texture
 *     formats it cannot keep (sRGB and integer formats): the texture is GLADIATOR's, GL4ES only syncs the active unit
 *     and translates names;
 *   - glTexBuffer & co.: GL4ES buffer ids are not GLES names (gl4es_real_buffer);
 *   - glBeginTransformFeedback: GL4ES binds the program lazily, at draw time, and GLADIATOR never saw glUseProgram;
 *   - conditional rendering on GL4ES's own query objects;
 *   - glReadPixels into integer formats.
 *
 * Mesa does all of this in one state tracker; here the two layers stay, and the seam is explicit. */
#include <stdlib.h>
#include <string.h>

#include "../glx/hardext.h"
#include "buffers.h"
#include "gl4es.h"
#include "glstate.h"
#include "init.h"
#include "loader.h"
#include "logs.h"
#include "queries.h"
#include "texture.h"
#include "hybrid.h"

#ifndef NOEGL
extern int   gl4es_gl31_hybrid_active(void);                 /* glx/gl31_bridge.c */
extern void* gl4es_gl31_fn(const char* name);                 /* GLADIATOR entry point by GL name (NULL if none) */
extern void  gl4es_gl31_hybrid_program(GLuint program);       /* GLADIATOR's idea of the current program */
extern GLuint gl4es_real_buffer(GLuint id);                   /* buffers.c */

int gl4es_hybrid(void) { return gl4es_gl31_hybrid_active(); }

/* GLADIATOR entry points, resolved once */
#define GLAD(ret, name, args) \
    static ret (*p_##name) args = NULL; \
    if (!p_##name) p_##name = (ret (*) args)gl4es_gl31_fn(#name)

/* GLADIATOR keeps its own active texture unit; GL4ES changes the GLES one behind its back (realize_textures, blits),
 * so it is set again before every delegated texture call. GL4ES's cache of the GLES unit follows. */
static void hy_unit(void)
{
    GLAD(void, glActiveTexture, (GLenum));
    if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0 + glstate->texture.active);
    glstate->gleshard->active = glstate->texture.active;
}

/* GL4ES's unpack state (and unpack PBO, which GL4ES keeps in client memory) as GLADIATOR expects it */
static const void* hy_unpack(const void* data)
{
    GLAD(void, glPixelStorei, (GLenum, GLint));
    if (p_glPixelStorei) {
        p_glPixelStorei(GL_UNPACK_ALIGNMENT, glstate->texture.unpack_align);
        p_glPixelStorei(GL_UNPACK_ROW_LENGTH, glstate->texture.unpack_row_length);
        p_glPixelStorei(GL_UNPACK_SKIP_PIXELS, glstate->texture.unpack_skip_pixels);
        p_glPixelStorei(GL_UNPACK_SKIP_ROWS, glstate->texture.unpack_skip_rows);
        p_glPixelStorei(0x806E /* GL_UNPACK_IMAGE_HEIGHT */, glstate->texture.unpack_image_height);
    }
    if (glstate->vao->unpack)
        data = (const char*)data + (uintptr_t)glstate->vao->unpack->data;
    return data;
}

/* GL4ES keeps GL_UNPACK_ROW_LENGTH / SKIP_* / IMAGE_HEIGHT in software and GLES at 0; GLADIATOR passes them on to
 * GLES, so they go back to 0 after a delegated upload (the alignment is the same in both) */
static void hy_unpack_end(void)
{
    GLAD(void, glPixelStorei, (GLenum, GLint));
    if (!p_glPixelStorei) return;
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    p_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    p_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    p_glPixelStorei(0x806E /* GL_UNPACK_IMAGE_HEIGHT */, 0);
}

/* ---- texture targets GL4ES does not model ---- */
int gl4es_hybrid_tex_target(GLenum target)
{
    switch (target) {
        case 0x8C18: /* GL_TEXTURE_1D_ARRAY */
        case 0x8C1A: /* GL_TEXTURE_2D_ARRAY */
        case 0x806F: /* GL_TEXTURE_3D: real 3D textures (GL4ES flattens them to their first layer) */
        case 0x9009: /* GL_TEXTURE_CUBE_MAP_ARRAY */
        case 0x9100: /* GL_TEXTURE_2D_MULTISAMPLE */
        case 0x9102: /* GL_TEXTURE_2D_MULTISAMPLE_ARRAY */
        case 0x8C2A: /* GL_TEXTURE_BUFFER */
            return gl4es_hybrid();
    }
    return 0;
}

/* the GLES name behind an application texture name (GL4ES maps names it did not generate) */
static GLuint hy_texname(GLenum target, GLuint texture)
{
    if (!texture) return 0;
    return gl4es_getTexture(target, texture)->glname;
}

void gl4es_hybrid_BindTexture(GLenum target, GLuint texture)
{
    GLAD(void, glBindTexture, (GLenum, GLuint));
    hy_unit();
    if (p_glBindTexture) p_glBindTexture(target, hy_texname(target, texture));
}

void gl4es_hybrid_TexImage3D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint border,
                             GLenum format, GLenum type, const void* data)
{
    GLAD(void, glTexImage3D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*));
    hy_unit();
    data = hy_unpack(data);
    if (p_glTexImage3D) p_glTexImage3D(target, level, ifmt, w, h, d, border, format, type, data);
    hy_unpack_end();
}

void gl4es_hybrid_TexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d,
                                GLenum format, GLenum type, const void* data)
{
    GLAD(void, glTexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*));
    hy_unit();
    data = hy_unpack(data);
    if (p_glTexSubImage3D) p_glTexSubImage3D(target, level, x, y, z, w, h, d, format, type, data);
    hy_unpack_end();
}

void gl4es_hybrid_TexStorage3D(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d)
{
    GLAD(void, glTexStorage3D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei));
    hy_unit();
    if (p_glTexStorage3D) p_glTexStorage3D(target, levels, ifmt, w, h, d);
}

void gl4es_hybrid_CompressedTexImage3D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d,
                                       GLint border, GLsizei size, const void* data)
{
    GLAD(void, glCompressedTexImage3D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLsizei, const void*));
    hy_unit();
    data = hy_unpack(data);
    if (p_glCompressedTexImage3D) p_glCompressedTexImage3D(target, level, ifmt, w, h, d, border, size, data);
    hy_unpack_end();
}

void gl4es_hybrid_CompressedTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                                          GLsizei d, GLenum format, GLsizei size, const void* data)
{
    GLAD(void, glCompressedTexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, const void*));
    hy_unit();
    data = hy_unpack(data);
    if (p_glCompressedTexSubImage3D) p_glCompressedTexSubImage3D(target, level, x, y, z, w, h, d, format, size, data);
    hy_unpack_end();
}

void gl4es_hybrid_CopyTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLint rx, GLint ry,
                                    GLsizei w, GLsizei h)
{
    GLAD(void, glCopyTexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei));
    hy_unit();
    if (p_glCopyTexSubImage3D) p_glCopyTexSubImage3D(target, level, x, y, z, rx, ry, w, h);
}

void gl4es_hybrid_TexParameteri(GLenum target, GLenum pname, GLint param)
{
    GLAD(void, glTexParameteri, (GLenum, GLenum, GLint));
    hy_unit();
    if (p_glTexParameteri) p_glTexParameteri(target, pname, param);
}

void gl4es_hybrid_TexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    GLAD(void, glTexParameterf, (GLenum, GLenum, GLfloat));
    hy_unit();
    if (p_glTexParameterf) p_glTexParameterf(target, pname, param);
}

void gl4es_hybrid_TexParameteriv(GLenum target, GLenum pname, const GLint* params)
{
    GLAD(void, glTexParameteriv, (GLenum, GLenum, const GLint*));
    hy_unit();
    if (p_glTexParameteriv) p_glTexParameteriv(target, pname, params);
}

void gl4es_hybrid_TexParameterfv(GLenum target, GLenum pname, const GLfloat* params)
{
    GLAD(void, glTexParameterfv, (GLenum, GLenum, const GLfloat*));
    hy_unit();
    if (p_glTexParameterfv) p_glTexParameterfv(target, pname, params);
}

void gl4es_hybrid_GetTexParameteriv(GLenum target, GLenum pname, GLint* params)
{
    GLAD(void, glGetTexParameteriv, (GLenum, GLenum, GLint*));
    hy_unit();
    if (p_glGetTexParameteriv) p_glGetTexParameteriv(target, pname, params);
}

void gl4es_hybrid_GetTexParameterfv(GLenum target, GLenum pname, GLfloat* params)
{
    GLAD(void, glGetTexParameterfv, (GLenum, GLenum, GLfloat*));
    hy_unit();
    if (p_glGetTexParameterfv) p_glGetTexParameterfv(target, pname, params);
}

void gl4es_hybrid_GetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params)
{
    GLAD(void, glGetTexLevelParameteriv, (GLenum, GLint, GLenum, GLint*));
    hy_unit();
    if (p_glGetTexLevelParameteriv) p_glGetTexLevelParameteriv(target, level, pname, params);
}

void gl4es_hybrid_GenerateMipmap(GLenum target)
{
    GLAD(void, glGenerateMipmap, (GLenum));
    hy_unit();
    if (p_glGenerateMipmap) p_glGenerateMipmap(target);
}

/* ---- 2D texture formats GL4ES cannot keep losslessly ----
 * sRGB (stored as linear RGBA), integer formats, and the normalized / float formats wider than 8 bits that GL4ES's ES 2
 * format logic collapses to 8-bit RGBA (or does not know): 16-bit UNORM and SNORM, 8-bit SNORM, 16/32-bit float,
 * RGB10_A2, R11F_G11F_B10F, RGB9_E5. GLADIATOR keeps them with the format the ES backend can sample and render
 * (gl31_storage_format: the requested one, or the closest one with at least its precision). */
int gl4es_hybrid_tex_format(GLint ifmt)
{
    switch (ifmt) {
        case 0x822A: case 0x822C: case 0x8054: case 0x805B:          /* R16, RG16, RGB16, RGBA16 */
        case 0x8F94: case 0x8F95: case 0x8F96: case 0x8F97:          /* R8_SNORM .. RGBA8_SNORM */
        case 0x8F98: case 0x8F99: case 0x8F9A: case 0x8F9B:          /* R16_SNORM .. RGBA16_SNORM */
        case 0x822D: case 0x822F: case 0x881B: case 0x881A:          /* R16F, RG16F, RGB16F, RGBA16F */
        case 0x822E: case 0x8230: case 0x8815: case 0x8814:          /* R32F, RG32F, RGB32F, RGBA32F */
        case 0x8059:                                                  /* RGB10_A2 */
        case 0x8C40: case 0x8C41: case 0x8C42: case 0x8C43:          /* SRGB, SRGB8, SRGB_ALPHA, SRGB8_ALPHA8 */
        case 0x8231: case 0x8232: case 0x8233: case 0x8234: case 0x8235: case 0x8236:   /* R8I .. R32UI */
        case 0x8237: case 0x8238: case 0x8239: case 0x823A: case 0x823B: case 0x823C:   /* RG8I .. RG32UI */
        case 0x8D70: case 0x8D71: case 0x8D76: case 0x8D77: case 0x8D7C: case 0x8D7D:   /* RGB(A)32/16/8UI */
        case 0x8D82: case 0x8D83: case 0x8D88: case 0x8D89: case 0x8D8E: case 0x8D8F:   /* RGB(A)32/16/8I */
        case 0x906F:                                                                       /* RGB10_A2UI */
        case 0x8C3A: case 0x8C3D:                                                          /* R11F_G11F_B10F, RGB9_E5 */
            return gl4es_hybrid();
    }
    return 0;
}

/* the texture bound to `target` on the active unit, also bound in GLADIATOR (GL_TEXTURE_2D or a cube face) */
static gltexture_t* hy_bind2d(GLenum target)
{
    GLAD(void, glBindTexture, (GLenum, GLuint));
    GLenum bt = (target == GL_TEXTURE_CUBE_MAP || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z))
              ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    gltexture_t* tex = glstate->texture.bound[glstate->texture.active][what_target(target)];
    hy_unit();
    if (p_glBindTexture && tex) p_glBindTexture(bt, tex->glname);
    if (bt == GL_TEXTURE_2D && tex) glstate->actual_tex2d[glstate->texture.active] = tex->glname;
    return tex;
}

/* GL4ES's record of a texture whose storage GLADIATOR keeps: valid, no automatic mipmaps, no format conversions */
static void hy_record(gltexture_t* tex, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLenum format, GLenum type)
{
    if (!tex || level != 0) return;
    tex->hybrid = 1;
    tex->valid = 1;
    tex->width = tex->nwidth = w;
    tex->height = tex->nheight = h;
    tex->adjust = 0; tex->adjustxy[0] = tex->adjustxy[1] = 1.f;
    tex->shrink = 0; tex->useratio = 0;
    tex->npot = 0;
    tex->format = tex->inter_format = format;
    tex->type = tex->inter_type = type;
    tex->wanted_internal = tex->orig_internal = tex->internalformat = ifmt;
    tex->mipmap_auto = 0; tex->mipmap_need = 0; tex->mipmap_done = 1;
    tex->compressed = 0;
    tex->alpha = true;
}

void gl4es_hybrid_TexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                             GLenum format, GLenum type, const void* data)
{
    GLAD(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*));
    gltexture_t* tex = hy_bind2d(target);
    data = hy_unpack(data);
    if (p_glTexImage2D) p_glTexImage2D(target, level, ifmt, w, h, border, format, type, data);
    hy_unpack_end();
    hy_record(tex, level, ifmt, w, h, format, type);
}

void gl4es_hybrid_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                GLenum format, GLenum type, const void* data)
{
    GLAD(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*));
    hy_bind2d(target);
    data = hy_unpack(data);
    if (p_glTexSubImage2D) p_glTexSubImage2D(target, level, x, y, w, h, format, type, data);
    hy_unpack_end();
}

void gl4es_hybrid_TexStorage2D(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h)
{
    GLAD(void, glTexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei));
    gltexture_t* tex = hy_bind2d(target);
    if (p_glTexStorage2D) p_glTexStorage2D(target, levels, ifmt, w, h);
    hy_record(tex, 0, ifmt, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
}

/* glGenerateMipmap on a 2D / cube texture GLADIATOR keeps: GLADIATOR also records the chain (glGetTexLevelParameter) */
void gl4es_hybrid_GenerateMipmap2D(GLenum target)
{
    GLAD(void, glGenerateMipmap, (GLenum));
    hy_bind2d(target);
    if (p_glGenerateMipmap) p_glGenerateMipmap(target);
}

/* glGetTexImage of a 2D / cube face texture GLADIATOR keeps: read back from the real storage (GLADIATOR converts to the
 * format / type asked), with GL4ES's pack state (kept in software, GLES at 0) and pack PBO (client memory) */
void gl4es_hybrid_GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* data)
{
    GLAD(void, glGetTexImage, (GLenum, GLint, GLenum, GLenum, void*));
    GLAD(void, glPixelStorei, (GLenum, GLint));
    hy_bind2d(target);
    if (glstate->vao->pack) data = (char*)glstate->vao->pack->data + (uintptr_t)data;
    if (p_glPixelStorei) {
        p_glPixelStorei(GL_PACK_ALIGNMENT, glstate->texture.pack_align);
        p_glPixelStorei(GL_PACK_ROW_LENGTH, glstate->texture.pack_row_length);
        p_glPixelStorei(GL_PACK_SKIP_ROWS, glstate->texture.pack_skip_rows);
        p_glPixelStorei(GL_PACK_SKIP_PIXELS, glstate->texture.pack_skip_pixels);
    }
    if (p_glGetTexImage && data) p_glGetTexImage(target, level, format, type, data);
    if (p_glPixelStorei) {
        p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        p_glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        p_glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    }
}

int gl4es_hybrid_tex_is_hybrid(GLenum target)
{
    gltexture_t* tex;
    if (!gl4es_hybrid()) return 0;
    tex = glstate->texture.bound[glstate->texture.active][what_target(target)];
    return tex && tex->hybrid;
}

/* ---- texture buffers: GL4ES buffer id -> GLES buffer name ---- */
void APIENTRY_GL4ES gl4es_glTexBuffer(GLenum target, GLenum ifmt, GLuint buffer)
{
    GLAD(void, glTexBuffer, (GLenum, GLenum, GLuint));
    if (!gl4es_hybrid() || !p_glTexBuffer) { errorShim(GL_INVALID_OPERATION); return; }
    hy_unit();
    p_glTexBuffer(target, ifmt, buffer ? gl4es_real_buffer(buffer) : 0);
    noerrorShim();
}

void APIENTRY_GL4ES gl4es_glTexBufferRange(GLenum target, GLenum ifmt, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
    GLAD(void, glTexBufferRange, (GLenum, GLenum, GLuint, GLintptr, GLsizeiptr));
    if (!gl4es_hybrid() || !p_glTexBufferRange) { errorShim(GL_INVALID_OPERATION); return; }
    hy_unit();
    p_glTexBufferRange(target, ifmt, buffer ? gl4es_real_buffer(buffer) : 0, offset, size);
    noerrorShim();
}

void APIENTRY_GL4ES gl4es_glTextureBuffer(GLuint texture, GLenum ifmt, GLuint buffer)
{
    GLAD(void, glTextureBuffer, (GLuint, GLenum, GLuint));
    if (!gl4es_hybrid() || !p_glTextureBuffer) { errorShim(GL_INVALID_OPERATION); return; }
    p_glTextureBuffer(hy_texname(0x8C2A /* GL_TEXTURE_BUFFER */, texture), ifmt, buffer ? gl4es_real_buffer(buffer) : 0);
    noerrorShim();
}

void APIENTRY_GL4ES gl4es_glTextureBufferRange(GLuint texture, GLenum ifmt, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
    GLAD(void, glTextureBufferRange, (GLuint, GLenum, GLuint, GLintptr, GLsizeiptr));
    if (!gl4es_hybrid() || !p_glTextureBufferRange) { errorShim(GL_INVALID_OPERATION); return; }
    p_glTextureBufferRange(hy_texname(0x8C2A /* GL_TEXTURE_BUFFER */, texture), ifmt, buffer ? gl4es_real_buffer(buffer) : 0, offset, size);
    noerrorShim();
}

/* ---- transform feedback: the program has to be current in GLES (and in GLADIATOR) when it begins ---- */
void APIENTRY_GL4ES gl4es_glBeginTransformFeedback(GLenum mode)
{
    GLAD(void, glBeginTransformFeedback, (GLenum));
    LOAD_GLES2(glUseProgram);
    FLUSH_BEGINEND;
    if (!gl4es_hybrid() || !p_glBeginTransformFeedback) { errorShim(GL_INVALID_OPERATION); return; }
    if (glstate->glsl->program && glstate->gleshard->program != glstate->glsl->program) {
        gles_glUseProgram(glstate->glsl->program);
        glstate->gleshard->program = glstate->glsl->program;
        glstate->gleshard->glprogram = glstate->glsl->glprogram;
    }
    gl4es_gl31_hybrid_program(glstate->glsl->program);
    p_glBeginTransformFeedback(mode);
    noerrorShim();
}

/* ---- glReadPixels into integer formats (GL4ES converts everything to normalized RGBA) ---- */
int gl4es_hybrid_read_format(GLenum format)
{
    switch (format) {
        case 0x8D94: /* GL_RED_INTEGER */ case 0x8228: /* GL_RG_INTEGER */ case 0x8D98: /* GL_RGB_INTEGER */
        case 0x8D99: /* GL_RGBA_INTEGER */ case 0x8D9A: /* GL_BGR_INTEGER */ case 0x8D9B: /* GL_BGRA_INTEGER */
            return gl4es_hybrid();
    }
    return 0;
}

/* GLES 3.x always reads an integer color buffer as GL_RGBA_INTEGER with GL_UNSIGNED_INT (or GL_INT for signed
 * ones), from the framebuffer GL4ES has bound. The pixels are then packed in the format, type and GL_PACK_* state
 * the application asked for, as the desktop does. */
void gl4es_hybrid_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* data)
{
    LOAD_GLES(glReadPixels);
    LOAD_GLES(glPixelStorei);
    int sgn = (type==GL_INT || type==GL_SHORT || type==GL_BYTE);
    int ncomp, esize, bgr = (format==0x8D9A || format==0x8D9B);
    GLuint* tmp;
    char* dst;
    size_t rl, stride;
    switch (format) {
        case 0x8D94: ncomp = 1; break;               /* RED_INTEGER */
        case 0x8228: ncomp = 2; break;               /* RG_INTEGER */
        case 0x8D98: case 0x8D9A: ncomp = 3; break;  /* RGB / BGR_INTEGER */
        default: ncomp = 4; break;                   /* RGBA / BGRA_INTEGER */
    }
    switch (type) {
        case GL_UNSIGNED_INT: case GL_INT: esize = 4; break;
        case GL_UNSIGNED_SHORT: case GL_SHORT: esize = 2; break;
        case GL_UNSIGNED_BYTE: case GL_BYTE: esize = 1; break;
        default: errorShim(GL_INVALID_ENUM); return;
    }
    if (w <= 0 || h <= 0) return;
    tmp = (GLuint*)malloc((size_t)w * (size_t)h * 16);
    if (!tmp) { errorShim(GL_OUT_OF_MEMORY); return; }
    // GL4ES keeps GL_PACK_ROW_LENGTH / SKIP_* in software (GLES stays at 0); only the alignment reaches GLES
    gles_glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gles_glReadPixels(x, y, w, h, 0x8D99 /* GL_RGBA_INTEGER */, sgn ? GL_INT : GL_UNSIGNED_INT, tmp);
    gles_glPixelStorei(GL_PACK_ALIGNMENT, glstate->texture.pack_align);

    dst = (char*)data;
    if (glstate->vao->pack) dst = (char*)glstate->vao->pack->data + (uintptr_t)data;
    rl = (size_t)(glstate->texture.pack_row_length ? glstate->texture.pack_row_length : (GLuint)w) * ncomp * esize;
    {
        size_t al = glstate->texture.pack_align ? glstate->texture.pack_align : 4;
        stride = (rl + al - 1) / al * al;
    }
    dst += (size_t)glstate->texture.pack_skip_rows * stride + (size_t)glstate->texture.pack_skip_pixels * ncomp * esize;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const GLuint* s = tmp + ((size_t)j * w + i) * 4;
            char* p = dst + (size_t)j * stride + (size_t)i * ncomp * esize;
            for (int c = 0; c < ncomp; c++) {
                int sc = (bgr && c < 3) ? 2 - c : c;
                long long v = sgn ? (long long)(GLint)s[sc] : (long long)s[sc];
                switch (type) {   /* values outside the destination type are clamped, as GL does */
                    case GL_UNSIGNED_INT:   ((GLuint*)p)[c] = (GLuint)v; break;
                    case GL_INT:            ((GLint*)p)[c] = (GLint)v; break;
                    case GL_UNSIGNED_SHORT: ((GLushort*)p)[c] = (GLushort)(v > 65535 ? 65535 : v); break;
                    case GL_SHORT:          ((GLshort*)p)[c] = (GLshort)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); break;
                    case GL_UNSIGNED_BYTE:  ((GLubyte*)p)[c] = (GLubyte)(v > 255 ? 255 : v); break;
                    default:                ((GLbyte*)p)[c] = (GLbyte)(v > 127 ? 127 : v < -128 ? -128 : v); break;
                }
            }
        }
    free(tmp);
}

/* glDeleteTextures: GLADIATOR drops its record (target, levels, bindings) of the GLES name. It also deletes the name;
 * GL4ES's own delete right after is then a no-op on a free name. */
void gl4es_hybrid_DeleteTexture(GLuint glname)
{
    GLAD(void, glDeleteTextures, (GLsizei, const GLuint*));
    if (glname && gl4es_hybrid() && p_glDeleteTextures) p_glDeleteTextures(1, &glname);
}

#else   /* NOEGL: no GLADIATOR, nothing is hybrid */
void gl4es_hybrid_DeleteTexture(GLuint glname) { (void)glname; }
int gl4es_hybrid(void) { return 0; }
int gl4es_hybrid_tex_target(GLenum target) { (void)target; return 0; }
int gl4es_hybrid_tex_format(GLint ifmt) { (void)ifmt; return 0; }
int gl4es_hybrid_tex_is_hybrid(GLenum target) { (void)target; return 0; }
int gl4es_hybrid_read_format(GLenum format) { (void)format; return 0; }
#endif

/* ---- conditional rendering on GL4ES's query objects ---- */
void APIENTRY_GL4ES gl4es_glBeginConditionalRender(GLuint id, GLenum mode)
{
    GLuint avail = 1, result = 1;
    FLUSH_BEGINEND;
    if (glstate->cond_render_active) { errorShim(GL_INVALID_OPERATION); return; }
    if (!gl4es_glIsQuery(id)) { errorShim(GL_INVALID_VALUE); return; }
    switch (mode) {
        case 0x8E13: /* GL_QUERY_WAIT */             case 0x8E14: /* GL_QUERY_NO_WAIT */
        case 0x8E15: /* GL_QUERY_BY_REGION_WAIT */   case 0x8E16: /* GL_QUERY_BY_REGION_NO_WAIT */
            break;
        default: errorShim(GL_INVALID_ENUM); return;
    }
    if (mode == 0x8E14 || mode == 0x8E16)
        gl4es_glGetQueryObjectuiv(id, GL_QUERY_RESULT_AVAILABLE, &avail);
    if (avail)                  /* NO_WAIT with the result still pending: GL lets the commands run */
        gl4es_glGetQueryObjectuiv(id, GL_QUERY_RESULT, &result);
    glstate->cond_render_active = 1;
    glstate->cond_render_skip = (result == 0);
    noerrorShim();
}

void APIENTRY_GL4ES gl4es_glEndConditionalRender(void)
{
    FLUSH_BEGINEND;
    if (!glstate->cond_render_active) { errorShim(GL_INVALID_OPERATION); return; }
    glstate->cond_render_active = 0;
    glstate->cond_render_skip = 0;
    noerrorShim();
}

