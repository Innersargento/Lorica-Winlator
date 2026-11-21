#ifndef _GL4ES_HYBRID_H_
#define _GL4ES_HYBRID_H_
/* hybrid.c - GL4ES side of the hybrid-mode seam (see hybrid.c) */
#include "gles.h"

int  gl4es_hybrid(void);                        // the current context is hybrid (GL4ES + GLADIATOR)
int  gl4es_hybrid_tex_target(GLenum target);    // texture target GLADIATOR keeps in a hybrid context
int  gl4es_hybrid_tex_format(GLint ifmt);       // 2D internal format GLADIATOR keeps (sRGB, integer...)
int  gl4es_hybrid_tex_is_hybrid(GLenum target); // the 2D texture bound to `target` has GLADIATOR storage
int  gl4es_hybrid_read_format(GLenum format);   // glReadPixels format GLADIATOR reads (integer formats)

void gl4es_hybrid_BindTexture(GLenum target, GLuint texture);
void gl4es_hybrid_TexImage3D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint border,
                             GLenum format, GLenum type, const void* data);
void gl4es_hybrid_TexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d,
                                GLenum format, GLenum type, const void* data);
void gl4es_hybrid_TexStorage3D(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d);
void gl4es_hybrid_CompressedTexImage3D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d,
                                       GLint border, GLsizei size, const void* data);
void gl4es_hybrid_CompressedTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                                          GLsizei d, GLenum format, GLsizei size, const void* data);
void gl4es_hybrid_CopyTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLint rx, GLint ry,
                                    GLsizei w, GLsizei h);
void gl4es_hybrid_TexParameteri(GLenum target, GLenum pname, GLint param);
void gl4es_hybrid_TexParameterf(GLenum target, GLenum pname, GLfloat param);
void gl4es_hybrid_TexParameteriv(GLenum target, GLenum pname, const GLint* params);
void gl4es_hybrid_TexParameterfv(GLenum target, GLenum pname, const GLfloat* params);
void gl4es_hybrid_GetTexParameteriv(GLenum target, GLenum pname, GLint* params);
void gl4es_hybrid_GetTexParameterfv(GLenum target, GLenum pname, GLfloat* params);
void gl4es_hybrid_GetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params);
void gl4es_hybrid_GenerateMipmap(GLenum target);
void gl4es_hybrid_TexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border,
                             GLenum format, GLenum type, const void* data);
void gl4es_hybrid_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                GLenum format, GLenum type, const void* data);
void gl4es_hybrid_TexStorage2D(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h);
void gl4es_hybrid_GenerateMipmap2D(GLenum target);   // 2D / cube texture with GLADIATOR storage
void gl4es_hybrid_GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* data);
void gl4es_hybrid_DeleteTexture(GLuint glname);
void gl4es_hybrid_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* data);

void APIENTRY_GL4ES gl4es_glTexBuffer(GLenum target, GLenum ifmt, GLuint buffer);
void APIENTRY_GL4ES gl4es_glTexBufferRange(GLenum target, GLenum ifmt, GLuint buffer, GLintptr offset, GLsizeiptr size);
void APIENTRY_GL4ES gl4es_glTextureBuffer(GLuint texture, GLenum ifmt, GLuint buffer);
void APIENTRY_GL4ES gl4es_glTextureBufferRange(GLuint texture, GLenum ifmt, GLuint buffer, GLintptr offset, GLsizeiptr size);
void APIENTRY_GL4ES gl4es_glBeginTransformFeedback(GLenum mode);
void APIENTRY_GL4ES gl4es_glBeginConditionalRender(GLuint id, GLenum mode);
void APIENTRY_GL4ES gl4es_glEndConditionalRender(void);

// conditional rendering: the draw entry points return early while the query said "nothing passed"
#define CONDITIONAL_RENDER_SKIP() do { if (glstate->cond_render_skip) { noerrorShim(); return; } } while (0)

#endif // _GL4ES_HYBRID_H_
