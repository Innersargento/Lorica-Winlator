#include "framebuffers.h"

#include "../glx/hardext.h"
#include "blit.h"
#include "debug.h"
#include "fpe.h"
#include "gl4es.h"
#include "hybrid.h"
#include "glstate.h"
#include "init.h"
#include "loader.h"

//#define DEBUG
#ifdef DEBUG
#define DBG(a) a
#else
#define DBG(a)
#endif

KHASH_MAP_IMPL_INT(renderbufferlist_t, glrenderbuffer_t *);
KHASH_MAP_IMPL_INT(framebufferlist_t, glframebuffer_t *);

//extern void* eglGetProcAddress(const char* name);

int npot(int n);
int wrap_npot(GLenum wrap);
static void gl4es_fbo_drop_proxy(glframebuffer_t* fb);
#ifndef NOEGL
extern unsigned gl4es_gl31_rb_format(unsigned f);                                          // glx/gl31_bridge.c
#endif

glframebuffer_t* find_framebuffer(GLuint framebuffer) {
    // Get a framebuffer based on ID
    if (framebuffer == 0) return glstate->fbo.fbo_0; // NULL or fbo_0 ?
    khint_t k;
    khash_t(framebufferlist_t) *list = glstate->fbo.framebufferlist;
    k = kh_get(framebufferlist_t, list, framebuffer);
    
    if (k != kh_end(list)){
        return kh_value(list, k);
    }
    return NULL;
}

glframebuffer_t* get_framebuffer(GLenum target) {
    switch (target) {
        case GL_FRAMEBUFFER: return glstate->fbo.current_fb;
        case GL_READ_FRAMEBUFFER: return glstate->fbo.fbo_read;
        case GL_DRAW_FRAMEBUFFER: return glstate->fbo.fbo_draw;
    }
    return NULL;
}


void readfboBegin() {
    if (glstate->fbo.fbo_read == glstate->fbo.fbo_draw)
        return;
    DBG(printf("readfboBegin, fbo status read=%u, draw=%u, main=%u, current=%u\n", glstate->fbo.fbo_read->id, glstate->fbo.fbo_draw->id, glstate->fbo.mainfbo_fbo, glstate->fbo.current_fb->id);)
    if(glstate->fbo.fbo_read==glstate->fbo.current_fb)
        return;
    glstate->fbo.current_fb = glstate->fbo.fbo_read;
    GLuint fbo = glstate->fbo.fbo_read->id;
    if (!fbo)
        fbo = glstate->fbo.mainfbo_fbo;
    LOAD_GLES2_OR_OES(glBindFramebuffer);
    gles_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

void readfboEnd() {
    if (glstate->fbo.fbo_read->id == glstate->fbo.fbo_draw->id)
        return;
    DBG(printf("readfboEnd, fbo status read=%p, draw=%p, main=%u, current=%p\n", glstate->fbo.fbo_read, glstate->fbo.fbo_draw, glstate->fbo.mainfbo_fbo, glstate->fbo.current_fb);)
    if(glstate->fbo.fbo_draw==glstate->fbo.current_fb)
        return;
    glstate->fbo.current_fb = glstate->fbo.fbo_draw;
    GLuint fbo = glstate->fbo.fbo_draw->id;
    if (!fbo)
        fbo = glstate->fbo.mainfbo_fbo;
    LOAD_GLES2_OR_OES(glBindFramebuffer);
    gles_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

glrenderbuffer_t* find_renderbuffer(GLuint renderbuffer) {
    // Get a renderbuffer based on ID
    if (renderbuffer == 0) return glstate->fbo.default_rb;
    khint_t k;
    khash_t(renderbufferlist_t) *list = glstate->fbo.renderbufferlist;
    k = kh_get(renderbufferlist_t, list, renderbuffer);
    
    if (k != kh_end(list)){
        return kh_value(list, k);
    }
    return NULL;
}

void APIENTRY_GL4ES gl4es_glGenFramebuffers(GLsizei n, GLuint *ids) {
    DBG(printf("glGenFramebuffers(%i, %p)\n", n, ids);)
    LOAD_GLES2_OR_OES(glGenFramebuffers);
    GLsizei m = 0;
    while(glstate->fbo.old && (glstate->fbo.old->nbr>0) && (n-m>0)) {
        DBG(printf("Recycled 1 FBO\n");)
        ids[m++] = glstate->fbo.old->fbos[--glstate->fbo.old->nbr];
    }
    noerrorShim();
    if(n-m) {
        errorGL();
        gles_glGenFramebuffers(n-m, ids+m);
    }
    // track the framebuffers...
    int ret;
    khint_t k;
    khash_t(framebufferlist_t) *list = glstate->fbo.framebufferlist;
    for(int i=0; i<n; ++i) {
        k = kh_put(framebufferlist_t, list, ids[i], &ret);
        glframebuffer_t *fb = kh_value(list, k) = malloc(sizeof(glframebuffer_t));
        memset(fb, 0, sizeof(glframebuffer_t));
        fb->id = ids[i];
        fb->n_draw = 0; // correct?
    }
}

void APIENTRY_GL4ES gl4es_glDeleteFramebuffers(GLsizei n, GLuint *framebuffers) {
    DBG(printf("glDeleteFramebuffers(%i, %p), framebuffers[0]=%u\n", n, framebuffers, framebuffers[0]);)
    // delete tracking
    if (glstate->fbo.framebufferlist)
        for (int i=0; i<n; i++) {
            khint_t k;
            glframebuffer_t *fb;
            for (int i = 0; i < n; i++) {
                GLuint t = framebuffers[i];
                if(t) {
                    k = kh_get(framebufferlist_t, glstate->fbo.framebufferlist, t);
                    if (k != kh_end(glstate->fbo.framebufferlist)) {
                        fb = kh_value(glstate->fbo.framebufferlist, k);
                        // detach texture...
                        for(int j=0; j<MAX_DRAW_BUFFERS; ++j) {
                            if(fb->color[j] && fb->t_color[j]!=GL_RENDERBUFFER) {
                                gltexture_t *tex = gl4es_getTexture(fb->t_color[j], fb->color[j]);
                                if(tex) {
                                    tex->binded_fbo = 0;
                                    tex->binded_attachment = 0;
                                }
                            }
                        }
                        if(fb->depth && fb->t_depth!=GL_RENDERBUFFER) {
                            gltexture_t *tex = gl4es_getTexture(fb->t_depth, fb->depth);
                            if(tex) {
                                tex->binded_fbo = 0;
                                tex->binded_attachment = 0;
                                tex->renderdepth = 0;
                            }
                        }
                        if(fb->stencil && fb->t_stencil!=GL_RENDERBUFFER) {
                            gltexture_t *tex = gl4es_getTexture(fb->t_stencil, fb->stencil);
                            if(tex) {
                                tex->binded_fbo = 0;
                                tex->binded_attachment = 0;
                                tex->renderstencil = 0;
                            }
                        }
                        if (glstate->fbo.current_fb == fb) {
                            glstate->fbo.current_fb = 0;
                        }
                        if (glstate->fbo.fbo_read == fb) {
                            glstate->fbo.fbo_read = 0;
                        }
                        if (glstate->fbo.fbo_draw == fb) {
                            glstate->fbo.fbo_draw = 0;
                        }
                        gl4es_fbo_drop_proxy(fb);
                        free(fb);
                        kh_del(framebufferlist_t, glstate->fbo.framebufferlist, k);                        
                    }
                }
            }
        }

    if (globals4es.recyclefbo) {
        DBG(printf("Recycling %i FBOs\n", n);)
        noerrorShim();
        if(glstate->fbo.old->cap == 0) {
            glstate->fbo.old->cap = 16;
            glstate->fbo.old->fbos = (GLuint*)malloc(glstate->fbo.old->cap * sizeof(GLuint));
        }
        if (glstate->fbo.old->nbr+n > glstate->fbo.old->cap) {
            glstate->fbo.old->cap += n;
            glstate->fbo.old->fbos = (GLuint*)realloc(glstate->fbo.old->fbos, glstate->fbo.old->cap *sizeof(GLuint));
        }
        memcpy(glstate->fbo.old->fbos+glstate->fbo.old->nbr, framebuffers, n*sizeof(GLuint));
        glstate->fbo.old->nbr += n;
    } else {
        LOAD_GLES2_OR_OES(glDeleteFramebuffers);
        errorGL();
        gles_glDeleteFramebuffers(n, framebuffers);
    }
}

GLboolean APIENTRY_GL4ES gl4es_glIsFramebuffer(GLuint framebuffer) {
    DBG(printf("glIsFramebuffer(%u)\n", framebuffer);)
    LOAD_GLES2_OR_OES(glIsFramebuffer);
    
    errorGL();
    return find_framebuffer(framebuffer)!=NULL;
}


GLenum APIENTRY_GL4ES gl4es_glCheckFramebufferStatus(GLenum target) {
    GLenum result;
    if(glstate->fbo.internal) {
        result = glstate->fbo.fb_status;
        noerrorShim();
     } else {
        LOAD_GLES2_OR_OES(glCheckFramebufferStatus);
        
        errorGL();
        GLenum rtarget = target;
        if(target==GL_READ_FRAMEBUFFER)
            return GL_FRAMEBUFFER_COMPLETE; // cheating here
        if(target==GL_DRAW_FRAMEBUFFER)
            rtarget = GL_FRAMEBUFFER;
        result = gles_glCheckFramebufferStatus(rtarget);
#ifndef NOEGL
        if (globals4es.verbose && gl4es_hybrid() && result != GL_FRAMEBUFFER_COMPLETE && glstate->fbo.current_fb)
            LOGD("LORICA FBO: framebuffer %u incomplete (0x%x)\n", glstate->fbo.current_fb->id, result);
#endif
     }
    DBG(printf("glCheckFramebufferStatus(0x%04X)=0x%04X\n", target, result);)
    return result;
}

void APIENTRY_GL4ES gl4es_glBindFramebuffer(GLenum target, GLuint framebuffer) {
    if (glstate->raster.bm_drawing) bitmap_flush();   // pending glBitmap pixels belong before this framebuffer access
    DBG(printf("glBindFramebuffer(%s, %u), list=%s, glstate->fbo.current_fb=%d (draw=%d, read=%d)\n", PrintEnum(target), framebuffer, glstate->list.active?"active":"none", glstate->fbo.current_fb->id, glstate->fbo.fbo_draw->id, glstate->fbo.fbo_read->id);)
    PUSH_IF_COMPILING(glBindFramebuffer);
    LOAD_GLES2_OR_OES(glBindFramebuffer);
//    LOAD_GLES2_OR_OES(glCheckFramebufferStatus);
    LOAD_GLES(glGetError);

    glframebuffer_t *fb = find_framebuffer(framebuffer);
    if(!fb) {
        errorShim(GL_INVALID_VALUE);
        return;
    }
    if(fb->remap) fb->proxy_dirty = 1;   // attachments may have changed through another path: the proxy is rebuilt
        
    if (target == GL_FRAMEBUFFER) {
        glstate->fbo.fbo_read = fb;
        glstate->fbo.fbo_draw = fb;
    }
    
    if (target == GL_READ_FRAMEBUFFER) {
        glstate->fbo.fbo_read = fb;
        noerrorShim();
        glstate->fbo.fb_status = GL_FRAMEBUFFER_COMPLETE;
        glstate->fbo.internal = 1;
        return;    //don't bind for now
    } else glstate->fbo.internal = 0;
        
    if (target == GL_DRAW_FRAMEBUFFER) {
        target = GL_FRAMEBUFFER;
        glstate->fbo.fbo_draw = fb;
    }
    
    if (target != GL_FRAMEBUFFER) {
        errorShim(GL_INVALID_ENUM);
        return;
    }

    if(framebuffer==0)
        framebuffer = glstate->fbo.mainfbo_fbo;

    glstate->fbo.current_fb = fb;
        
    gles_glBindFramebuffer(target, framebuffer);
    GLenum err=gles_glGetError();
    errorShim(err);
    
//    glstate->fbo.fb_status = (framebuffer==0)?GL_FRAMEBUFFER_COMPLETE:gles_glCheckFramebufferStatus(target);
}

GLenum ReadDraw_Push(GLenum target) {
    if(target==GL_FRAMEBUFFER)
        return GL_FRAMEBUFFER;
    LOAD_GLES2_OR_OES(glBindFramebuffer);
    if(target==GL_DRAW_FRAMEBUFFER) {
        if(glstate->fbo.current_fb!=glstate->fbo.fbo_draw)
            gles_glBindFramebuffer(GL_FRAMEBUFFER, (glstate->fbo.fbo_draw->id)?glstate->fbo.fbo_draw->id:glstate->fbo.mainfbo_fbo);
        return GL_FRAMEBUFFER;
    }
    if(target==GL_READ_FRAMEBUFFER) {
        if(glstate->fbo.current_fb!=glstate->fbo.fbo_read)
            gles_glBindFramebuffer(GL_FRAMEBUFFER, (glstate->fbo.fbo_read->id)?glstate->fbo.fbo_read->id:glstate->fbo.mainfbo_fbo);
        return GL_FRAMEBUFFER;
    }
    return target;
}
void ReadDraw_Pop(GLenum target) {
    if(target==GL_FRAMEBUFFER)
        return;
    LOAD_GLES2_OR_OES(glBindFramebuffer);
    if(target==GL_DRAW_FRAMEBUFFER && glstate->fbo.current_fb!=glstate->fbo.fbo_draw) {
        gles_glBindFramebuffer(GL_FRAMEBUFFER, (glstate->fbo.current_fb->id)?glstate->fbo.current_fb->id:glstate->fbo.mainfbo_fbo);
    }
    if(target==GL_READ_FRAMEBUFFER && glstate->fbo.current_fb!=glstate->fbo.fbo_read) {
        gles_glBindFramebuffer(GL_FRAMEBUFFER, (glstate->fbo.current_fb->id)?glstate->fbo.current_fb->id:glstate->fbo.mainfbo_fbo);
    }
}

void SetAttachment(glframebuffer_t* fb, GLenum attachment, GLenum atttarget, GLuint att, int level)
{
    fb->proxy_dirty = 1;    // draw buffer proxy (gl4es_fbo_draw_begin) follows the attachments
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
    case GL_COLOR_ATTACHMENT1:
    case GL_COLOR_ATTACHMENT2:
    case GL_COLOR_ATTACHMENT3:
    case GL_COLOR_ATTACHMENT4:
    case GL_COLOR_ATTACHMENT5:
    case GL_COLOR_ATTACHMENT6:
    case GL_COLOR_ATTACHMENT7:
    case GL_COLOR_ATTACHMENT8:
    case GL_COLOR_ATTACHMENT9:
    case GL_COLOR_ATTACHMENT10:
    case GL_COLOR_ATTACHMENT11:
    case GL_COLOR_ATTACHMENT12:
    case GL_COLOR_ATTACHMENT13:
    case GL_COLOR_ATTACHMENT14:
    case GL_COLOR_ATTACHMENT15:
        fb->color[attachment - GL_COLOR_ATTACHMENT0] = att;
        fb->l_color[attachment - GL_COLOR_ATTACHMENT0] = level;
        fb->t_color[attachment - GL_COLOR_ATTACHMENT0] = atttarget;
        break;
    case GL_DEPTH_ATTACHMENT:
        fb->depth = att;
        fb->t_depth = atttarget;
        fb->l_depth = 0;
        break;
    case GL_STENCIL_ATTACHMENT:
        fb->stencil = att;
        fb->t_stencil = atttarget;
        fb->l_stencil = 0;
        break;
    case GL_DEPTH_STENCIL_ATTACHMENT:
        fb->depth = att;
        fb->t_depth = atttarget;
        fb->l_depth = 0;
        fb->stencil = att;
        fb->t_stencil = atttarget;
        fb->l_stencil = 0;
        break;
    }
}
GLuint GetAttachment(glframebuffer_t* fb, GLenum attachment) {
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
    case GL_COLOR_ATTACHMENT1:
    case GL_COLOR_ATTACHMENT2:
    case GL_COLOR_ATTACHMENT3:
    case GL_COLOR_ATTACHMENT4:
    case GL_COLOR_ATTACHMENT5:
    case GL_COLOR_ATTACHMENT6:
    case GL_COLOR_ATTACHMENT7:
    case GL_COLOR_ATTACHMENT8:
    case GL_COLOR_ATTACHMENT9:
    case GL_COLOR_ATTACHMENT10:
    case GL_COLOR_ATTACHMENT11:
    case GL_COLOR_ATTACHMENT12:
    case GL_COLOR_ATTACHMENT13:
    case GL_COLOR_ATTACHMENT14:
    case GL_COLOR_ATTACHMENT15:
        return fb->color[attachment - GL_COLOR_ATTACHMENT0];
    case GL_DEPTH_ATTACHMENT:
        return fb->depth;
    case GL_STENCIL_ATTACHMENT:
        return fb->stencil;
    case GL_DEPTH_STENCIL_ATTACHMENT:
        // is that possible?
        return fb->depth;
    }
    return 0;
}
GLenum GetAttachmentType(glframebuffer_t* fb, GLenum attachment) {
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
    case GL_COLOR_ATTACHMENT1:
    case GL_COLOR_ATTACHMENT2:
    case GL_COLOR_ATTACHMENT3:
    case GL_COLOR_ATTACHMENT4:
    case GL_COLOR_ATTACHMENT5:
    case GL_COLOR_ATTACHMENT6:
    case GL_COLOR_ATTACHMENT7:
    case GL_COLOR_ATTACHMENT8:
    case GL_COLOR_ATTACHMENT9:
    case GL_COLOR_ATTACHMENT10:
    case GL_COLOR_ATTACHMENT11:
    case GL_COLOR_ATTACHMENT12:
    case GL_COLOR_ATTACHMENT13:
    case GL_COLOR_ATTACHMENT14:
    case GL_COLOR_ATTACHMENT15:
        return fb->t_color[attachment - GL_COLOR_ATTACHMENT0];
    case GL_DEPTH_ATTACHMENT:
        return fb->t_depth;
    case GL_STENCIL_ATTACHMENT:
        return fb->t_stencil;
    case GL_DEPTH_STENCIL_ATTACHMENT:
        return fb->t_depth;
    }
    return 0;
}
int GetAttachmentLevel(glframebuffer_t* fb, GLenum attachment) {
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
    case GL_COLOR_ATTACHMENT1:
    case GL_COLOR_ATTACHMENT2:
    case GL_COLOR_ATTACHMENT3:
    case GL_COLOR_ATTACHMENT4:
    case GL_COLOR_ATTACHMENT5:
    case GL_COLOR_ATTACHMENT6:
    case GL_COLOR_ATTACHMENT7:
    case GL_COLOR_ATTACHMENT8:
    case GL_COLOR_ATTACHMENT9:
    case GL_COLOR_ATTACHMENT10:
    case GL_COLOR_ATTACHMENT11:
    case GL_COLOR_ATTACHMENT12:
    case GL_COLOR_ATTACHMENT13:
    case GL_COLOR_ATTACHMENT14:
    case GL_COLOR_ATTACHMENT15:
        return fb->l_color[attachment - GL_COLOR_ATTACHMENT0];
    case GL_DEPTH_ATTACHMENT:
        return fb->l_depth;
    case GL_STENCIL_ATTACHMENT:
        return fb->l_stencil;
    case GL_DEPTH_STENCIL_ATTACHMENT:
        return fb->l_depth;
    }
    return 0;
}

