#include "texture.h"
#include "hybrid.h"

#include "../glx/hardext.h"
#include "../glx/streaming.h"
#include "array.h"
#include "blit.h"
#include "decompress.h"
#include "debug.h"
#include "enum_info.h"
#include "fpe.h"
#include "framebuffers.h"
#include "gles.h"
#include "init.h"
#include "loader.h"
#include "matrix.h"
#include "pixel.h"
#include "raster.h"

//#define DEBUG
#ifdef DEBUG
#define DBG(a) a
#else
#define DBG(a)
#endif

static int inline nlevel(int size, int level) {
    if(size) {
        size>>=level;
        if(!size) size=1;
    }
    return size;
}

#ifndef NOEGL
extern int gl4es_backend_es3(void);   // glx/gl31_bridge.c
#endif

// Does the internal format the application asked for store alpha? (GL base formats and their sized variants)
static int internalformat_has_alpha(GLenum ifmt) {
    switch (ifmt) {
        case 1: case 2: case 3:
        case GL_RED: case GL_RG: case GL_RGB: case GL_LUMINANCE: case GL_INTENSITY:
        case GL_R8: case 0x822B /* GL_RG8 */: case GL_RGB8: case GL_RGB5: case GL_RGB565: case GL_RGB4: case GL_R3_G3_B2:
        case 0x8052 /* GL_RGB10 */: case 0x8053 /* GL_RGB12 */: case GL_RGB16: case 0x8C40 /* GL_SRGB */: case 0x8C41 /* GL_SRGB8 */:
        case GL_LUMINANCE8: case GL_LUMINANCE16: case GL_INTENSITY8: case GL_INTENSITY16:
        case GL_R16F: case 0x822F /* GL_RG16F */: case GL_RGB16F: case 0x822E /* GL_R32F */: case 0x8230 /* GL_RG32F */: case GL_RGB32F:
        case 0x8C3A /* GL_R11F_G11F_B10F */: case GL_COMPRESSED_RGB: case GL_COMPRESSED_LUMINANCE:
        case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32:
            return 0;
    }
    return 1;
}

// GLES only copies framebuffer pixels into a texture whose components the framebuffer has (a read buffer without alpha cannot fill an RGBA
// texture: GL_INVALID_OPERATION, "Invalid copy texture format combination"), desktop GL gives alpha = 1 there. Asks the read buffer that is
// bound right now.
static int read_fb_lacks_alpha(void) {
    LOAD_GLES(glGetIntegerv);
    GLint abits = 8;
    gles_glGetIntegerv(0x0D55 /* GL_ALPHA_BITS */, &abits);
    return abits == 0;
}

// Copies the read buffer into the bound RGBA texture on the GPU: reallocates the level, attaches it to a scratch FBO and blits (the blit fills
// alpha with 1 where the source has none). Needs a GLES 3 context; returns 0 when it cannot be used so the caller reads the pixels back.
static int copy_to_rgba_by_blit(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, gltexture_t* bound) {
    typedef void (APIENTRY_GLES *blit_t)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    static blit_t blit = NULL;
    static int probed = 0;
    static GLuint scratch = 0;
    static void* scratch_owner = NULL;
    if (!probed) {
        probed = 1;
        if (gles) blit = (blit_t)proc_address(gles, "glBlitFramebuffer");
    }
#ifndef NOEGL
    if (!gl4es_backend_es3()) return 0;     // glBlitFramebuffer is ES 3 (the symbol may resolve in an ES 2 context and still be invalid there)
#else
    return 0;
#endif
    if (!blit || !bound || !bound->glname || width <= 0 || height <= 0) return 0;
    if (target != GL_TEXTURE_2D && !(target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)) return 0;

    LOAD_GLES2_OR_OES(glBindFramebuffer);
    LOAD_GLES2_OR_OES(glGenFramebuffers);
    LOAD_GLES2_OR_OES(glFramebufferTexture2D);
    LOAD_GLES(glIsEnabled);
    LOAD_GLES(glEnable);
    LOAD_GLES(glDisable);
    if (!gles_glBindFramebuffer || !gles_glGenFramebuffers || !gles_glFramebufferTexture2D) return 0;

    // storage first, through GL4ES so its bookkeeping (size, format) stays right
    gl4es_glTexImage2D(target, level, internalformat, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    // only a color-renderable RGBA8 level can be a blit destination (alpha-only / luminance-alpha storage goes through the read-back path)
    if (bound->format != GL_RGBA || bound->type != GL_UNSIGNED_BYTE) return 0;

    if (!scratch || scratch_owner != (void*)glstate) { gles_glGenFramebuffers(1, &scratch); scratch_owner = (void*)glstate; }
    GLuint restore = glstate->fbo.current_fb->id ? glstate->fbo.current_fb->id : glstate->fbo.mainfbo_fbo;
    gles_glBindFramebuffer(0x8CA9 /* DRAW_FRAMEBUFFER */, scratch);
    gles_glFramebufferTexture2D(0x8CA9, GL_COLOR_ATTACHMENT0, target, bound->glname, level);
    GLboolean scissor = gles_glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) gles_glDisable(GL_SCISSOR_TEST);
    blit(x, y, x + width, y + height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissor) gles_glEnable(GL_SCISSOR_TEST);
    gles_glFramebufferTexture2D(0x8CA9, GL_COLOR_ATTACHMENT0, target, 0, level);
    gles_glBindFramebuffer(0x8CA9, restore);
    return 1;
}