void APIENTRY_GL4ES gl4es_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    DBG(printf("glFramebufferTexture2D(%s, %s, %s, %u, %i) glstate->fbo.current_fb=%d (draw=%d, read=%d)\n", PrintEnum(target), PrintEnum(attachment), PrintEnum(textarget), texture, level, glstate->fbo.current_fb->id, glstate->fbo.fbo_draw->id, glstate->fbo.fbo_read->id);)
    static GLuint scrap_tex = 0;
    static int scrap_width = 0;
    static int scrap_height = 0;
    
    LOAD_GLES2_OR_OES(glFramebufferTexture2D);
    LOAD_GLES(glTexImage2D);
    LOAD_GLES(glBindTexture);
    LOAD_GLES(glActiveTexture);
    void gles_glTexParameteri(glTexParameteri_ARG_EXPAND); //LOAD_GLES(glTexParameteri);

    glframebuffer_t *fb = get_framebuffer(target);
    if(!fb) {
        errorShim(GL_INVALID_ENUM);
        return;
    }

    if( !(attachment>=GL_COLOR_ATTACHMENT0 && attachment<(GL_COLOR_ATTACHMENT0+hardext.maxcolorattach))
     && attachment!=GL_DEPTH_ATTACHMENT 
     && attachment!=GL_STENCIL_ATTACHMENT 
     && attachment!=GL_DEPTH_STENCIL_ATTACHMENT) {
         errorShim(GL_INVALID_ENUM);
         return;
     }
    
    int twidth = 0, theight = 0;
    // find texture and get it's real name
    gltexture_t *tex = NULL;
    if (texture) {
        tex = gl4es_getTexture(textarget, texture);

        if (!tex) {
            LOGE("texture for FBO not found, name=%u\n", texture);
        } else {
            texture = tex->glname;
            tex->fbtex_ratio = (globals4es.fbtexscale > 0.0f) ? globals4es.fbtexscale : 0.0f;

            // check if texture was shrunk or if fb texture is being scaled...
            // (not for a texture whose storage GLADIATOR keeps, hybrid.c: re-creating it with GL4ES's format/type would replace it)
            if (tex->hybrid) {
                // nothing to adjust: full size, real format
            } else if (globals4es.fbtexscale > 0.0f || tex->shrink || tex->useratio || (tex->adjust && (hardext.npot==1 || hardext.npot==2) && !globals4es.potframebuffer)) {
                LOGD("%s texture for FBO\n",(tex->useratio)?"going back to npot size pot'ed":"unshrinking shrinked");
                if(tex->shrink || tex->useratio) {
                    if(tex->useratio) {
                        tex->width = tex->nwidth/tex->ratiox;
                        tex->height = tex->nheight/tex->ratioy;
                    } else {
                        tex->width *= 1<<tex->shrink;
                        tex->height *= 1<<tex->shrink;
                    }
                }

                // Use FBO Ratio
                if (tex->fbtex_ratio > 0.0f) {
                    tex->width *= tex->fbtex_ratio;
                    tex->height *= tex->fbtex_ratio;
                }

                tex->nwidth = (hardext.npot>0 || hardext.esversion>1)?tex->width:npot(tex->width);
                tex->nheight = (hardext.npot>0 || hardext.esversion>1)?tex->height:npot(tex->height);
                tex->adjustxy[0] = (float)tex->width / tex->nwidth;
                tex->adjustxy[1] = (float)tex->height / tex->nheight;
                tex->adjust=(tex->width!=tex->nwidth || tex->height!=tex->nheight);
                tex->shrink = 0; tex->useratio = 0;
                int oldactive = glstate->texture.active;
                if(oldactive) gles_glActiveTexture(GL_TEXTURE0);
                gltexture_t *bound = glstate->texture.bound[0/*glstate->texture.active*/][ENABLED_TEX2D];
                GLuint oldtex = bound->glname;
                if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, tex->glname);
                gles_glTexImage2D(GL_TEXTURE_2D, 0, tex->format, tex->nwidth, tex->nheight, 0, tex->format, tex->type, NULL);
                if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, oldtex);
                if(oldactive) gles_glActiveTexture(GL_TEXTURE0+oldactive);
            }
            int need_change = (globals4es.potframebuffer && (npot(twidth)!=twidth || npot(theight)!=theight) && !tex->hybrid)?1:0;
            if(!tex->hybrid && ((tex->type==GL_FLOAT && !hardext.floatfbo) || (tex->type==GL_HALF_FLOAT_OES && !hardext.halffloatfbo))) {
                need_change += 2;
                tex->type = GL_UNSIGNED_BYTE;
            }
            if(!tex->hybrid && tex->format==GL_BGRA && (globals4es.nobgra || !hardext.bgra8888)) {
                if(need_change<2) need_change += 2;
                tex->format = GL_RGBA;
            }
            if(need_change) {
                // check if POT size is asked
                LOGD("Recreate a texture for a FBO (%s%s%s)\n", (need_change&1)?"POT":"", (need_change==3)?" & ":"", (need_change&2)?"Format/Type":"");
                if(need_change&1) {
                    twidth = tex->nwidth = npot(tex->nwidth);
                    theight = tex->nheight = npot(tex->nheight);
                    tex->adjustxy[0] = (float)tex->width / tex->nwidth;
                    tex->adjustxy[1] = (float)tex->height / tex->nheight;
                    tex->adjust=(tex->width!=tex->nwidth || tex->height!=tex->nheight);
                }
                int oldactive = glstate->texture.active;
                if(oldactive) gles_glActiveTexture(GL_TEXTURE0);
                gltexture_t *bound = glstate->texture.bound[0/*glstate->texture.active*/][ENABLED_TEX2D];
                GLuint oldtex = bound->glname;
                if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, tex->glname);
                gles_glTexImage2D(GL_TEXTURE_2D, 0, tex->format, tex->nwidth, tex->nheight, 0, tex->format, tex->type, NULL);
                if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, oldtex);
                if(oldactive) gles_glActiveTexture(GL_TEXTURE0+oldactive);
            }
            twidth = tex->nwidth;
            theight = tex->nheight;
 /*           if ((tex->width<32) || (tex->height<32)) {
                printf("LIBGL: enlarging too-small texture for FBO\n");
                tex->nwidth = (tex->nwidth<32)?32:tex->nwidth;
                tex->nheight = (tex->nheight<32)?32:tex->nheight;
                tex->shrink = 0;
                gltexture_t *bound = glstate->texture.bound[glstate->texture.active][ENABLED_TEX2D];
                GLuint oldtex = bound->glname;
                if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, tex->glname);
                gles_glTexImage2D(GL_TEXTURE_2D, 0, tex->format, tex->nwidth, tex->nheight, 0, tex->format, tex->type, NULL);
                if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, oldtex);
            }*/

            fb->width  = twidth;
            fb->height = theight;

            DBG(printf("found texture, glname=%u, size=%ix%i(%ix%i), format/type=%s/%s\n", texture, tex->width, tex->height, tex->nwidth, tex->nheight, PrintEnum(tex->format), PrintEnum(tex->type));)
        }
    }
    
    GLenum ntarget = ReadDraw_Push(target);

    GLuint old_attachment = GetAttachment(fb, attachment);
    GLuint old_attachment_type = GetAttachmentType(fb, attachment);
    if(old_attachment) {
        gltexture_t* old = gl4es_getTexture(old_attachment_type, old_attachment);
        if(old) {
            DBG(printf("Detach Texture %d from FBO %d as Attachement %s\n", old->glname, old->binded_fbo, PrintEnum(old->binded_attachment));)
            old->binded_fbo = 0;
            old->binded_attachment = 0;
        } else {
            DBG(printf("Old attachement %s for FBO %d not found in textures\n", PrintEnum(attachment), fb->id);)
        }
    }

    if(tex) {
        DBG(printf("Attach Texture %d to FBO %d as Attachement %s\n", tex->glname, fb->id, PrintEnum(attachment));)
        tex->binded_fbo = fb->id;
        tex->binded_attachment = attachment;
    }

    if ((old_attachment_type == textarget) && (old_attachment == (tex?tex->texture:texture)))
    {
        // no need to reattach
        noerrorShim();
        return;
    }

    SetAttachment(fb, attachment, textarget, tex?tex->texture:texture, level);

    if(attachment>=GL_COLOR_ATTACHMENT0 && attachment<(GL_COLOR_ATTACHMENT0+hardext.maxcolorattach) && tex) {
        gltexture_t *bound = glstate->texture.bound[0/*glstate->texture.active*/][ENABLED_TEX2D];
        if((hardext.npot==1 || hardext.npot==2) && (!tex->actual.wrap_s || !tex->actual.wrap_t || !wrap_npot(tex->actual.wrap_s) || !wrap_npot(tex->actual.wrap_t))) {
            tex->sampler.wrap_s = tex->sampler.wrap_t = GL_CLAMP_TO_EDGE;
            tex->adjust = 0;
        }
        //npot==2 and 3 should support that, but let's ignore that for now and force no mipmap for texture attached to fbo...
        if(!tex->actual.min_filter || !minmag_npot(tex->actual.min_filter) || tex->actual.min_filter!=tex->sampler.min_filter) {
            if(hardext.npot<2) {
                tex->sampler.min_filter = minmag_forcenpot(tex->sampler.min_filter);
                tex->adjust = 0;
                tex->mipmap_need = 0;
                tex->mipmap_auto = 0;
            }
        }
        realize_1texture(map_tex_target(textarget), -1, tex, NULL);
    }

    if(attachment==GL_DEPTH_ATTACHMENT /*&& hardext.depthtex==0*/) {
        noerrorShim();
        if (level!=0) return;
        if(hardext.depthtex && (tex || !texture)) {
            // depth texture supported!
            //check if texture needs to be re-created as true depth texture
            if(tex && !(tex->format==GL_DEPTH_COMPONENT || tex->format==GL_DEPTH_STENCIL)) {
                tex->format = GL_DEPTH_COMPONENT;
                if(tex->type!=GL_UNSIGNED_INT && tex->type!=GL_UNSIGNED_SHORT && tex->type!=GL_FLOAT) tex->type = (hardext.depth24)?GL_UNSIGNED_INT:GL_UNSIGNED_SHORT;
                tex->fpe_format = FPE_TEX_DEPTH;
                realize_textures(0);
                int oldactive = glstate->texture.active;
                if(oldactive) gles_glActiveTexture(GL_TEXTURE0);
                gltexture_t *bound = glstate->texture.bound[0/*glstate->texture.active*/][ENABLED_TEX2D];
                GLuint oldtex = bound->glname;
                if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, tex->glname);
                gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                gles_glTexImage2D(GL_TEXTURE_2D, 0, tex->format, tex->nwidth, tex->nheight, 0, tex->format, tex->type, NULL);
                if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, oldtex);
                if(oldactive) gles_glActiveTexture(GL_TEXTURE0+oldactive);
            }
            // bind the depth texture...
            gles_glFramebufferTexture2D(ntarget, attachment, GL_TEXTURE_2D, texture, 0);
        } else {
            // let's create a renderbuffer and attach it instead of the (presumably) depth texture
            if(tex && !tex->renderdepth) {
                gl4es_glGenRenderbuffers(1, &tex->renderdepth);
                gl4es_glBindRenderbuffer(GL_RENDERBUFFER, tex->renderdepth);
                gl4es_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, twidth, theight);
                gl4es_glBindRenderbuffer(GL_RENDERBUFFER, 0);
            }
            gl4es_glFramebufferRenderbuffer(ntarget, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, tex?tex->renderdepth:0);
        }
        errorGL();
        ReadDraw_Pop(target);
        return;
    }
    if(attachment==GL_STENCIL_ATTACHMENT /*&& hardext.depthtex==0*/) {
        noerrorShim();
        if (level!=0) return;
        // this one is tricky, as it can be GL_DEPTH_STENCIL with DEPTH+DEPTH_STENCIL extension of GL_STENCIL_INDEX8 with STENCIL extension
        // and having DEPTH+DEPTH_STENCIL extension doesn't grant STENCIL8 texture extension!
        if((tex || !texture) && (hardext.stenciltex || (hardext.depthtex && hardext.depthstencil))) {
            // depth texture supported, so are stencil one then!
            //check if texture needs to be re-created as true depth texture
            if(tex && !(tex->format==GL_STENCIL_INDEX8 || tex->format==GL_DEPTH_STENCIL)) {
                if(tex->format==GL_DEPTH_ATTACHMENT) {
                    //TODO: need to create a new texture, as the depth one is probably used
                    gl4es_glGenTextures(1, &texture);
                    realize_textures(0);
                    int oldactive = glstate->texture.active;
                    if(oldactive) gles_glActiveTexture(GL_TEXTURE0);
                    gltexture_t *bound = glstate->texture.bound[0/*glstate->texture.active*/][ENABLED_TEX2D];
                    GLuint oldtex = bound->glname;
                    int nwidth = tex->nwidth;
                    int nheight = tex->nheight;
                    tex = gl4es_getTexture(textarget, texture);
                    if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, tex->glname);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    tex->format = (hardext.stenciltex)?GL_STENCIL_INDEX8:GL_DEPTH_STENCIL;
                    tex->type = (hardext.stenciltex)?GL_UNSIGNED_BYTE:GL_UNSIGNED_INT_24_8;
                    gl4es_glTexImage2D(GL_TEXTURE_2D, 0, tex->format, nwidth, nheight, 0, tex->format, tex->type, NULL);
                    if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, oldtex);
                    if(oldactive) gles_glActiveTexture(GL_TEXTURE0+oldactive);
                } else {
                    tex->format = GL_STENCIL_INDEX8;
                    if(tex->type!=GL_UNSIGNED_BYTE && tex->type!=GL_UNSIGNED_SHORT && tex->type!=GL_FLOAT) tex->type = GL_UNSIGNED_BYTE;
                    tex->fpe_format = FPE_TEX_DEPTH;
                    realize_textures(0);
                    int oldactive = glstate->texture.active;
                    if(oldactive) gles_glActiveTexture(GL_TEXTURE0);
                    gltexture_t *bound = glstate->texture.bound[0/*glstate->texture.active*/][ENABLED_TEX2D];
                    GLuint oldtex = bound->glname;
                    if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, tex->glname);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    gles_glTexImage2D(GL_TEXTURE_2D, 0, tex->format, tex->nwidth, tex->nheight, 0, tex->format, tex->type, NULL);
                    if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, oldtex);
                    if(oldactive) gles_glActiveTexture(GL_TEXTURE0+oldactive);
                }
            }
            // bind the stencil texture...
            gles_glFramebufferTexture2D(ntarget, attachment, GL_TEXTURE_2D, texture, 0);
        } else {
            // let's create a renderbuffer and attach it instead of the (presumably) stencil texture
            if(tex && !tex->renderstencil) {
                gl4es_glGenRenderbuffers(1, &tex->renderstencil);
                gl4es_glBindRenderbuffer(GL_RENDERBUFFER, tex->renderstencil);
                gl4es_glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, twidth, theight);
                gl4es_glBindRenderbuffer(GL_RENDERBUFFER, 0);
            }
            gl4es_glFramebufferRenderbuffer(ntarget, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, tex?tex->renderstencil:0);
        }
        errorGL();
        ReadDraw_Pop(target);
        return;
    }
    if(attachment==GL_DEPTH_STENCIL_ATTACHMENT /*&& hardext.depthtex==0*/) {
        noerrorShim();
        if (level!=0) return;
        // let's create a renderbuffer and attach it instead of the (presumably) depth texture
        if(hardext.depthstencil /*&& !(hardext.depthtex && tex)*/) {
            if(hardext.depthtex && (tex || !texture)) {
                if(tex && tex->format!=GL_DEPTH_STENCIL) {
                    tex->format = GL_DEPTH_STENCIL;
                    tex->type = GL_UNSIGNED_INT_24_8;
                    tex->fpe_format = FPE_TEX_DEPTH; // add depth_stencil?
                    int oldactive = glstate->texture.active;
                    if(oldactive) gles_glActiveTexture(GL_TEXTURE0);
                    gltexture_t *bound = glstate->texture.bound[0/*glstate->texture.active*/][ENABLED_TEX2D];
                    GLuint oldtex = bound->glname;
                    if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, tex->glname);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    gles_glTexImage2D(GL_TEXTURE_2D, 0, tex->format, tex->nwidth, tex->nheight, 0, tex->format, tex->type, NULL);
                    if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, oldtex);
                    if(oldactive) gles_glActiveTexture(GL_TEXTURE0+oldactive);
                }
                gles_glFramebufferTexture2D(ntarget, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
                gles_glFramebufferTexture2D(ntarget, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
            } else {
                if(tex && !tex->renderdepth) {
                    gl4es_glGenRenderbuffers(1, &tex->renderdepth);
                    gl4es_glBindRenderbuffer(GL_RENDERBUFFER, tex->renderdepth);
                    gl4es_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, twidth, theight);
                    gl4es_glBindRenderbuffer(GL_RENDERBUFFER, 0);
                }
                errorGL();
                gl4es_glFramebufferRenderbuffer(ntarget, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, tex?tex->renderdepth:0);
                gl4es_glFramebufferRenderbuffer(ntarget, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, tex?tex->renderdepth:0);
            }
        } else {
            if(hardext.depthtex && (tex || !texture)) {
                // depth texture supported!
                //check if texture needs to be re-created ad true depth texture
                if(tex && tex->format!=GL_DEPTH_COMPONENT) {
                    tex->format = GL_DEPTH_COMPONENT;
                    if(tex->type!=GL_UNSIGNED_INT && tex->type!=GL_UNSIGNED_SHORT && tex->type!=GL_FLOAT) tex->type = (hardext.depth24)?GL_UNSIGNED_INT:GL_UNSIGNED_SHORT;
                    tex->fpe_format = FPE_TEX_DEPTH;
                    int oldactive = glstate->texture.active;
                    if(oldactive) gles_glActiveTexture(GL_TEXTURE0);
                    gltexture_t *bound = glstate->texture.bound[0/*glstate->texture.active*/][ENABLED_TEX2D];
                    GLuint oldtex = bound->glname;
                    if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, tex->glname);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    gles_glTexImage2D(GL_TEXTURE_2D, 0, tex->format, tex->nwidth, tex->nheight, 0, tex->format, tex->type, NULL);
                    if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, oldtex);
                    if(oldactive) gles_glActiveTexture(GL_TEXTURE0+oldactive);
                }
                // bind the depth texture...
                gles_glFramebufferTexture2D(ntarget, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture, 0);
            } else {
                if(tex && !tex->renderdepth) {
                    gl4es_glGenRenderbuffers(1, &tex->renderdepth);
                    gl4es_glBindRenderbuffer(GL_RENDERBUFFER, tex->renderdepth);
                    gl4es_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, twidth, theight);
                    gl4es_glBindRenderbuffer(GL_RENDERBUFFER, 0);
                }
                gl4es_glFramebufferRenderbuffer(ntarget, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, tex?tex->renderdepth:0);
            }
            if(tex && !tex->renderstencil) {
                gl4es_glGenRenderbuffers(1, &tex->renderstencil);
                gl4es_glBindRenderbuffer(GL_RENDERBUFFER, tex->renderstencil);
                gl4es_glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, twidth, theight);
                gl4es_glBindRenderbuffer(GL_RENDERBUFFER, 0);
            }
            errorGL();
            gl4es_glFramebufferRenderbuffer(ntarget, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, tex?tex->renderstencil:0);

        }
        ReadDraw_Pop(target);
        return;
    }

    twidth = twidth >> level; if(twidth<1) twidth=1;
    theight = theight >> level; if(theight<1) theight=1;
    
    if (level!=0) {
        //bind a scrap texture, we don't want level != 0 binding on GLES
        if(!scrap_tex)
            gl4es_glGenTextures(1, &scrap_tex);
        if ((scrap_width!=twidth) || (scrap_height!=theight)) {
                scrap_width = twidth;
                scrap_height = theight;
                gltexture_t *bound = glstate->texture.bound[glstate->texture.active][ENABLED_TEX2D];
                GLuint oldtex = bound->glname;
                if (oldtex!=scrap_tex) gles_glBindTexture(GL_TEXTURE_2D, scrap_tex);
                gles_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, scrap_width, scrap_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                if (oldtex!=scrap_tex) gles_glBindTexture(GL_TEXTURE_2D, oldtex);
        }
        texture = scrap_tex;
    }
    
    errorGL();
    GLenum realtarget = GL_TEXTURE_2D;
    if(textarget>=GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget<GL_TEXTURE_CUBE_MAP_POSITIVE_X+6)
        realtarget = textarget;
    gles_glFramebufferTexture2D(ntarget, attachment, realtarget, texture, 0);
    DBG(CheckGLError(1);)
    ReadDraw_Pop(target);
}

void APIENTRY_GL4ES gl4es_glFramebufferTexture1D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,    GLint level) {
    gl4es_glFramebufferTexture2D(target, attachment, textarget, texture, level);
}
void APIENTRY_GL4ES gl4es_glFramebufferTexture3D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,    GLint level, GLint layer) {
    (void)layer;
    gl4es_glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

void APIENTRY_GL4ES gl4es_glGenRenderbuffers(GLsizei n, GLuint *renderbuffers) {
    DBG(printf("glGenRenderbuffers(%i, %p)\n", n, renderbuffers);)
    LOAD_GLES2_OR_OES(glGenRenderbuffers);
    errorGL();
    gles_glGenRenderbuffers(n, renderbuffers);
    // track the renderbuffers...
    int ret;
    khint_t k;
    khash_t(renderbufferlist_t) *list = glstate->fbo.renderbufferlist;
    for(int i=0; i<n; ++i) {
        k = kh_put(renderbufferlist_t, list, renderbuffers[i], &ret);
        glrenderbuffer_t *rend = kh_value(list, k) = malloc(sizeof(glrenderbuffer_t));
        memset(rend, 0, sizeof(glrenderbuffer_t));
        rend->renderbuffer = renderbuffers[i];
    }
}

void APIENTRY_GL4ES gl4es_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    DBG(printf("glFramebufferRenderbuffer(%s, %s, %s, %u)\n", PrintEnum(target), PrintEnum(attachment), PrintEnum(renderbuffertarget), renderbuffer);)
    LOAD_GLES2_OR_OES(glFramebufferRenderbuffer);
    LOAD_GLES2_OR_OES(glGetFramebufferAttachmentParameteriv);
    LOAD_GLES(glGetError);

    glframebuffer_t *fb = get_framebuffer(target);
    if(!fb) {
        errorShim(GL_INVALID_ENUM);
        return;
    }

    if( !(attachment>=GL_COLOR_ATTACHMENT0 && attachment<(GL_COLOR_ATTACHMENT0+hardext.maxcolorattach))
     && attachment!=GL_DEPTH_ATTACHMENT 
     && attachment!=GL_STENCIL_ATTACHMENT 
     && attachment!=GL_DEPTH_STENCIL_ATTACHMENT) {
         errorShim(GL_INVALID_ENUM);
         return;
     }
    
    // get renderbuffer
    glrenderbuffer_t *rend = find_renderbuffer(renderbuffer);
    if(!rend /*|| !rend->renderbuffer*/) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }

    if (attachment >= GL_COLOR_ATTACHMENT0 && (attachment < (GL_COLOR_ATTACHMENT0+hardext.maxcolorattach)) && globals4es.fboforcetex) {
        if(rend->renderbuffer) {
            // drop the renderbuffer attachement and create a texture instead...
            int oldactive = glstate->texture.active;
            if(oldactive) gl4es_glActiveTexture(GL_TEXTURE0);
            gltexture_t *bound = glstate->texture.bound[0][ENABLED_TEX2D];
            // put back through gl4es_glBindTexture, so the application's name (glname is the GLES one: it differs for a
            // name the application did not get from glGenTextures, and then binding it picked another texture for unit 0)
            GLuint oldtex = bound->texture;
            // get size of renderbuffer
            GLenum format = rend->format;
            GLint width = rend->width;
            GLint height = rend->height;
            // create a texture if needed
            if(!rend->secondarytexture) {
                GLuint newtex;
                gl4es_glGenTextures(1, &newtex);
                gl4es_glBindTexture(GL_TEXTURE_2D, newtex);
                gl4es_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl4es_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                gl4es_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                gl4es_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                gl4es_glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
                gl4es_glBindTexture(GL_TEXTURE_2D, oldtex);
                rend->secondarytexture = newtex;
            }
            gl4es_glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, rend->secondarytexture, 0);
            // end of cleanup
            if(oldactive) gl4es_glActiveTexture(GL_TEXTURE0+oldactive);
        } else {
            // renderbuffer is 0, unbind the texture attachement...
            gl4es_glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, 0, 0);
        }
        return;
    }

    if (attachment==GL_DEPTH_STENCIL_ATTACHMENT) {
        // doesn't seems to be supported "as-is" on GLES on PVR
        gl4es_glFramebufferRenderbuffer(target, GL_DEPTH_ATTACHMENT, renderbuffertarget, renderbuffer);
        gl4es_glFramebufferRenderbuffer(target, GL_STENCIL_ATTACHMENT, renderbuffertarget, renderbuffer);
        return;
    }

    
    //TODO: handle target=READBUFFER or DRAWBUFFER...
    if (attachment==GL_STENCIL_ATTACHMENT) {
        if(rend && rend->secondarybuffer)
            renderbuffer = rend->secondarybuffer;
    }

    fb->width  = rend->width;
    fb->height = rend->height;
    if ((GetAttachmentType(fb, attachment) == GL_RENDERBUFFER) && (GetAttachment(fb, attachment)==renderbuffer))
    {
        // no need to reattach
        noerrorShim();
        return;
    }

    SetAttachment(fb, attachment, GL_RENDERBUFFER, renderbuffer, 0);

    /*if ((glstate->fbo.current_fb!=0) && (renderbuffer==0)) {
        //Hack, avoid unbind a renderbuffer on a framebuffer...
        // TODO, avoid binding an already binded RB
        noerrorShim();
        return;
    }*/ // Let it do it now
    
    GLenum ntarget = ReadDraw_Push(target);

    errorGL();
    gles_glFramebufferRenderbuffer(ntarget, attachment, renderbuffertarget, renderbuffer);
    DBG(CheckGLError(1);)
    ReadDraw_Pop(target);
}