void APIENTRY_GL4ES gl4es_glCopyTexImage2D(GLenum target,  GLint level,  GLenum internalformat,  GLint x,  GLint y,  
                                GLsizei width,  GLsizei height,  GLint border) {
    if (glstate->raster.bm_drawing) bitmap_flush();   // pending glBitmap pixels belong before this framebuffer access
    DBG(printf("glCopyTexImage2D(%s, %i, %s, %i, %i, %i, %i, %i), glstate->fbo.current_fb=%p\n", PrintEnum(target), level, PrintEnum(internalformat), x, y, width, height, border, glstate->fbo.current_fb);)
     //PUSH_IF_COMPILING(glCopyTexImage2D);
    FLUSH_BEGINEND;
    const GLuint itarget = what_target(target);

    // actually bound if targeting shared TEX2D
    realize_bound(glstate->texture.active, target);

    if (globals4es.skiptexcopies) {
        DBG(printf("glCopyTexImage2D skipped.\n"));
        return;
    }

    errorGL();

    // "Unmap" if buffer mapped...
    glbuffer_t *pack = glstate->vao->pack;
    glbuffer_t *unpack = glstate->vao->unpack;
    glstate->vao->pack = NULL;
    glstate->vao->unpack = NULL;
    
    readfboBegin(); // multiple readfboBegin() can be chained...
    gltexture_t* bound = glstate->texture.bound[glstate->texture.active][itarget];

    if(glstate->fbo.current_fb->read_type==0) {
        LOAD_GLES(glGetIntegerv);
        gles_glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES, (GLint *) &glstate->fbo.current_fb->read_format);
        gles_glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE_OES, (GLint *) &glstate->fbo.current_fb->read_type);
    }
    int copytex = ((bound->format==GL_RGBA && bound->type==GL_UNSIGNED_BYTE) 
        || (bound->format==glstate->fbo.current_fb->read_format && bound->type==glstate->fbo.current_fb->read_type));
    int copied_by_blit = 0;
    if (copytex && internalformat_has_alpha(internalformat) && read_fb_lacks_alpha()) {
        // a texture with alpha out of a buffer without alpha: GLES refuses the direct copy (desktop GL gives alpha = 1). Blit on the GPU when
        // the backend is ES 3, otherwise read the pixels back.
        copied_by_blit = copy_to_rgba_by_blit(target, level, internalformat, x, y, width, height, bound);
        copytex = 0;
    }

    if (copied_by_blit) {
        // done on the GPU
    } else if (copytex) {
        GLenum fmt;
        switch(internalformat) {
            case GL_ALPHA:
            case GL_ALPHA8:
                fmt = GL_ALPHA; break;
            case GL_LUMINANCE:
            case GL_LUMINANCE8:
                fmt = GL_LUMINANCE; break;
            case GL_LUMINANCE_ALPHA:
            case GL_LUMINANCE8_ALPHA8:
                fmt = GL_LUMINANCE_ALPHA; break;
            case GL_RGB:
            case 3:
                fmt = GL_RGB; break;
            default:
                fmt = GL_RGBA;
        }
        LOAD_GLES(glCopyTexImage2D);
        gles_glCopyTexImage2D(target, level, fmt, x, y, width, height, border);
    } else {
        void* tmp = malloc(width*height*4);
        gl4es_glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
        gl4es_glTexImage2D(target, level, internalformat, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
        free(tmp);
    }
    
    readfboEnd();
    // "Remap" if buffer mapped...
    glstate->vao->pack = pack;
    glstate->vao->unpack = unpack;
}