void APIENTRY_GL4ES gl4es_glDeleteRenderbuffers(GLsizei n, GLuint *renderbuffers) {
    DBG(printf("glDeleteRenderbuffer(%d, %p)\n", n, renderbuffers);)
    LOAD_GLES2_OR_OES(glDeleteRenderbuffers);
    
    // check if we delete a depthstencil
    if (glstate->fbo.renderbufferlist)
        for (int i=0; i<n; i++) {
            khint_t k;
            glrenderbuffer_t *rend;
            for (int i = 0; i < n; i++) {
                GLuint t = renderbuffers[i];
                if(t) {
                    k = kh_get(renderbufferlist_t, glstate->fbo.renderbufferlist, t);
                    if (k != kh_end(glstate->fbo.renderbufferlist)) {
                        rend = kh_value(glstate->fbo.renderbufferlist, k);
                        if(glstate->fbo.current_rb == rend)
                            glstate->fbo.current_rb = glstate->fbo.default_rb;
                        if(rend->secondarybuffer)
                            gles_glDeleteRenderbuffers(1, &rend->secondarybuffer);
                        if(rend->secondarytexture)
                            gl4es_glDeleteTextures(1, &rend->secondarytexture);
                        free(rend);
                        kh_del(renderbufferlist_t, glstate->fbo.renderbufferlist, k);
                    }
                }
            }
        }

    errorGL();
    gles_glDeleteRenderbuffers(n, renderbuffers);
}

void APIENTRY_GL4ES gl4es_glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    DBG(printf("glRenderbufferStorage(%s, %s, %i, %i)\n", PrintEnum(target), PrintEnum(internalformat), width, height);)
    LOAD_GLES2_OR_OES(glRenderbufferStorage);
    LOAD_GLES2_OR_OES(glGenRenderbuffers);
    LOAD_GLES2_OR_OES(glBindRenderbuffer);

    glrenderbuffer_t *rend = glstate->fbo.current_rb;
    if(!rend->renderbuffer) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    
    errorGL();
    width = (hardext.npot>0 && !globals4es.potframebuffer)?width:npot(width);
    height = (hardext.npot>0 && !globals4es.potframebuffer)?height:npot(height);
    int use_secondarybuffer = 0;
    int use_secondarytexture = 0;
    GLenum format = internalformat;
#ifndef NOEGL
    // hybrid context (ES 3 backend): GLADIATOR picks the sized ES format by backend capability (GL_DEPTH_COMPONENT32 ->
    // DEPTH_COMPONENT32F, GL_RGBA -> RGBA8, RGB16F -> RGBA16F...) instead of the ES 2 substitutions below (DEPTH16, RGBA4)
    if (gl4es_hybrid())
        internalformat = gl4es_gl31_rb_format(internalformat);
#endif
    // check if internal format is GL_DEPTH_STENCIL_EXT
    if (internalformat == GL_DEPTH_STENCIL)
        internalformat = GL_DEPTH24_STENCIL8;
    // in that case, create first a STENCIL one then a DEPTH one....
    if ((internalformat == GL_DEPTH24_STENCIL8 && (hardext.depthstencil==0 || ((hardext.vendor&VEND_IMGTEC)==VEND_IMGTEC)))) {
        internalformat = (hardext.depth24)?GL_DEPTH_COMPONENT24:GL_DEPTH_COMPONENT16;
        // create a stencil buffer if needed
        if(!rend->secondarybuffer) {
            gles_glGenRenderbuffers(1, &rend->secondarybuffer);
        }
        use_secondarybuffer = 1;
    }
    else if (internalformat == GL_DEPTH_COMPONENT || internalformat == GL_DEPTH_COMPONENT32)    // Not much is supported on GLES...
        internalformat = GL_DEPTH_COMPONENT16;
    else if (internalformat == GL_RGB8 && hardext.rgba8==0)
        internalformat = GL_RGB565_OES;
    else if (internalformat == GL_RGBA8 && hardext.rgba8==0)
        internalformat = GL_RGBA4_OES;
    else if (internalformat == GL_RGB5)
        internalformat = GL_RGB565_OES;
    else if (internalformat == GL_R3_G3_B2)
        internalformat = GL_RGB565_OES;
    else if (internalformat == GL_RGB4)
        internalformat = GL_RGBA4_OES;
    else if (internalformat == GL_RGBA) {
        if(hardext.rgba8==0)
            internalformat = GL_RGBA8;
        else
            internalformat = GL_RGBA4_OES;
    }

    if(rend->secondarybuffer) {
        if(use_secondarybuffer) {
            GLuint current_rb = glstate->fbo.current_rb->renderbuffer;
            gles_glBindRenderbuffer(GL_RENDERBUFFER, rend->secondarybuffer);
            gles_glRenderbufferStorage(target, GL_STENCIL_INDEX8, width, height);
            gles_glBindRenderbuffer(GL_RENDERBUFFER, current_rb);
        } else {
            LOAD_GLES2_OR_OES(glDeleteRenderbuffers);
            gles_glDeleteRenderbuffers(1, &rend->secondarybuffer);
            rend->secondarybuffer = 0;
        }
    }

    if(rend->secondarytexture) {
        // should check if texture is still needed?
        gltexture_t *tex = gl4es_getTexture(GL_TEXTURE_2D, rend->secondarytexture);
        LOAD_GLES(glActiveTexture);
        LOAD_GLES(glBindTexture);
        LOAD_GLES(glTexImage2D);
        int oldactive = glstate->texture.active;
        if(oldactive) gles_glActiveTexture(GL_TEXTURE0);
        gltexture_t *bound = glstate->texture.bound[0/*glstate->texture.active*/][ENABLED_TEX2D];
        GLuint oldtex = bound->glname;
        if (oldtex!=rend->secondarytexture) gles_glBindTexture(GL_TEXTURE_2D, rend->secondarytexture);
        tex->nwidth = tex->width = width;
        tex->nheight = tex->height = height;
        gles_glTexImage2D(GL_TEXTURE_2D, 0, tex->format, tex->nwidth, tex->nheight, 0, tex->format, tex->type, NULL);
        if (oldtex!=tex->glname) gles_glBindTexture(GL_TEXTURE_2D, oldtex);
        if(oldactive) gles_glActiveTexture(GL_TEXTURE0+oldactive);
    }

    rend->width  = width;
    rend->height = height;
    rend->format = format;
    rend->actual = internalformat;

    gles_glRenderbufferStorage(target, internalformat, width, height);
    DBG(CheckGLError(1);)
}

void APIENTRY_GL4ES gl4es_glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height) {    //STUB
    gl4es_glRenderbufferStorage(target, internalformat, width, height);
}

void APIENTRY_GL4ES gl4es_glBindRenderbuffer(GLenum target, GLuint renderbuffer) {
    DBG(printf("glBindRenderbuffer(%s, %u), binded Fbo=%u\n", PrintEnum(target), renderbuffer, glstate->fbo.current_fb->id);)
    LOAD_GLES2_OR_OES(glBindRenderbuffer);
    
    GLuint current = glstate->fbo.current_rb->renderbuffer;
    if(current==renderbuffer) {
        noerrorShim();
        return;
    }
     glrenderbuffer_t * rend = find_renderbuffer(renderbuffer);
    if(!rend || !rend->renderbuffer) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    glstate->fbo.current_rb = rend;
    
    errorGL();
    gles_glBindRenderbuffer(target, renderbuffer);
}