void APIENTRY_GL4ES gl4es_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                GLint x, GLint y, GLsizei width, GLsizei height) {
    if (glstate->raster.bm_drawing) bitmap_flush();   // pending glBitmap pixels belong before this framebuffer access
    const GLuint itarget = what_target(target);
    // WARNING: It seems glColorMask has an impact on what channel are actually copied by this. The crude glReadPixel / glTexSubImage cannot emulate that, and proper emulation will take need 2 read pixels.
    //  And using the real glCopyTexSubImage2D needs that the FrameBuffer were data are read is compatible with the Texture it's copied to...
    DBG(printf("glCopyTexSubImage2D(%s, %i, %i, %i, %i, %i, %i, %i), bounded texture=%u format/type=%s, %s\n", PrintEnum(target), level, xoffset, yoffset, x, y, width, height, (glstate->texture.bound[glstate->texture.active][itarget])?glstate->texture.bound[glstate->texture.active][itarget]->texture:0, PrintEnum((glstate->texture.bound[glstate->texture.active][itarget])?glstate->texture.bound[glstate->texture.active][itarget]->format:0), PrintEnum((glstate->texture.bound[glstate->texture.active][itarget])?glstate->texture.bound[glstate->texture.active][itarget]->type:0));)
    // PUSH_IF_COMPILING(glCopyTexSubImage2D);
    FLUSH_BEGINEND;

    if (globals4es.skiptexcopies) {
        DBG(printf("glCopyTexSubImage2D skipped.\n"));
        return;
    }
 
    LOAD_GLES(glCopyTexSubImage2D);
    errorGL();
    realize_bound(glstate->texture.active, target);
    
    // "Unmap" if buffer mapped...
    glbuffer_t *pack = glstate->vao->pack;
    glbuffer_t *unpack = glstate->vao->unpack;
    glstate->vao->pack = NULL;
    glstate->vao->unpack = NULL;

    readfboBegin(); // multiple readfboBegin() can be chained...
    // the read and the upload below move GL4ES's own buffer: the application's swap state does not apply
    const GLboolean swap_pack = glstate->texture.pack_swap_bytes, swap_unpack = glstate->texture.unpack_swap_bytes;
    glstate->texture.pack_swap_bytes = glstate->texture.unpack_swap_bytes = GL_FALSE;

    gltexture_t* bound = glstate->texture.bound[glstate->texture.active][itarget];
#ifdef TEXSTREAM
    if (bound->streamed) {
        void* buff = GetStreamingBuffer(bound->streamingID);
        if ((bound->width == width) && (bound->height == height) && (xoffset == yoffset == 0)) {
            gl4es_glReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, buff);
        } else {
            void* tmp = malloc(width*height*2);
            gl4es_glReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, tmp);
            for (int y=0; y<height; y++) {
                memcpy(buff+((yoffset+y)*bound->width+xoffset)*2, tmp+y*width*2, width*2);
            }
            free(tmp);
        }
    } else 
#endif
    {
        int copytex = 0;
        if(glstate->fbo.current_fb->read_type==0) {
            LOAD_GLES(glGetIntegerv);
            gles_glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES, (GLint *) &glstate->fbo.current_fb->read_format);
            gles_glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE_OES, (GLint *) &glstate->fbo.current_fb->read_type);
        }
        copytex = ((bound->format==GL_RGBA && bound->type==GL_UNSIGNED_BYTE) 
            || (bound->format==glstate->fbo.current_fb->read_format && bound->type==glstate->fbo.current_fb->read_type));
        // an RGBA texture cannot be filled by GLES straight from a read buffer without alpha (see read_fb_lacks_alpha)
        int noalpha = (bound->format==GL_RGBA && read_fb_lacks_alpha());
        if (noalpha) copytex = 0;
        if (!noalpha && (copytex || !glstate->colormask[0] || !glstate->colormask[1] || !glstate->colormask[2] || !glstate->colormask[3])) {
            gles_glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
            if(((((bound->max_level == level) && (level || bound->mipmap_need)) && (globals4es.automipmap!=3) && (bound->mipmap_need!=0))) && !(bound->max_level==bound->base_level && bound->base_level==0)) {
                LOAD_GLES2_OR_OES(glGenerateMipmap);
                if(gles_glGenerateMipmap)
                    gles_glGenerateMipmap(to_target(itarget));
            }
        } else {
            void* tmp = malloc(width*height*4);
            GLenum format = bound->format;
            GLenum type = bound->type;
            gl4es_glReadPixels(x, y, width, height, format, type, tmp);
            // mipmap will be calculated buy gl4es_glTexSubImage2D
            gl4es_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, tmp);
            free(tmp);
        }
    }
    glstate->texture.pack_swap_bytes = swap_pack;
    glstate->texture.unpack_swap_bytes = swap_unpack;
    readfboEnd();
    // "Remap" if buffer mapped...
    glstate->vao->pack = pack;
    glstate->vao->unpack = unpack;
}