GLboolean APIENTRY_GL4ES gl4es_glIsRenderbuffer(GLuint renderbuffer) {
    DBG(printf("glIsRenderbuffer(%u)\n", renderbuffer);)
    noerrorShim();
    return((find_renderbuffer(renderbuffer)!=NULL)?GL_TRUE:GL_FALSE);
}

void APIENTRY_GL4ES gl4es_glGenerateMipmap(GLenum target) {
    if (gl4es_hybrid_tex_target(target)) { gl4es_hybrid_GenerateMipmap(target); noerrorShim(); return; }
    if (gl4es_hybrid_tex_is_hybrid(target)) { gl4es_hybrid_GenerateMipmap2D(target); noerrorShim(); return; }   // storage kept by GLADIATOR
    DBG(printf("glGenerateMipmap(%s)\n", PrintEnum(target));)
    LOAD_GLES2_OR_OES(glGenerateMipmap);
    
    const GLuint rtarget = map_tex_target(target);
    realize_bound(glstate->texture.active, target);
    gltexture_t *bound = gl4es_getCurrentTexture(target);
    if(globals4es.forcenpot && hardext.npot==1) {
        if(bound->npot) {
            noerrorShim();
            return; // no need to generate mipmap, mipmap is disabled here
        }
    }

    errorGL();
    if(globals4es.automipmap != 3) {
        gles_glGenerateMipmap(rtarget);
        bound->mipmap_auto = 1;
        /*if(bound->sampler.min_filer != bound->actual.min_filter)  // mainly for S3TC textures...
            gl4es_glTexParameteri(target, GL_TEXTURE_MIN_FILTER, bound->sampler.min_filer);*/
    }
}

void APIENTRY_GL4ES gl4es_glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname, GLint *params) {
    DBG(printf("glGetFramebufferAttachmentParameteriv(%s, %s, %s, %p)\n", PrintEnum(target), PrintEnum(attachment), PrintEnum(pname), params);)
    LOAD_GLES2_OR_OES(glGetFramebufferAttachmentParameteriv);

    glframebuffer_t *fb = get_framebuffer(target);
    if(!fb) {
        errorShim(GL_INVALID_ENUM);
        return;
    }

    if( !(attachment>=GL_COLOR_ATTACHMENT0 && attachment<(GL_COLOR_ATTACHMENT0+hardext.maxcolorattach))
     && attachment!=GL_DEPTH_ATTACHMENT 
     && attachment!=GL_STENCIL_ATTACHMENT 
     && attachment!=GL_DEPTH_STENCIL_ATTACHMENT) {
         errorShim(GL_INVALID_ENUM);
         return;
    }

    if(pname==GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) {
        noerrorShim();
        *params = GetAttachment(fb, attachment);
        return;
    }

    if(pname==GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE) {
        noerrorShim();
        *params = GetAttachmentType(fb, attachment);
        if(*params!=0 && *params!=GL_RENDERBUFFER)
            *params = GL_TEXTURE;
        return;
    }
    
    if(pname==GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL) {
        GLenum tmp = GetAttachmentType(fb, attachment);
        if(tmp!=0 && tmp!=GL_RENDERBUFFER) {
            noerrorShim();
            *params = GetAttachmentLevel(fb, attachment);
        } else {
            errorShim(GL_INVALID_ENUM);
        }
        return;
    }
    if(pname==GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE) {
        GLenum tmp = GetAttachmentType(fb, attachment);
        if(tmp!=0 && tmp!=GL_RENDERBUFFER) {
            noerrorShim();
            *params = (tmp>=GL_TEXTURE_CUBE_MAP_POSITIVE_X && tmp<=GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)?tmp:0;
        } else {
            errorShim(GL_INVALID_ENUM);
        }
        return;        
    }
    // more stuff can be done
    /*
    if(pname==GL_FRAMEBUFFER_ATTACHMENT_LAYERED) {
        GLenum tmp = GetAttachmentType(fb, attachment);
        noerrorShim();
        *params = (tmp==GL_TEXTURE_CUBE_MAP || tmp==GL_TEXTURE_3D)?GL_TRUE:GL_FALSE;
        return;
    }
    */
    // hack to return DEPTH size
    if(attachment==GL_DEPTH_ATTACHMENT && pname==GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE) {
        if(hardext.depthtex==0) {
            errorGL();
            *params = GetAttachment(fb, attachment);
            //TODO: Check all this?!
            if (*params)
                *params = 16;   //Depth buffer is 16 on GLES. No check for 24 bits here...
            return;
        }
        // check if it's depth/stencil 16/8, in that case, lie by spoofing 24/8 or some FNA game will fail
        int depth, stencil;
        GLenum ntarget = ReadDraw_Push(target);
        gles_glGetFramebufferAttachmentParameteriv(ntarget, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencil);
        gles_glGetFramebufferAttachmentParameteriv(ntarget, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depth);
        errorGL();
        ReadDraw_Pop(target);
        if(depth==16 && stencil==8)
            depth = 24;
        *params = depth;
        return;
    }

    GLenum ntarget = ReadDraw_Push(target);
    
    errorGL();
    gles_glGetFramebufferAttachmentParameteriv(ntarget, attachment, pname, params);
    ReadDraw_Pop(target);
}

void APIENTRY_GL4ES gl4es_glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint * params) {
    DBG(printf("glGetRenderbufferParameteriv(%s, %s, %p)\n", PrintEnum(target), PrintEnum(pname), params);)
    LOAD_GLES2_OR_OES(glGetRenderbufferParameteriv);
    
    errorGL();
    gles_glGetRenderbufferParameteriv(target, pname, params);
}

void createMainFBO(int width, int height) {
    LOAD_GLES2_OR_OES(glGenFramebuffers);
    LOAD_GLES2_OR_OES(glBindFramebuffer);
    LOAD_GLES2_OR_OES(glFramebufferTexture2D);
    LOAD_GLES2_OR_OES(glCheckFramebufferStatus);
    LOAD_GLES2_OR_OES(glFramebufferRenderbuffer);
    LOAD_GLES2_OR_OES(glRenderbufferStorage);
    LOAD_GLES2_OR_OES(glGenRenderbuffers);
    LOAD_GLES2_OR_OES(glBindRenderbuffer);
    LOAD_GLES(glTexImage2D);
    LOAD_GLES(glGenTextures);
    LOAD_GLES(glBindTexture);
    LOAD_GLES(glActiveTexture);
    void gles_glTexParameteri(glTexParameteri_ARG_EXPAND); //LOAD_GLES(glTexParameteri);
    LOAD_GLES2(glClientActiveTexture);
    LOAD_GLES(glClear);

    // If there is already a Framebuffer created, let's delete it.... unless it's already the right size!
    int createIt = 1;
    if (glstate->fbo.mainfbo_fbo) {
        if (width==glstate->fbo.mainfbo_width && height==glstate->fbo.mainfbo_height)
            return;
        //lets adjust the FBO instead of adjusting it
        createIt = 0;
    }
    DBG(printf("LIBGL: Create FBO of %ix%i 32bits\n", width, height);)
    // switch to texture unit 0 if needed
    if (glstate->texture.active != 0)
        gles_glActiveTexture(GL_TEXTURE0);
    if (glstate->texture.client != 0 && gles_glClientActiveTexture)
        gles_glClientActiveTexture(GL_TEXTURE0);
        
    glstate->fbo.mainfbo_width = width;
    glstate->fbo.mainfbo_height = height;
    glstate->fbo.mainfbo_nwidth = width = hardext.npot>0?width:npot(width);
    glstate->fbo.mainfbo_nheight = height = hardext.npot>0?height:npot(height);

    // create the texture
    if(createIt)
        gles_glGenTextures(1, &glstate->fbo.mainfbo_tex);
    gles_glBindTexture(GL_TEXTURE_2D, glstate->fbo.mainfbo_tex);
    if(createIt) {
        gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gles_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    gles_glTexImage2D(GL_TEXTURE_2D, 0, globals4es.fbo_noalpha?GL_RGB:GL_RGBA, width, height,
                    0, globals4es.fbo_noalpha?GL_RGB:GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gles_glBindTexture(GL_TEXTURE_2D, 0);
    // create the render buffers
    if(createIt) {
        gles_glGenRenderbuffers(1, &glstate->fbo.mainfbo_dep);
        gles_glGenRenderbuffers(1, &glstate->fbo.mainfbo_ste);
    }
    gles_glBindRenderbuffer(GL_RENDERBUFFER, glstate->fbo.mainfbo_ste);
    gles_glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width, height);
    gles_glBindRenderbuffer(GL_RENDERBUFFER, glstate->fbo.mainfbo_dep);
    gles_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    gles_glBindRenderbuffer(GL_RENDERBUFFER, 0);
    // create a fbo
    if(createIt)
        gles_glGenFramebuffers(1, &glstate->fbo.mainfbo_fbo);
    gles_glBindFramebuffer(GL_FRAMEBUFFER, glstate->fbo.mainfbo_fbo);
    
    // re-attach, even if not creating the fbo...
    gles_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, glstate->fbo.mainfbo_ste);
    gles_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, glstate->fbo.mainfbo_dep);
    
    gles_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glstate->fbo.mainfbo_tex, 0);

    GLenum status = gles_glCheckFramebufferStatus(GL_FRAMEBUFFER);

    gles_glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Put everything back
    gles_glBindTexture(GL_TEXTURE_2D, glstate->texture.bound[0][ENABLED_TEX2D]->glname);
    if (glstate->texture.active != 0)
        gles_glActiveTexture(GL_TEXTURE0 + glstate->texture.active);
    if (glstate->texture.client != 0 && gles_glClientActiveTexture)
        gles_glClientActiveTexture(GL_TEXTURE0 + glstate->texture.client);
    GLuint current_rb = glstate->fbo.current_rb->renderbuffer;
    gles_glBindRenderbuffer(GL_RENDERBUFFER, current_rb);
    // Final check, and bind the fbo for future use
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        printf("LIBGL: Error while creating main fbo (0x%04X)\n", status);
        deleteMainFBO(glstate);
        gles_glBindFramebuffer(GL_FRAMEBUFFER, glstate->fbo.current_fb->id);
        
    } else {
        gles_glBindFramebuffer(GL_FRAMEBUFFER, (glstate->fbo.current_fb->id)?glstate->fbo.current_fb->id:glstate->fbo.mainfbo_fbo);
        // clear color, depth and stencil...
        if (glstate->fbo.current_fb->id==0)
            gles_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    
}

void blitMainFBO(int x, int y, int width, int height) {
    if (glstate->fbo.mainfbo_fbo==0)
        return;

    // blit the texture
    if(!width && !height) {
        gl4es_glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        gl4es_glClear(GL_COLOR_BUFFER_BIT);
    }

    GLint vp[4];
    memcpy(vp, &glstate->raster.viewport, sizeof(vp));
    gl4es_glViewport(0, 0, glstate->fbowidth, glstate->fboheight);
    float rx, ry;
    if(!width && !height) {
        width = glstate->fbo.mainfbo_width;
        height = glstate->fbo.mainfbo_height;
        rx = ry = 1.0f;
    } else {
        y = glstate->fboheight - (y+height);
        //y = - y;
        rx = (float)width/glstate->fbo.mainfbo_width;
        ry = (float)height/glstate->fbo.mainfbo_height;
    }
    gl4es_blitTexture(glstate->fbo.mainfbo_tex, 0.f, 0.f,
        glstate->fbo.mainfbo_width, glstate->fbo.mainfbo_height, 
        glstate->fbo.mainfbo_nwidth, glstate->fbo.mainfbo_nheight, 
        rx, ry,
        0, 0, x, y, BLIT_OPAQUE);
    gl4es_glViewport(vp[0], vp[1], vp[2], vp[3]);
}