#ifndef NOEGL
extern int gl4es_backend_es3(void);   // glx/gl31_bridge.c
extern int gl4es_gl31_read_depth_stencil(int x, int y, int w, int h, unsigned format, unsigned type, void* pixels,
                                         int pack_align, int pack_row_length, int pack_skip_rows, int pack_skip_pixels);
#endif
// returns 1 when the pixels were written to the destination
static int read_pixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid * data) {
    DBG(printf("glReadPixels(%i, %i, %i, %i, %s, %s, 0x%p)\n", x, y, width, height, PrintEnum(format), PrintEnum(type), data);)
    FLUSH_BEGINEND;
    if (glstate->raster.bm_drawing) bitmap_flush();   // pending glBitmap pixels belong before this framebuffer access
    if (glstate->list.compiling && glstate->list.active) {
        errorShim(GL_INVALID_OPERATION);
        return 0;	// never in list
    }
    if (gl4es_hybrid_read_format(format)) {   // integer formats: read as GLES stores them (hybrid.c)
        readfboBegin();                      // the read framebuffer is bound only around the read
        gl4es_hybrid_ReadPixels(x, y, width, height, format, type, data);
        readfboEnd();
        noerrorShim();
        return 1;
    }
    LOAD_GLES(glReadPixels);
    errorGL();
    GLvoid* dst = data;
    if (glstate->vao->pack)
        dst = (char*)dst + (uintptr_t)glstate->vao->pack->data;
        
    readfboBegin();
#ifndef NOEGL
    // depth / stencil: GLES cannot read them; in a hybrid context GLADIATOR copies them on the GPU (ES 3.1)
    if (format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX || format == GL_DEPTH_STENCIL) {
        int r = gl4es_gl31_read_depth_stencil(x, y, width, height, format, type, dst, glstate->texture.pack_align,
                    glstate->texture.pack_row_length, glstate->texture.pack_skip_rows, glstate->texture.pack_skip_pixels);
        if (r >= 0) {
            // GLADIATOR restored its own draw framebuffer and program, not GL4ES's: put back what GL4ES has bound
            LOAD_GLES2_OR_OES(glBindFramebuffer);
            LOAD_GLES2(glUseProgram);
            gles_glBindFramebuffer(GL_FRAMEBUFFER, glstate->fbo.current_fb->id ? glstate->fbo.current_fb->id : glstate->fbo.mainfbo_fbo);
            if (gles_glUseProgram) gles_glUseProgram(glstate->gleshard->program);
            readfboEnd();
            if (r) noerrorShim(); else errorShim(GL_INVALID_OPERATION);
            return r ? 1 : 0;
        }
    }
    // float colour buffer (ES 3 + EXT_color_buffer_float): GLES reads it only as RGBA/FLOAT, unclamped, never as RGBA/UNSIGNED_BYTE
    if (gl4es_backend_es3() && format != GL_DEPTH_COMPONENT && format != GL_STENCIL_INDEX && format != GL_DEPTH_STENCIL) {
        LOAD_GLES(glGetIntegerv);
        GLint implt = 0;
        gles_glGetIntegerv(0x8B9A /* GL_IMPLEMENTATION_COLOR_READ_TYPE */, &implt);
        if (implt == GL_FLOAT || implt == GL_HALF_FLOAT || implt == GL_HALF_FLOAT_OES) {
            if (format == GL_RGBA && type == GL_FLOAT) {
                gles_glReadPixels(x, y, width, height, GL_RGBA, GL_FLOAT, dst);
            } else {
                GLvoid *fpix = malloc((size_t)width*height*4*sizeof(GLfloat));
                if (fpix) {
                    gles_glReadPixels(x, y, width, height, GL_RGBA, GL_FLOAT, fpix);
                    if (!pixel_convert(fpix, &dst, width, height, GL_RGBA, GL_FLOAT, format, type, 0, glstate->texture.pack_align))
                        LOGE("ReadPixels error: (GL_RGBA, GL_FLOAT -> %s, %s )\n", PrintEnum(format), PrintEnum(type));
                    free(fpix);
                }
            }
            readfboEnd();
            return 1;
        }
    }
#endif
    if ((format == GL_RGBA && type == GL_UNSIGNED_BYTE)     // should not use default GL_RGBA on Pandora as it's very slow...
       || (format == glstate->readf && type == glstate->readt)    // use the IMPLEMENTATION_READ too...
       || (format == GL_DEPTH_COMPONENT && (type == GL_FLOAT || type==GL_HALF_FLOAT)))   // this one will probably fail, as DEPTH is not readable on most GLES hardware 
    {
        // easy passthru
        gles_glReadPixels(x, y, width, height, format, type, dst);
        readfboEnd();
        return 1;
    }
    // grab data in GL_RGBA format
    int use_bgra = 0;
    if(glstate->readf==GL_BGRA && glstate->readt==GL_UNSIGNED_BYTE)
        use_bgra = 1;   // if IMPLEMENTATION_READ is BGRA, then use it as it's probably faster then RGBA.
    GLvoid *pixels = malloc(width*height*4);
    gles_glReadPixels(x, y, width, height, use_bgra?GL_BGRA:GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (! pixel_convert(pixels, &dst, width, height,
                        use_bgra?GL_BGRA:GL_RGBA, GL_UNSIGNED_BYTE, format, type, 0,glstate->texture.pack_align)) {
        LOGE("ReadPixels error: (%s, UNSIGNED_BYTE -> %s, %s )\n",
            PrintEnum(use_bgra?GL_BGRA:GL_RGBA), PrintEnum(format), PrintEnum(type));
    }
    free(pixels);
    readfboEnd();
    return 1;
}

void APIENTRY_GL4ES gl4es_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid * data) {
    // GL_PACK_SWAP_BYTES: GLES has none, so the pixels are swapped where the read left them
    if (read_pixels(x, y, width, height, format, type, data))
        gl4es_pack_swap(width, height, format, type, data);
}