void bindMainFBO() {
    LOAD_GLES2_OR_OES(glBindFramebuffer);
    LOAD_GLES2_OR_OES(glCheckFramebufferStatus);
    if (!glstate->fbo.mainfbo_fbo)
        return;
    if (glstate->fbo.current_fb->id==0) {
        gles_glBindFramebuffer(GL_FRAMEBUFFER, glstate->fbo.mainfbo_fbo);
        //gles_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    }
}

void unbindMainFBO() {
    LOAD_GLES2_OR_OES(glBindFramebuffer);
    if (!glstate->fbo.mainfbo_fbo)
        return;
    if (glstate->fbo.current_fb->id==0) {
        gles_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void deleteMainFBO(void *state) {
    LOAD_GLES2_OR_OES(glDeleteFramebuffers);
    LOAD_GLES2_OR_OES(glDeleteRenderbuffers);
    LOAD_GLES(glDeleteTextures);

    glstate_t *glstate = (glstate_t*)state;

    if (glstate->fbo.mainfbo_dep) {
        gles_glDeleteRenderbuffers(1, &glstate->fbo.mainfbo_dep);
        glstate->fbo.mainfbo_dep = 0;
    }
    if (glstate->fbo.mainfbo_ste) {
        gles_glDeleteRenderbuffers(1, &glstate->fbo.mainfbo_ste);
        glstate->fbo.mainfbo_ste = 0;
    }
    if (glstate->fbo.mainfbo_tex) {
        gles_glDeleteTextures(1, &glstate->fbo.mainfbo_tex);
        glstate->fbo.mainfbo_tex = 0;
    }
    if (glstate->fbo.mainfbo_fbo) {
        gles_glDeleteFramebuffers(1, &glstate->fbo.mainfbo_fbo);
        glstate->fbo.mainfbo_fbo = 0;
    }
    
    // all done...
}

void APIENTRY_GL4ES gl4es_glFramebufferTextureLayer(    GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
    gl4es_glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, texture,    level); // Force Texture2D, ignore layer (should track?)...
}

#ifndef NOX11
void gl4es_SwapBuffers_currentContext();    // defined in glx/glx.c
#endif
void APIENTRY_GL4ES gl4es_glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
    if (glstate->raster.bm_drawing) bitmap_flush();   // pending glBitmap pixels belong before this framebuffer access
    // mask will be ignored
    // filter will be taken only for ReadFBO has no Texture attached (so readpixel is used)
    DBG(printf("glBlitFramebuffer(%d, %d, %d, %d,  %d, %d, %d, %d,  0x%04X, %s) fbo_read=%d, fbo_draw=%d\n",
        srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, PrintEnum(filter), glstate->fbo.fbo_read->id, glstate->fbo.fbo_draw->id);)

    if((mask&GL_COLOR_BUFFER_BIT)==0)
        return; // cannot copy DEPTH or STENCIL data on GLES, only COLOR_BUFFER...

    if(glstate->fbo.fbo_read == glstate->fbo.fbo_draw && srcX0==dstX0 && srcX1==dstX1 && srcY0==dstY0 && srcY1==dstY1)
        return; // no need to try copying on itself
    
    if(dstX1==dstX0 || dstY1==dstY0)
        return; // nothing to draw
    if(srcX1==srcX0 || srcY1==srcY0)
        return; // nothing to draw

    GLuint texture = (glstate->fbo.fbo_read->id==0 && glstate->fbo.mainfbo_fbo)?glstate->fbo.mainfbo_tex:glstate->fbo.fbo_read->color[0];

    int created = (texture==0 || (glstate->fbo.fbo_read==glstate->fbo.fbo_draw));
    int oldtex = glstate->texture.active;
    DBG(printf("   blit: created=%d, texture=%u, oldtex=%d\n", created, texture, oldtex);)
    if (oldtex)
        gl4es_glActiveTexture(GL_TEXTURE0);
    float nwidth, nheight;
    if (created) {
        gltexture_t *old = glstate->texture.bound[ENABLED_TEX2D][0];
        gl4es_glGenTextures(1, &texture);
        gl4es_glBindTexture(GL_TEXTURE_2D, texture);
        gl4es_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl4es_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl4es_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (filter==GL_LINEAR)?GL_LINEAR:GL_NEAREST);
        gl4es_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (filter==GL_LINEAR)?GL_LINEAR:GL_NEAREST);
        gl4es_glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, srcX0, srcY0, srcX1-srcX0, srcY1-srcY0, 0);
        srcX1-=srcX0; srcX0=0.f;
        srcY1-=srcY0; srcY0=0.f;
        gl4es_glBindTexture(GL_TEXTURE_2D, old->texture);
    }
    GLenum glname = texture;
    if(texture==glstate->fbo.mainfbo_tex) {
        nwidth = glstate->fbo.mainfbo_nwidth;
        nheight = glstate->fbo.mainfbo_nheight;
    } else {
        gltexture_t *tex = gl4es_getTexture(GL_TEXTURE_2D, texture);
        if(tex) {
            nwidth = tex->nwidth;
            nheight = tex->nheight;
            glname = tex->glname;
            if(!created) {
                if((tex->actual.min_filter!=filter) || (tex->actual.mag_filter!=filter)) {
                    gltexture_t *old = glstate->texture.bound[ENABLED_TEX2D][0];
                    if(old->texture != glname)
                        gl4es_glBindTexture(GL_TEXTURE_2D, glname);
                    gl4es_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                    gl4es_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                    tex->actual.min_filter = tex->actual.mag_filter = filter;
                    if(old->texture != glname)
                        gl4es_glBindTexture(GL_TEXTURE_2D, old->texture);
                }
            }
        } else {
            // not good if here!
            nwidth = srcX1;
            nheight = srcY1;
        }
    }
    float srcW = srcX1-srcX0;
    float srcH = srcY1-srcY0;
    float zoomx = ((float)(dstX1-dstX0))/srcW;
    float zoomy = ((float)(dstY1-dstY0))/srcH;
    // get the width / height of write FBO
    int fbowidth = 0, fboheight = 0;
    int blitfullscreen = 0;
    if(glstate->fbo.fbo_draw->id==0/* && glstate->fbo.mainfbo_fbo*/) {
        if(globals4es.blitfb0/* || (globals4es.usefb && !globals4es.usefbo)*/)
            blitfullscreen = 1;
        else {
            fbowidth = glstate->fbo.mainfbo_width;
            fboheight = glstate->fbo.mainfbo_height;
            if((glstate->fbo.mainfbo_width==abs(dstX1-dstX0)) && (glstate->fbo.mainfbo_height==abs(dstY1-dstY0))) {
                blitfullscreen = 1;
            } else {
                if (gl4es_getMainFBSize) {
                    gl4es_getMainFBSize(&glstate->fbo.mainfbo_width, &glstate->fbo.mainfbo_height);
                    if((glstate->fbo.mainfbo_width==abs(dstX1-dstX0)) && (glstate->fbo.mainfbo_height==abs(dstY1-dstY0)))
                        blitfullscreen = 1;
                }
            }
        }
    } else {
        fbowidth  = glstate->fbo.fbo_draw->width;
        fboheight = glstate->fbo.fbo_draw->height;
    }
    GLint vp[4];
    memcpy(vp, &glstate->raster.viewport, sizeof(vp));
    gl4es_glViewport(0, 0, fbowidth, fboheight);
    gl4es_blitTexture(glname, srcX0, srcY0, srcW, srcH, nwidth, nheight, zoomx, zoomy, 0, 0, dstX0, dstY0, BLIT_OPAQUE);
    gl4es_glViewport(vp[0], vp[1], vp[2], vp[3]);
    if(created) {
        gl4es_glDeleteTextures(1, &texture);
    }
    if(oldtex)
        gl4es_glActiveTexture(GL_TEXTURE0+oldtex);

#ifndef NOX11
    if(blitfullscreen)  // hack, force a swapbuffer (help wine d3d show stuff on certain games)
        gl4es_SwapBuffers_currentContext();
#endif
}

GLuint gl4es_getCurrentFBO() {
  return (glstate->fbo.current_fb->id)?glstate->fbo.current_fb->id:glstate->fbo.mainfbo_fbo;
}

void gl4es_setCurrentFBO() {
  LOAD_GLES2_OR_OES(glBindFramebuffer);
  gles_glBindFramebuffer(GL_FRAMEBUFFER, (glstate->fbo.current_fb->id)?glstate->fbo.current_fb->id:glstate->fbo.mainfbo_fbo);
}

// DrawBuffers functions are faked unless GL_EXT_draw_buffers is supported
#ifndef NOEGL
extern int gl4es_backend_es3(void);                 // glx/gl31_bridge.c
extern unsigned gl4es_gl31_tex_target(unsigned name);   // glx/gl31_bridge.c: GLES target GLADIATOR gave a texture (2D if unknown)
#endif

/* ---- arbitrary draw buffer mapping on a GLES 3 backend ----
 * Desktop GL lets glDrawBuffers send fragment output i to any color attachment (glDrawBuffer(GL_COLOR_ATTACHMENT3): output
 * 0 -> attachment 3); GLES only allows output i -> COLOR_ATTACHMENTi or NONE ("Ith value does not match COLOR_ATTACHMENTi").
 * As GLADIATOR does for core contexts (gl31_fbo.c, rm_apply), the draws then go to a proxy FBO whose attachment i is the
 * image the application's FBO has at drawbuff[i], plus its depth/stencil. The proxy is bound as the GLES draw framebuffer
 * only around GL4ES's draws and clears (gl4es_fbo_draw_begin/end): everything else keeps seeing the application's FBO,
 * which is what GL4ES binds and edits. It is rebuilt from what GLES has attached when the FBO changes or is bound again. */
static int fb_identity_map(GLsizei n, const GLenum* bufs)
{
    for (GLsizei j=0; j<n; j++)
        if (bufs[j]!=GL_NONE && bufs[j]!=GL_COLOR_ATTACHMENT0+(GLenum)j)
            return 0;
    return 1;
}

static int fb_use_proxy(glframebuffer_t* fb, GLsizei n, const GLenum* bufs)
{
#ifndef NOEGL
    return fb && fb->id && hardext.drawbuffers && gl4es_backend_es3() && !fb_identity_map(n, bufs);
#else
    (void)fb; (void)n; (void)bufs;
    return 0;
#endif
}

#ifndef NOEGL
typedef void (APIENTRY_GLES *fbp_tex2d_t)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY_GLES *fbp_layer_t)(GLenum, GLenum, GLuint, GLint, GLint);
typedef void (APIENTRY_GLES *fbp_rb_t)(GLenum, GLenum, GLenum, GLuint);
typedef void (APIENTRY_GLES *fbp_query_t)(GLenum, GLenum, GLenum, GLint*);
typedef void (APIENTRY_GLES *fbp_bind_t)(GLenum, GLuint);
typedef void (APIENTRY_GLES *fbp_gen_t)(GLsizei, GLuint*);
typedef void (APIENTRY_GLES *fbp_db_t)(GLsizei, const GLenum*);
static struct { int loaded; fbp_tex2d_t tex2d; fbp_layer_t layer; fbp_rb_t rb; fbp_query_t query; fbp_bind_t bind; fbp_gen_t gen; fbp_db_t db; } fbp;
static int fbp_proxy_bound = 0;    // the proxy is the GLES draw framebuffer right now (between begin and end)

static int fbp_load(void)
{
    if (!fbp.loaded) {
        fbp.loaded = 1;
        fbp.tex2d = (fbp_tex2d_t)proc_address(gles, "glFramebufferTexture2D");
        fbp.layer = (fbp_layer_t)proc_address(gles, "glFramebufferTextureLayer");
        fbp.rb    = (fbp_rb_t)proc_address(gles, "glFramebufferRenderbuffer");
        fbp.query = (fbp_query_t)proc_address(gles, "glGetFramebufferAttachmentParameteriv");
        fbp.bind  = (fbp_bind_t)proc_address(gles, "glBindFramebuffer");
        fbp.gen   = (fbp_gen_t)proc_address(gles, "glGenFramebuffers");
        fbp.db    = (fbp_db_t)proc_address(gles, "glDrawBuffers");
    }
    return fbp.tex2d && fbp.layer && fbp.rb && fbp.query && fbp.bind && fbp.gen && fbp.db;
}

static GLint fbp_att(GLenum att, GLenum pname)
{
    GLint v = 0;
    fbp.query(GL_READ_FRAMEBUFFER, att, pname, &v);
    return v;
}

// attachment `dst` of the draw framebuffer (the proxy) = the image `src` has in the read framebuffer (the application's FBO)
static void fbp_copy(GLenum dst, GLenum src)
{
    GLint ty = fbp_att(src, 0x8CD0 /* GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE */);
    GLint name;
    if (ty == GL_NONE) { fbp.rb(GL_DRAW_FRAMEBUFFER, dst, GL_RENDERBUFFER, 0); return; }
    name = fbp_att(src, 0x8CD1 /* GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME */);
    if (ty == GL_RENDERBUFFER) { fbp.rb(GL_DRAW_FRAMEBUFFER, dst, GL_RENDERBUFFER, (GLuint)name); return; }
    {
        GLint level = fbp_att(src, 0x8CD2 /* TEXTURE_LEVEL */);
        GLint face  = fbp_att(src, 0x8CD3 /* TEXTURE_CUBE_MAP_FACE */);
        GLint layer = fbp_att(src, 0x8CD4 /* TEXTURE_LAYER */);
        GLenum t = gl4es_gl31_tex_target((GLuint)name);
        if (face) fbp.tex2d(GL_DRAW_FRAMEBUFFER, dst, (GLenum)face, (GLuint)name, level);
        else if (t == 0x806F /* 3D */ || t == 0x8C1A /* 2D_ARRAY */ || t == 0x9009 /* CUBE_MAP_ARRAY */ || t == 0x9102 /* 2D_MS_ARRAY */)
            fbp.layer(GL_DRAW_FRAMEBUFFER, dst, (GLuint)name, level, layer);
        else fbp.tex2d(GL_DRAW_FRAMEBUFFER, dst, t == 0x9100 ? 0x9100 /* 2D_MULTISAMPLE */ : GL_TEXTURE_2D, (GLuint)name, level);
    }
}

static void fbp_build(glframebuffer_t* fb)
{
    GLenum db[MAX_DRAW_BUFFERS];
    int n = fb->n_draw, j, nmax = hardext.maxcolorattach;
    if (nmax > MAX_DRAW_BUFFERS) nmax = MAX_DRAW_BUFFERS;
    if (!fb->proxy) fbp.gen(1, &fb->proxy);
    fbp.bind(GL_READ_FRAMEBUFFER, fb->id);
    fbp.bind(GL_DRAW_FRAMEBUFFER, fb->proxy);
    for (j = 0; j < nmax; j++) {
        if (j < n && fb->drawbuff[j] != GL_NONE) {
            fbp_copy(GL_COLOR_ATTACHMENT0 + j, fb->drawbuff[j]);
            db[j] = GL_COLOR_ATTACHMENT0 + j;
        } else {
            fbp.rb(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + j, GL_RENDERBUFFER, 0);
            if (j < n) db[j] = GL_NONE;
        }
    }
    fbp_copy(GL_DEPTH_ATTACHMENT, GL_DEPTH_ATTACHMENT);
    fbp_copy(GL_STENCIL_ATTACHMENT, GL_STENCIL_ATTACHMENT);
    fbp.db(n, db);
    fb->proxy_dirty = 0;
    // GL4ES keeps one GLES binding (GL_FRAMEBUFFER) for its current framebuffer
    fbp.bind(GL_FRAMEBUFFER, glstate->fbo.current_fb->id ? glstate->fbo.current_fb->id : glstate->fbo.mainfbo_fbo);
}
#endif

void gl4es_fbo_draw_begin(void)
{
#ifndef NOEGL
    glframebuffer_t* fb = glstate->fbo.current_fb;
    if (!fb || !fb->remap || !fb->id || fb != glstate->fbo.fbo_draw || fbp_proxy_bound) return;
    if (!fbp_load()) return;
    if (fb->proxy_dirty || !fb->proxy) fbp_build(fb);
    fbp.bind(GL_DRAW_FRAMEBUFFER, fb->proxy);
    fbp_proxy_bound = 1;
#endif
}

void gl4es_fbo_draw_end(void)
{
#ifndef NOEGL
    if (!fbp_proxy_bound) return;
    fbp_proxy_bound = 0;
    fbp.bind(GL_DRAW_FRAMEBUFFER, glstate->fbo.current_fb->id ? glstate->fbo.current_fb->id : glstate->fbo.mainfbo_fbo);
#endif
}

// the proxy of a deleted framebuffer
static void gl4es_fbo_drop_proxy(glframebuffer_t* fb)
{
#ifndef NOEGL
    if (fb && fb->proxy) {
        LOAD_GLES2_OR_OES(glDeleteFramebuffers);
        gles_glDeleteFramebuffers(1, &fb->proxy);
        fb->proxy = 0;
    }
#else
    (void)fb;
#endif
}

// glDrawBuffers on `fb` (bound as the draw framebuffer when `bound`)
static void fb_set_drawbuffers(glframebuffer_t* fb, GLsizei n, const GLenum *bufs, int bound)
{
    fb->n_draw = n;
    memcpy(fb->drawbuff, bufs, n*sizeof(GLenum));
    if (fb_use_proxy(fb, n, bufs)) {
        fb->remap = 1;
        fb->proxy_dirty = 1;
    } else {
        fb->remap = 0;
        if (hardext.drawbuffers) {
            LOAD_GLES_EXT(glDrawBuffers);
            GLenum tb[MAX_DRAW_BUFFERS];
            if (!fb->id && n <= MAX_DRAW_BUFFERS) {
                // default framebuffer: GLES knows GL_BACK only (COLOR_ATTACHMENT0 when it is GL4ES's main FBO)
                for (GLsizei j=0; j<n; j++)
                    tb[j] = (bufs[j]==GL_NONE) ? GL_NONE : (glstate->fbo.mainfbo_fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK);
                bufs = tb;
            }
            if (bound) {
                ReadDraw_Push(GL_DRAW_FRAMEBUFFER);
                gles_glDrawBuffers(n, bufs);
                errorGL();
                ReadDraw_Pop(GL_DRAW_FRAMEBUFFER);
            } else {
                GLuint oldf = glstate->fbo.fbo_draw->id;
                gl4es_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb->id);
                gles_glDrawBuffers(n, bufs);
                errorGL();
                gl4es_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldf);
            }
        }
    }
}

void APIENTRY_GL4ES gl4es_glDrawBuffers(GLsizei n, const GLenum *bufs) {
    DBG(printf("glDrawBuffers(%d, %p) [0]=%s\n", n, bufs, n?PrintEnum(bufs[0]):"nil");)
    if(n<0 || n>hardext.maxdrawbuffers) {
        errorShim(GL_INVALID_VALUE);
        return;
    }
    fb_set_drawbuffers(glstate->fbo.fbo_draw, n, bufs, 1);
    noerrorShim();
}

// glDrawBuffer(mode) = glDrawBuffers(1, &mode) on a framebuffer object; on the default framebuffer GLES has GL_BACK only
void APIENTRY_GL4ES gl4es_glDrawBuffer(GLenum mode) {
    DBG(printf("glDrawBuffer(%s)\n", PrintEnum(mode));)
    FLUSH_BEGINEND;
    glframebuffer_t* fb = glstate->fbo.fbo_draw;
    if(fb->id) {
        if(mode!=GL_NONE && (mode<GL_COLOR_ATTACHMENT0 || mode>=GL_COLOR_ATTACHMENT0+(GLenum)hardext.maxcolorattach)) {
            errorShim((mode==GL_BACK || mode==GL_FRONT || mode==GL_FRONT_AND_BACK || mode==0x0402 || mode==0x0400 || mode==0x0406)
                      ? GL_INVALID_OPERATION : GL_INVALID_ENUM);
            return;
        }
    } else {
        switch(mode) {
            case GL_NONE: case GL_FRONT: case GL_BACK: case GL_FRONT_AND_BACK:
            case 0x0400 /* GL_FRONT_LEFT */: case 0x0402 /* GL_BACK_LEFT */: case 0x0406 /* GL_LEFT */:
                break;
            default:
                errorShim(GL_INVALID_ENUM);
                return;
        }
#ifndef NOEGL
        if(!gl4es_backend_es3()) {     // GLES 2: only recorded, as before
            fb->n_draw = 1;
            fb->drawbuff[0] = mode;
            noerrorShim();
            return;
        }
#endif
    }
    fb_set_drawbuffers(fb, 1, &mode, 1);
    noerrorShim();
}
AliasExport(void,glDrawBuffer,,(GLenum mode));
// glReadBuffer: ES 3 has it (per framebuffer object, like GL); ES 2 can only read COLOR_ATTACHMENT0 / the back buffer, so there it
// stays a no-op. The default framebuffer only knows GL_BACK in GLES (or COLOR_ATTACHMENT0 when it is GL4ES's main FBO, LIBGL_FB=2).
void APIENTRY_GL4ES gl4es_glReadBuffer(GLenum mode) {
    DBG(printf("glReadBuffer(%s)\n", PrintEnum(mode));)
    FLUSH_BEGINEND;
    int es3 = 0;
#ifndef NOEGL
    es3 = gl4es_backend_es3();
#endif
    GLenum m = mode;
    if(!glstate->fbo.fbo_read->id) {
        switch(mode) {
            case GL_NONE: break;
            case GL_FRONT: case GL_BACK: case 0x0406 /* GL_LEFT */: case 0x0400 /* GL_FRONT_LEFT */: case 0x0402 /* GL_BACK_LEFT */: case GL_FRONT_AND_BACK:
                m = glstate->fbo.mainfbo_fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK;
                break;
            default:
                errorShim(GL_INVALID_ENUM);
                return;
        }
    } else if(mode!=GL_NONE && (mode<GL_COLOR_ATTACHMENT0 || mode>=GL_COLOR_ATTACHMENT0+(GLenum)hardext.maxcolorattach)) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    if(!es3) {
        noerrorShim();
        return;
    }
    static void (APIENTRY_GLES *gles_glReadBuffer)(GLenum) = NULL;
    static int rb_loaded = 0;
    if(!rb_loaded) { rb_loaded = 1; gles_glReadBuffer = (void (APIENTRY_GLES *)(GLenum))proc_address(gles, "glReadBuffer"); }
    if(!gles_glReadBuffer) {
        noerrorShim();
        return;
    }
    readfboBegin();
    gles_glReadBuffer(m);
    errorGL();
    readfboEnd();
}
void APIENTRY_GL4ES gl4es_glNamedFramebufferDrawBuffers(GLuint framebuffer, GLsizei n, const GLenum *bufs) {
    if(n<0 || n>hardext.maxdrawbuffers) {
        errorShim(GL_INVALID_VALUE);
        return;
    }
    glframebuffer_t* fb = find_framebuffer(framebuffer);
    if(!fb) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    fb_set_drawbuffers(fb, n, bufs, fb==glstate->fbo.fbo_draw);
    noerrorShim();
}


// glClearBuffer* on a GLES 3.x backend: GLES has them, with the same meaning (drawbuffer = index into the draw
// buffers, values in the buffer's own type). The emulation below goes through glClear and a normalized clear color,
// which cannot clear an integer buffer. The application's draw framebuffer has to be the bound one.
static void* clearbuffer_native(const char* name) {
#ifndef NOEGL
    if(!gl4es_backend_es3() || !gles) return NULL;
    FLUSH_BEGINEND;
    if (glstate->raster.bm_drawing) bitmap_flush();
    if(glstate->fbo.current_fb != glstate->fbo.fbo_draw) {
        LOAD_GLES2_OR_OES(glBindFramebuffer);
        glstate->fbo.current_fb = glstate->fbo.fbo_draw;
        gles_glBindFramebuffer(GL_FRAMEBUFFER, glstate->fbo.fbo_draw->id ? glstate->fbo.fbo_draw->id : glstate->fbo.mainfbo_fbo);
    }
    return proc_address(gles, name);
#else
    (void)name;
    return NULL;
#endif
}