void APIENTRY_GL4ES gl4es_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid * img) {
    DBG(printf("glGetTexImage(%s, %i, %s, %s, %p)\n", PrintEnum(target), level, PrintEnum(format), PrintEnum(type), img);)
    FLUSH_BEGINEND;
    if (gl4es_hybrid_tex_is_hybrid(target)) {   // storage kept by GLADIATOR (hybrid.c): read the real texture
        gl4es_hybrid_GetTexImage(target, level, format, type, img);
        gl4es_pack_swap(nlevel(glstate->texture.bound[glstate->texture.active][what_target(target)]->width, level),
                        nlevel(glstate->texture.bound[glstate->texture.active][what_target(target)]->height, level), format, type, img);
        noerrorShim();
        return;
    }
    const GLuint itarget = what_target(target);

    realize_bound(glstate->texture.active, target);
       
    gltexture_t* bound = glstate->texture.bound[glstate->texture.active][itarget];
    int width = bound->width;
    int height = bound->height;
    int nwidth = bound->nwidth;
    int nheight = bound->nheight;
    int shrink = bound->shrink;
    if (level != 0) {
        //printf("STUBBED glGetTexImage with level=%i\n", level);
        void* tmp = malloc(width*height*pixel_sizeof(format, type)); // tmp space...
        void* tmp2;
        const GLboolean swap_pack = glstate->texture.pack_swap_bytes;
        glstate->texture.pack_swap_bytes = GL_FALSE;   // scratch buffer: swapped once, at the end
        gl4es_glGetTexImage(map_tex_target(target), 0, format, type, tmp);
        glstate->texture.pack_swap_bytes = swap_pack;
        for (int i=0; i<level; i++) {
            pixel_halfscale(tmp, &tmp2, width, height, format, type);
            free(tmp);
            tmp = tmp2;
            width = nlevel(width, 1);
            height = nlevel(height, 1);
        }
        memcpy(img, tmp, width*height*pixel_sizeof(format, type));
        free(tmp);
        gl4es_pack_swap(width, height, format, type, img);
        return;
    }
    
    if (target!=GL_TEXTURE_2D) {
        return;
    }

    DBG(printf("glGetTexImage(%s, %i, %s, %s, 0x%p), texture=0x%x, size=%i,%i\n", PrintEnum(target), level, PrintEnum(format), PrintEnum(type), img, bound->glname, width, height);)
    
    GLvoid *dst = img;
    if (glstate->vao->pack)
        dst = (char*)dst + (uintptr_t)glstate->vao->pack->data;
#ifdef TEXSTREAM
    if (globals4es.texstream && bound->streamed) {
        noerrorShim();
        pixel_convert(GetStreamingBuffer(bound->streamingID), &dst, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, format, type, 0, glstate->texture.unpack_align);
        readfboEnd();
        return;
    }
#endif
    if (globals4es.texcopydata && bound->data) {
        //printf("texcopydata* glGetTexImage(0x%04X, %d, 0x%04x, 0x%04X, %p)\n", target, level, format, type, img);
        noerrorShim();
        if (!pixel_convert(bound->data, &dst, width, height, GL_RGBA, GL_UNSIGNED_BYTE, format, type, 0, glstate->texture.pack_align))
            printf("LIBGL: Error on pixel_convert while glGetTexImage\n");
    } else {
        // Setup an FBO the same size of the texture
        GLuint oldBind = bound->glname;
        GLuint old_fbo = glstate->fbo.current_fb->id;
        GLuint fbo;
    
        // if the texture is not RGBA or RGB or ALPHA, the "just attach texture to the fbo" trick will not work, and a full Blit has to be done
        if((bound->format==GL_RGBA || bound->format==GL_RGB || (bound->format==GL_BGRA && hardext.bgra8888) || bound->format==GL_ALPHA) && (shrink==0)) {
            gl4es_glGenFramebuffers(1, &fbo);
            gl4es_glBindFramebuffer(GL_FRAMEBUFFER_OES, fbo);
            gl4es_glFramebufferTexture2D(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, oldBind, 0);
            // Read the pixels!
            gl4es_glReadPixels(0, nheight-height, width, height, format, type, img);	// using "full" version with conversion of format/type
            gl4es_glBindFramebuffer(GL_FRAMEBUFFER_OES, old_fbo);
            gl4es_glDeleteFramebuffers(1, &fbo);
            noerrorShim();
        } else {
            gl4es_glGenFramebuffers(1, &fbo);
            gl4es_glBindFramebuffer(GL_FRAMEBUFFER_OES, fbo);
            GLuint temptex;
            gl4es_glGenTextures(1, &temptex);
            gl4es_glBindTexture(GL_TEXTURE_2D, temptex);
            gl4es_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, nwidth<<shrink, nheight<<shrink, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            gl4es_glFramebufferTexture2D(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_TEXTURE_2D, temptex, 0);
            gl4es_glBindTexture(GL_TEXTURE_2D, oldBind);
            // blit the texture
            gl4es_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            gl4es_glClear(GL_COLOR_BUFFER_BIT);
            gl4es_blitTexture(oldBind, 0.f, 0.f, width, height, nwidth, nheight, 1.0f, 1.0f, nwidth<<shrink, nheight<<shrink, 0, 0, BLIT_OPAQUE);
            // Read the pixels!
            gl4es_glReadPixels(0, (nheight-height)<<shrink, width<<shrink, height<<shrink, format, type, img);	// using "full" version with conversion of format/type
            gl4es_glBindFramebuffer(GL_FRAMEBUFFER_OES, old_fbo);
            gl4es_glDeleteFramebuffers(1, &fbo);
            gl4es_glDeleteTextures(1, &temptex);
            noerrorShim();
        }
    }
}

void APIENTRY_GL4ES gl4es_glCopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
            GLsizei width, GLint border) {
    gl4es_glCopyTexImage2D(GL_TEXTURE_1D, level, internalformat, x, y, width, 1, border);
            
}

void APIENTRY_GL4ES gl4es_glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y,
                                GLsizei width) {
    gl4es_glCopyTexSubImage2D(GL_TEXTURE_1D, level, xoffset, 0, x, y, width, 1);
}
                                
//Direct wrapper
AliasExport(void,glGetTexImage,,(GLenum target, GLint level, GLenum format, GLenum type, GLvoid * img));
AliasExport(void,glReadPixels,,(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid * data));
AliasExport(void,glCopyTexImage1D,,(GLenum target,  GLint level,  GLenum internalformat,  GLint x,  GLint y, GLsizei width,  GLint border));
AliasExport(void,glCopyTexImage2D,,(GLenum target,  GLint level,  GLenum internalformat,  GLint x,  GLint y, GLsizei width,  GLsizei height,  GLint border));
AliasExport(void,glCopyTexSubImage2D,,(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height));
AliasExport(void,glCopyTexSubImage1D,,(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width));