void APIENTRY_GL4ES gl4es_glClearBufferiv(GLenum buffer, GLint drawbuffer, const GLint * value) {
    noerrorShim();
    if (!glstate->list.compiling) {
        if (glstate->cond_render_skip) return;
        void (APIENTRY_GLES *f)(GLenum, GLint, const GLint*) = (void (APIENTRY_GLES *)(GLenum, GLint, const GLint*))clearbuffer_native("glClearBufferiv");
        if (f) { gl4es_fbo_draw_begin(); f(buffer, drawbuffer, value); gl4es_fbo_draw_end(); errorGL(); return; }
    }
    GLenum attch;
    switch(buffer) {
        case GL_COLOR:
            if(drawbuffer>glstate->fbo.fbo_draw->n_draw)
                return; // GL_NONE...
            attch = glstate->fbo.fbo_draw->drawbuff[buffer];
            if(!(attch>=GL_COLOR_ATTACHMENT0 && attch<GL_COLOR_ATTACHMENT0+hardext.maxdrawbuffers)) {
                errorShim(GL_INVALID_VALUE);
                return;
            } else {
                GLfloat oldclear[4];
                LOAD_GLES_EXT(glDrawBuffers);
                // select the buffer...
                if(hardext.drawbuffers)
                    gles_glDrawBuffers(1, (const GLenum *) &drawbuffer);
                gl4es_glGetFloatv(GL_COLOR_CLEAR_VALUE, oldclear);
                // how to convert the value? Most FB will be 8bits / component for now...
                gl4es_glClearColor(value[0]/127.0f, value[1]/127.0f, value[2]/127.0f, value[3]/127.0f);
                gl4es_glClear(GL_COLOR_BUFFER_BIT);
                gl4es_glClearColor(oldclear[0], oldclear[1], oldclear[2], oldclear[3]);
                // put back the draw buffers...
                if(hardext.drawbuffers)
                    gles_glDrawBuffers(glstate->fbo.fbo_draw->n_draw, glstate->fbo.fbo_draw->drawbuff);
                return;
            }
            break;
        case GL_STENCIL:
            if(drawbuffer==0) {
                GLint old;
                gl4es_glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &old);
                gl4es_glClearStencil(*value);
                gl4es_glClear(GL_STENCIL_BUFFER_BIT);
                gl4es_glClearStencil(old);
                return;
            } else {
                errorShim(GL_INVALID_ENUM);
                return;
            }
        default:
            errorShim(GL_INVALID_ENUM);
    }
    return;
}
void APIENTRY_GL4ES gl4es_glClearBufferuiv(GLenum buffer, GLint drawbuffer, const GLuint * value) {
    noerrorShim();
    if (!glstate->list.compiling) {
        if (glstate->cond_render_skip) return;
        void (APIENTRY_GLES *f)(GLenum, GLint, const GLuint*) = (void (APIENTRY_GLES *)(GLenum, GLint, const GLuint*))clearbuffer_native("glClearBufferuiv");
        if (f) { gl4es_fbo_draw_begin(); f(buffer, drawbuffer, value); gl4es_fbo_draw_end(); errorGL(); return; }
    }
    GLenum attch;
    switch(buffer) {
        case GL_COLOR:
            if(drawbuffer>glstate->fbo.fbo_draw->n_draw)
                return; // GL_NONE...
            attch = glstate->fbo.fbo_draw->drawbuff[buffer];
            if(!(attch>=GL_COLOR_ATTACHMENT0 && attch<GL_COLOR_ATTACHMENT0+hardext.maxdrawbuffers)) {
                errorShim(GL_INVALID_VALUE);
                return;
            } else {
                GLfloat oldclear[4];
                LOAD_GLES_EXT(glDrawBuffers);
                // select the buffer...
                if(hardext.drawbuffers)
                    gles_glDrawBuffers(1, (const GLenum *) &drawbuffer);
                gl4es_glGetFloatv(GL_COLOR_CLEAR_VALUE, oldclear);
                // how to convert the value? Most FB will be 8bits / component for now...
                gl4es_glClearColor(value[0]/255.0f, value[1]/255.0f, value[2]/255.0f, value[3]/255.0f);
                gl4es_glClear(GL_COLOR_BUFFER_BIT);
                gl4es_glClearColor(oldclear[0], oldclear[1], oldclear[2], oldclear[3]);
                // put back the draw buffers...
                if(hardext.drawbuffers)
                    gles_glDrawBuffers(glstate->fbo.fbo_draw->n_draw, glstate->fbo.fbo_draw->drawbuff);
                return;
            }
            break;
        default:
            errorShim(GL_INVALID_ENUM);
    }
    return;
}
void APIENTRY_GL4ES gl4es_glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat * value) {
    noerrorShim();
    if (!glstate->list.compiling) {
        if (glstate->cond_render_skip) return;
        void (APIENTRY_GLES *f)(GLenum, GLint, const GLfloat*) = (void (APIENTRY_GLES *)(GLenum, GLint, const GLfloat*))clearbuffer_native("glClearBufferfv");
        if (f) { gl4es_fbo_draw_begin(); f(buffer, drawbuffer, value); gl4es_fbo_draw_end(); errorGL(); return; }
    }
    GLenum attch;
    switch(buffer) {
        case GL_COLOR:
            if(drawbuffer>glstate->fbo.fbo_draw->n_draw)
                return; // GL_NONE...
            attch = glstate->fbo.fbo_draw->drawbuff[buffer];
            if(!(attch>=GL_COLOR_ATTACHMENT0 && attch<GL_COLOR_ATTACHMENT0+hardext.maxdrawbuffers)) {
                errorShim(GL_INVALID_VALUE);
                return;
            } else {
                GLfloat oldclear[4];
                LOAD_GLES_EXT(glDrawBuffers);
                // select the buffer...
                if(hardext.drawbuffers)
                    gles_glDrawBuffers(1, (const GLenum *) &drawbuffer);
                gl4es_glGetFloatv(GL_COLOR_CLEAR_VALUE, oldclear);
                // how to convert the value? Most FB will be 8bits / component for now...
                gl4es_glClearColor(value[0], value[1], value[2], value[3]);
                gl4es_glClear(GL_COLOR_BUFFER_BIT);
                gl4es_glClearColor(oldclear[0], oldclear[1], oldclear[2], oldclear[3]);
                // put back the draw buffers...
                if(hardext.drawbuffers)
                    gles_glDrawBuffers(glstate->fbo.fbo_draw->n_draw, glstate->fbo.fbo_draw->drawbuff);
                return;
            }
            break;
        case GL_DEPTH:
            if(drawbuffer==0) {
                GLint old;
                gl4es_glGetIntegerv(GL_DEPTH_CLEAR_VALUE, &old);
                gl4es_glClearDepthf(*value);
                gl4es_glClear(GL_DEPTH_BUFFER_BIT);
                gl4es_glClearDepthf(old);
                return;
            } else {
                errorShim(GL_INVALID_ENUM);
                return;
            }
        default:
            errorShim(GL_INVALID_ENUM);
    }
    return;
}
void APIENTRY_GL4ES gl4es_glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
    if (!glstate->list.compiling) {
        if (glstate->cond_render_skip) { noerrorShim(); return; }
        void (APIENTRY_GLES *f)(GLenum, GLint, GLfloat, GLint) = (void (APIENTRY_GLES *)(GLenum, GLint, GLfloat, GLint))clearbuffer_native("glClearBufferfi");
        if (f) { gl4es_fbo_draw_begin(); f(buffer, drawbuffer, depth, stencil); gl4es_fbo_draw_end(); errorGL(); return; }
    }
    if(buffer!=GL_DEPTH_STENCIL || drawbuffer!=0) {
        errorShim(GL_INVALID_ENUM);
        return;
    }
    GLint olds, oldd;
    gl4es_glGetIntegerv(GL_DEPTH_CLEAR_VALUE, &oldd);
    gl4es_glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &olds);
    gl4es_glClearDepthf(depth);
    gl4es_glClearStencil(stencil);
    gl4es_glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    gl4es_glClearDepthf(oldd);
    gl4es_glClearStencil(olds);
}

void APIENTRY_GL4ES gl4es_glClearNamedFramebufferiv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLint *value) {
    GLuint oldf = glstate->fbo.fbo_draw->id;
    GLenum target = (glstate->fbo.fbo_draw==glstate->fbo.fbo_read)?GL_FRAMEBUFFER:GL_DRAW_FRAMEBUFFER;
    gl4es_glBindFramebuffer(target, framebuffer);
    gl4es_glClearBufferiv(buffer, drawbuffer, value);
    gl4es_glBindFramebuffer(target, oldf);
}
void APIENTRY_GL4ES gl4es_glClearNamedFramebufferuiv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLuint *value) {
    GLuint oldf = glstate->fbo.fbo_draw->id;
    GLenum target = (glstate->fbo.fbo_draw==glstate->fbo.fbo_read)?GL_FRAMEBUFFER:GL_DRAW_FRAMEBUFFER;
    gl4es_glBindFramebuffer(target, framebuffer);
    gl4es_glClearBufferuiv(buffer, drawbuffer, value);
    gl4es_glBindFramebuffer(target, oldf);
}
void APIENTRY_GL4ES gl4es_glClearNamedFramebufferfv(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLfloat *value) {
    GLuint oldf = glstate->fbo.fbo_draw->id;
    GLenum target = (glstate->fbo.fbo_draw==glstate->fbo.fbo_read)?GL_FRAMEBUFFER:GL_DRAW_FRAMEBUFFER;
    gl4es_glBindFramebuffer(target, framebuffer);
    gl4es_glClearBufferfv(buffer, drawbuffer, value);
    gl4es_glBindFramebuffer(target, oldf);
}
void APIENTRY_GL4ES gl4es_glClearNamedFramebufferfi(GLuint framebuffer, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil) {
    GLuint oldf = glstate->fbo.fbo_draw->id;
    GLenum target = (glstate->fbo.fbo_draw==glstate->fbo.fbo_read)?GL_FRAMEBUFFER:GL_DRAW_FRAMEBUFFER;
    gl4es_glBindFramebuffer(target, framebuffer);
    gl4es_glClearBufferfi(buffer, drawbuffer, depth, stencil);
    gl4es_glBindFramebuffer(target, oldf);
}

void APIENTRY_GL4ES gl4es_glColorMaskIndexed(GLuint framebuffer, GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    GLuint oldf = glstate->fbo.fbo_draw->id;
    GLenum target = (glstate->fbo.fbo_draw==glstate->fbo.fbo_read)?GL_FRAMEBUFFER:GL_DRAW_FRAMEBUFFER;
    gl4es_glBindFramebuffer(target, framebuffer);
    gl4es_glColorMask(red, green, blue, alpha);
    gl4es_glBindFramebuffer(target, oldf);
}

void gl4es_saveCurrentFBO()
{
    // this, in fact, bind FBO to 0 if it wasn't
    GLuint framebuffer = (glstate->fbo.current_fb)?glstate->fbo.current_fb->id:0;
    if(framebuffer==0)
        framebuffer = glstate->fbo.mainfbo_fbo;
    if(framebuffer) {
        LOAD_GLES2_OR_OES(glBindFramebuffer);
        if(hardext.vendor&VEND_ARM)
            gl4es_glFinish(); //MALI seems to need a flush commandbefore unbinding the Framebuffer here
        gles_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void gl4es_restoreCurrentFBO()
{
    // Bind back current FBO if it wasn't 0
    GLuint framebuffer = (glstate->fbo.current_fb)?glstate->fbo.current_fb->id:0;
    if(framebuffer==0)
        framebuffer = glstate->fbo.mainfbo_fbo;
    if(framebuffer) {
        LOAD_GLES2_OR_OES(glBindFramebuffer);
        gles_glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    }
}

// direct wrapper

AliasExport(void,glGenFramebuffers,,(GLsizei n, GLuint *ids));
AliasExport(void,glDeleteFramebuffers,,(GLsizei n, GLuint *framebuffers));
AliasExport(GLboolean,glIsFramebuffer,,(GLuint framebuffer));
AliasExport(GLenum,glCheckFramebufferStatus,,(GLenum target));
AliasExport(void,glBindFramebuffer,,(GLenum target, GLuint framebuffer));
AliasExport(void,glFramebufferTexture1D,,(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,    GLint level));
AliasExport(void,glFramebufferTexture2D,,(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,    GLint level));
AliasExport(void,glFramebufferTexture3D,,(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,    GLint level, GLint layer));
AliasExport(void,glGenRenderbuffers,,(GLsizei n, GLuint *renderbuffers));
AliasExport(void,glFramebufferRenderbuffer,,(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer));
AliasExport(void,glDeleteRenderbuffers,,(GLsizei n, GLuint *renderbuffers));
AliasExport(void,glRenderbufferStorage,,(GLenum target, GLenum internalformat, GLsizei width, GLsizei height));
AliasExport(void,glBindRenderbuffer,,(GLenum target, GLuint renderbuffer));
AliasExport(GLboolean,glIsRenderbuffer,,(GLuint renderbuffer));
AliasExport(void,glGenerateMipmap,,(GLenum target));
AliasExport(void,glGetFramebufferAttachmentParameteriv,,(GLenum target, GLenum attachment, GLenum pname, GLint *params));
AliasExport(void,glGetRenderbufferParameteriv,,(GLenum target, GLenum pname, GLint * params));
AliasExport(void,glFramebufferTextureLayer,,(    GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer));
AliasExport(void,glBlitFramebuffer,,(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter));

// EXT direct wrapper
AliasExport(void,glGenFramebuffers,EXT,(GLsizei n, GLuint *ids));
AliasExport(void,glDeleteFramebuffers,EXT,(GLsizei n, GLuint *framebuffers));
AliasExport(GLboolean,glIsFramebuffer,EXT,(GLuint framebuffer));
AliasExport(GLenum,glCheckFramebufferStatus,EXT,(GLenum target));
AliasExport(void,glBindFramebuffer,EXT,(GLenum target, GLuint framebuffer));
AliasExport(void,glFramebufferTexture1D,EXT,(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,    GLint level));
AliasExport(void,glFramebufferTexture2D,EXT,(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,    GLint level));
AliasExport(void,glFramebufferTexture3D,EXT,(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,    GLint level, GLint layer));
AliasExport(void,glGenRenderbuffers,EXT,(GLsizei n, GLuint *renderbuffers));
AliasExport(void,glFramebufferRenderbuffer,EXT,(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer));
AliasExport(void,glDeleteRenderbuffers,EXT,(GLsizei n, GLuint *renderbuffers));
AliasExport(void,glRenderbufferStorage,EXT,(GLenum target, GLenum internalformat, GLsizei width, GLsizei height));
AliasExport(void,glBindRenderbuffer,EXT,(GLenum target, GLuint renderbuffer));
AliasExport(GLboolean,glIsRenderbuffer,EXT,(GLuint renderbuffer));
AliasExport(void,glGenerateMipmap,EXT,(GLenum target));
AliasExport(void,glGetFramebufferAttachmentParameteriv,EXT,(GLenum target, GLenum attachment, GLenum pname, GLint *params));
AliasExport(void,glGetRenderbufferParameteriv,EXT,(GLenum target, GLenum pname, GLint * params));
AliasExport(void,glFramebufferTextureLayer,EXT,(    GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer));
AliasExport(void,glBlitFramebuffer,EXT,(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter));

// Multisample stub
AliasExport(void,glRenderbufferStorageMultisample,,(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height));

// DrawBuffers
AliasExport(void,glDrawBuffers,,(GLsizei n, const GLenum *bufs));
AliasExport(void,glReadBuffer,,(GLenum mode));
AliasExport(void,glDrawBuffers,ARB,(GLsizei n, const GLenum *bufs));
AliasExport(void,glNamedFramebufferDrawBuffers,,(GLuint framebuffer, GLsizei n, const GLenum *bufs));
AliasExport(void,glNamedFramebufferDrawBuffers,EXT,(GLuint framebuffer, GLsizei n, const GLenum *bufs));

// ClearBuffer...
AliasExport(void,glClearBufferiv,,(GLenum buffer, GLint drawbuffer, const GLint * value));
AliasExport(void,glClearBufferuiv,,(GLenum buffer, GLint drawbuffer, const GLuint * value));
AliasExport(void,glClearBufferfv,,(GLenum buffer, GLint drawbuffer, const GLfloat * value));
AliasExport(void,glClearBufferfi,,(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil));

AliasExport(void,glClearNamedFramebufferiv,,(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLint *value));
AliasExport(void,glClearNamedFramebufferuiv,,(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLuint *value));
AliasExport(void,glClearNamedFramebufferfv,,(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLfloat *value));
AliasExport(void,glClearNamedFramebufferfi,,(GLuint framebuffer, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil));

AliasExport(void,glClearNamedFramebufferiv,EXT,(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLint *value));
AliasExport(void,glClearNamedFramebufferuiv,EXT,(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLuint *value));
AliasExport(void,glClearNamedFramebufferfv,EXT,(GLuint framebuffer, GLenum buffer, GLint drawbuffer, const GLfloat *value));
AliasExport(void,glClearNamedFramebufferfi,EXT,(GLuint framebuffer, GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil));
