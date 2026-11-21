#include "uniform.h"

#include "../glx/hardext.h"
#include "gl4es.h"
#include "glstate.h"
#include "loader.h"
#include "matvec.h"

//#define DEBUG
#ifdef DEBUG
#define DBG(a) a
#else
#define DBG(a)
#endif

// Uniform types. GL4ES started with the GLES 2 set; programs linked in a hybrid context (GLSL ES 3.x from GLADIATOR)
// also have unsigned ints, non-square matrices and every sampler / image type. A type missing here gets a cache size
// of 0 and its glUniform* calls never reach GLES.
int is_uniform_sampler(GLenum type) {
    switch (type) {
        case 0x8B5D: case 0x8B5E: case 0x8B5F: case 0x8B60:     // SAMPLER_1D, 2D, 3D, CUBE
        case 0x8B61: case 0x8B62: case 0x8B63: case 0x8B64:     // SAMPLER_1D_SHADOW, 2D_SHADOW, 2D_RECT, 2D_RECT_SHADOW
        case 0x8DC0: case 0x8DC1: case 0x8DC2: case 0x8DC3:     // SAMPLER_1D_ARRAY, 2D_ARRAY, BUFFER, 1D_ARRAY_SHADOW
        case 0x8DC4: case 0x8DC5:                               // SAMPLER_2D_ARRAY_SHADOW, CUBE_SHADOW
        case 0x8DC9: case 0x8DCA: case 0x8DCB: case 0x8DCC:     // INT_SAMPLER_1D, 2D, 3D, CUBE
        case 0x8DCD: case 0x8DCE: case 0x8DCF: case 0x8DD0:     // INT_SAMPLER_2D_RECT, 1D_ARRAY, 2D_ARRAY, BUFFER
        case 0x8DD1: case 0x8DD2: case 0x8DD3: case 0x8DD4:     // UNSIGNED_INT_SAMPLER_1D, 2D, 3D, CUBE
        case 0x8DD5: case 0x8DD6: case 0x8DD7: case 0x8DD8:     // UNSIGNED_INT_SAMPLER_2D_RECT, 1D_ARRAY, 2D_ARRAY, BUFFER
        case 0x8D66:                                            // SAMPLER_EXTERNAL_OES
        case 0x900C: case 0x900D: case 0x900E: case 0x900F:     // SAMPLER_CUBE_MAP_ARRAY (+SHADOW, INT, UNSIGNED_INT)
        case 0x9108: case 0x9109: case 0x910A:                  // SAMPLER_2D_MULTISAMPLE (+INT, UNSIGNED_INT)
        case 0x910B: case 0x910C: case 0x910D:                  // SAMPLER_2D_MULTISAMPLE_ARRAY (+INT, UNSIGNED_INT)
            return 1;
    }
    if (type >= 0x904C && type <= 0x906B) return 1;              // IMAGE_* / INT_IMAGE_* / UNSIGNED_INT_IMAGE_*
    return 0;
}

int is_uniform_uint(GLenum type) {
    switch (type) {
        case GL_UNSIGNED_INT:                                   // uint
        case 0x8DC6: case 0x8DC7: case 0x8DC8:                  // uvec2, uvec3, uvec4
            return 1;
    }
    return 0;
}

// columns x rows of a non-square matrix (0 if the type is not one)
static int uniform_nonsquare(GLenum type, int* cols, int* rows) {
    switch (type) {
        case 0x8B65: *cols = 2; *rows = 3; return 1;   // mat2x3
        case 0x8B66: *cols = 2; *rows = 4; return 1;   // mat2x4
        case 0x8B67: *cols = 3; *rows = 2; return 1;   // mat3x2
        case 0x8B68: *cols = 3; *rows = 4; return 1;   // mat3x4
        case 0x8B69: *cols = 4; *rows = 2; return 1;   // mat4x2
        case 0x8B6A: *cols = 4; *rows = 3; return 1;   // mat4x3
    }
    return 0;
}

int uniformsize(GLenum type) {
    #define GO(T, t, s) \
        case T: return sizeof(t)*s
    #define GOV(T, t) \
        GO(T, t, 1); \
        GO(T##_VEC2, t, 2); \
        GO(T##_VEC3, t, 3); \
        GO(T##_VEC4, t, 4)
    #define GOM(T, t) \
        GO(T##_MAT2, t, 2*2); \
        GO(T##_MAT3, t, 3*3); \
        GO(T##_MAT4, t, 4*4);
    int c, r;
    switch(type) {
    // GLES2 types
        GOV(GL_FLOAT, GLfloat);
        GOV(GL_INT, GLint);
        GOV(GL_BOOL, /*GLboolean*/GLint); //GLboolean is an unsigned char and is not suitable here
        GOM(GL_FLOAT, GLfloat);
    // GLES3 types
        GO(GL_UNSIGNED_INT, GLuint, 1);
        GO(0x8DC6, GLuint, 2);
        GO(0x8DC7, GLuint, 3);
        GO(0x8DC8, GLuint, 4);
    }
    if (uniform_nonsquare(type, &c, &r)) return sizeof(GLfloat)*c*r;
    if (is_uniform_sampler(type)) return sizeof(GLint);
    return sizeof(GLint)*4;     // unknown (atomic counter...): not settable, but keeps its own room in the cache
    #undef GOM
    #undef GOV
    #undef GO
}

int is_uniform_float(GLenum type) {
    switch (type) {
        case GL_FLOAT:
        case GL_FLOAT_VEC2:
        case GL_FLOAT_VEC3:
        case GL_FLOAT_VEC4:
        case GL_FLOAT_MAT2:
        case GL_FLOAT_MAT3:
        case GL_FLOAT_MAT4:
            return 1;
    }
    {
        int c, r;
        if (uniform_nonsquare(type, &c, &r)) return 1;
    }
    return 0;
}
int is_uniform_int(GLenum type) {
    switch (type) {
        case GL_INT:
        case GL_INT_VEC2:
        case GL_INT_VEC3:
        case GL_INT_VEC4:
        case GL_BOOL:
        case GL_BOOL_VEC2:
        case GL_BOOL_VEC3:
        case GL_BOOL_VEC4:
            return 1;
    }
    return is_uniform_sampler(type);
}
int is_uniform_matrix(GLenum type) {
    switch (type) {
        case GL_FLOAT_MAT2:
        case GL_FLOAT_MAT3:
        case GL_FLOAT_MAT4:
            return 1;
    }
    {
        int c, r;
        if (uniform_nonsquare(type, &c, &r)) return 1;
    }
    return 0;
}

int n_uniform(GLenum type) {
    // matrix type will return 0
    switch(type) {
        case GL_FLOAT:
        case GL_INT:
        case GL_BOOL:
        case GL_UNSIGNED_INT:
            return 1;
        case GL_FLOAT_VEC2:
        case GL_INT_VEC2:
        case GL_BOOL_VEC2:
        case 0x8DC6:    // uvec2
            return 2;
        case GL_FLOAT_VEC3:
        case GL_INT_VEC3:
        case GL_BOOL_VEC3:
        case 0x8DC7:    // uvec3
            return 3;
        case GL_FLOAT_VEC4:
        case GL_INT_VEC4:
        case GL_BOOL_VEC4:
        case 0x8DC8:    // uvec4
            return 4;
    }
    return is_uniform_sampler(type) ? 1 : 0;
}

// glGetUniform*: one element (the location's), from GL4ES's cache, converted to the type asked for
static uniform_t* get_uniform_value(GLuint program, GLint location, const void** src, int* n)
{
    FLUSH_BEGINEND;
    program_t *glprogram = NULL;
    khint_t k;
    if(program) {
        khash_t(programlist) *programs = glstate->glsl->programs;
        k = kh_get(programlist, programs, program);
        if (k != kh_end(programs))
            glprogram = kh_value(programs, k);
    }
    if(!glprogram) {
        errorShim(GL_INVALID_OPERATION);
        return NULL;
    }
    k = kh_get(uniformlist, glprogram->uniform, location);
    if(k==kh_end(glprogram->uniform)) {
        errorShim(GL_INVALID_VALUE);
        return NULL;
    }
    uniform_t *m = kh_value(glprogram->uniform, k);
    int size = uniformsize(m->type);
    if(size > m->cache_size) size = m->cache_size;
    *src = (const char*)glprogram->cache.cache + m->cache_offs;
    *n = size / 4;
    noerrorShim();
    return m;
}

void APIENTRY_GL4ES gl4es_glGetUniformfv(GLuint program, GLint location, GLfloat *params) {
    DBG(printf("glGetUniformfv(%d, %d, %p)\n", program, location, params);)
    const void* src; int n;
    uniform_t *m = get_uniform_value(program, location, &src, &n);
    if(!m) return;
    for (int i=0; i<n; i++)
        params[i] = is_uniform_float(m->type) ? ((const GLfloat*)src)[i]
                  : is_uniform_uint(m->type) ? (GLfloat)((const GLuint*)src)[i] : (GLfloat)((const GLint*)src)[i];
}
void APIENTRY_GL4ES gl4es_glGetUniformiv(GLuint program, GLint location, GLint *params) {
    DBG(printf("glGetUniformiv(%d, %d, %p)\n", program, location, params);)
    const void* src; int n;
    uniform_t *m = get_uniform_value(program, location, &src, &n);
    if(!m) return;
    for (int i=0; i<n; i++)
        params[i] = is_uniform_float(m->type) ? (GLint)((const GLfloat*)src)[i] : ((const GLint*)src)[i];
}
void APIENTRY_GL4ES gl4es_glGetUniformuiv(GLuint program, GLint location, GLuint *params) {
    DBG(printf("glGetUniformuiv(%d, %d, %p)\n", program, location, params);)
    const void* src; int n;
    uniform_t *m = get_uniform_value(program, location, &src, &n);
    if(!m) return;
    for (int i=0; i<n; i++)
        params[i] = is_uniform_float(m->type) ? (GLuint)((const GLfloat*)src)[i] : ((const GLuint*)src)[i];
}

void GoUniformfv(program_t *glprogram, GLint location, int size, int count, const GLfloat *value)
{
    DBG(printf("GoUniformfv(%p[%d], %d, %d, %d, %p) =>(%f...)\n", glprogram, glprogram->id, location, size, count, value, value[0]);)
    if(location==-1) {
        noerrorShim();
        return;
    }
    if(count<0) {
        errorShim(GL_INVALID_VALUE);
        return;
    }

    khint_t k;
    uniform_t *m;
    k = kh_get(uniformlist, glprogram->uniform, location);
    if (k==kh_end(glprogram->uniform)) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    m = kh_value(glprogram->uniform, k);
    if(size != n_uniform(m->type) || !is_uniform_float(m->type) || count>m->size) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    // ok, check the value in the cache
    int rsize = sizeof(GLfloat)*size*count;
    if (memcmp((char*)glprogram->cache.cache + m->cache_offs, value, rsize)==0) {
        noerrorShim();
        return; // nothing to do, same value already there
    }
    // update uniform
    memcpy((char*)glprogram->cache.cache + m->cache_offs, value, rsize);
    LOAD_GLES2(glUniform1fv);
    LOAD_GLES2(glUniform2fv);
    LOAD_GLES2(glUniform3fv);
    LOAD_GLES2(glUniform4fv);
    if(gles_glUniform1fv) {
        switch (size) {
            case 1: gles_glUniform1fv(m->id, count, value); break;
            case 2: gles_glUniform2fv(m->id, count, value); break;
            case 3: gles_glUniform3fv(m->id, count, value); break;
            case 4: gles_glUniform4fv(m->id, count, value); break;
        }
        errorGL();
    } else
        errorShim(GL_INVALID_OPERATION);    // no GLLS hardware
}
void GoUniformiv(program_t *glprogram, GLint location, int size, int count, const GLint *value)
{
    DBG(printf("GoUniformiv(%p[%d], %d, %d, %d, %p) =>(%d...)\n", glprogram, glprogram->id, location, size, count, value, value[0]);)
    if(location==-1) {
        noerrorShim();
        return;
    }
    if(count<0) {
        errorShim(GL_INVALID_VALUE);
        return;
    }

    khint_t k;
    uniform_t *m;
    k = kh_get(uniformlist, glprogram->uniform, location);
    if (k==kh_end(glprogram->uniform)) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    m = kh_value(glprogram->uniform, k);
    if(size != n_uniform(m->type) || !is_uniform_int(m->type)  || count>m->size) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    // ok, check the value in the cache
    int rsize = sizeof(GLint)*size*count;
    if (memcmp((char*)glprogram->cache.cache + m->cache_offs, value, rsize)==0) {
        noerrorShim();
        return; // nothing to do, same value already there
    }
    DBG(printf("Uniform updated, cache=%p(%d/%d), offset=%p, size=%d\n", glprogram->cache.cache, glprogram->cache.size, glprogram->cache.cap, (void*)m->cache_offs, rsize);)
    // update uniform
    memcpy((char*)glprogram->cache.cache + m->cache_offs, value, rsize);
    LOAD_GLES2(glUniform1iv);
    LOAD_GLES2(glUniform2iv);
    LOAD_GLES2(glUniform3iv);
    LOAD_GLES2(glUniform4iv);
    if(gles_glUniform1iv) {
        switch (size) {
            case 1: gles_glUniform1iv(m->id, count, value); break;
            case 2: gles_glUniform2iv(m->id, count, value); break;
            case 3: gles_glUniform3iv(m->id, count, value); break;
            case 4: gles_glUniform4iv(m->id, count, value); break;
        }
        errorGL();
    } else
        errorShim(GL_INVALID_OPERATION);    // no GLLS hardware
}

void APIENTRY_GL4ES gl4es_glUniform1f(GLint location, GLfloat v0) {
    DBG(printf("glUniform1f(%d, %f)\n", location, v0);)
    PUSH_IF_COMPILING(glUniform1f);
    GLuint program = glstate->glsl->program; 
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 1, 1, &v0);
}
void APIENTRY_GL4ES gl4es_glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
    DBG(printf("glUniform2f(%d, %f, %f)\n", location, v0, v1);)
    PUSH_IF_COMPILING(glUniform2f);
    GLfloat fl[2] = {v0, v1};
    GLuint program = glstate->glsl->program; 
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 2, 1, fl);
}
void APIENTRY_GL4ES gl4es_glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
    DBG(printf("glUniform3f(%d, %f, %f, %f)\n", location, v0, v1, v2);)
    PUSH_IF_COMPILING(glUniform3f);
    GLfloat fl[3] = {v0, v1, v2};
    GLuint program = glstate->glsl->program; 
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 3, 1, fl);
}
void APIENTRY_GL4ES gl4es_glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    DBG(printf("glUniform4f(%d, %f, %f, %f, %f)\n", location, v0, v1, v2, v3);)
    PUSH_IF_COMPILING(glUniform4f);
    GLfloat fl[4] = {v0, v1, v2, v3};
    GLuint program = glstate->glsl->program; 
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 4, 1, fl);
}
void APIENTRY_GL4ES gl4es_glUniform1i(GLint location, GLint v0) {
    DBG(printf("glUniform1i(%d, %d)\n", location, v0);)
    PUSH_IF_COMPILING(glUniform1i);
    GLuint program = glstate->glsl->program;
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 1, 1, &v0);
}
void APIENTRY_GL4ES gl4es_glUniform2i(GLint location, GLint v0, GLint v1) {
    DBG(printf("glUniform2i(%d, %d, %d)\n", location, v0, v1);)
    PUSH_IF_COMPILING(glUniform2i);
    GLint fl[2] = {v0, v1};
    GLuint program = glstate->glsl->program;
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 2, 1, fl);
}
void APIENTRY_GL4ES gl4es_glUniform3i(GLint location, GLint v0, GLint v1, GLint v2) {
    DBG(printf("glUniform3i(%d, %d, %d, %d)\n", location, v0, v1, v2);)
    PUSH_IF_COMPILING(glUniform3i);
    GLint fl[3] = {v0, v1, v2};
    GLuint program = glstate->glsl->program;
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 3, 1, fl);
}
void APIENTRY_GL4ES gl4es_glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3) {
    DBG(printf("glUniform4i(%d, %d, %d, %d, %d)\n", location, v0, v1, v2, v3);)
    PUSH_IF_COMPILING(glUniform4i);
    GLint fl[4] = {v0, v1, v2, v3};
    GLuint program = glstate->glsl->program;
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 4, 1, fl);
}
//TODO: the "v" variant and matrix variant cannot be pushed simply...
void APIENTRY_GL4ES gl4es_glUniform1fv(GLint location, GLsizei count, const GLfloat *value) {
    DBG(printf("glUniform1fv(%d, %d, %p) =>(%f)\n", location, count, value, value[0]);)
    PUSH_IF_COMPILING(glUniform1fv);
    GLuint program = glstate->glsl->program; 
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 1, count, value);
}
void APIENTRY_GL4ES gl4es_glUniform2fv(GLint location, GLsizei count, const GLfloat *value) {
    DBG(printf("glUniform2fv(%d, %d, %p) =>(%f %f)\n", location, count, value, value[0], value[1]);)
    PUSH_IF_COMPILING(glUniform2fv);
    GLuint program = glstate->glsl->program; 
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 2, count, value);
}
void APIENTRY_GL4ES gl4es_glUniform3fv(GLint location, GLsizei count, const GLfloat *value) {
    DBG(printf("glUniform3fv(%d, %d, %p) =>(%f %f, %f)\n", location, count, value, value[0], value[1], value[2]);)
    PUSH_IF_COMPILING(glUniform3fv);
    GLuint program = glstate->glsl->program; 
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 3, count, value);
}
void APIENTRY_GL4ES gl4es_glUniform4fv(GLint location, GLsizei count, const GLfloat *value) {
    DBG(printf("glUniform4fv(%d, %d, %p) =>(%f %f, %f, %f)\n", location, count, value, value[0], value[1], value[2], value[3]);)
    PUSH_IF_COMPILING(glUniform4fv);
    GLuint program = glstate->glsl->program; 
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 4, count, value);
}
void APIENTRY_GL4ES gl4es_glUniform1iv(GLint location, GLsizei count, const GLint *value) {
    PUSH_IF_COMPILING(glUniform1iv);
    GLuint program = glstate->glsl->program;
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 1, count, value);
}
void APIENTRY_GL4ES gl4es_glUniform2iv(GLint location, GLsizei count, const GLint *value) {
    PUSH_IF_COMPILING(glUniform2iv);
    GLuint program = glstate->glsl->program;
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 2, count, value);
}
void APIENTRY_GL4ES gl4es_glUniform3iv(GLint location, GLsizei count, const GLint *value) {
    PUSH_IF_COMPILING(glUniform3iv);
    GLuint program = glstate->glsl->program;
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 3, count, value);
}
void APIENTRY_GL4ES gl4es_glUniform4iv(GLint location, GLsizei count, const GLint *value) {
    PUSH_IF_COMPILING(glUniform4iv);
    GLuint program = glstate->glsl->program;
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 4, count, value);
}

void APIENTRY_GL4ES gl4es_glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    DBG(printf("glUniformMatrix2fv(%d, %d, %d, %p)\n", location, count, transpose, value);)
    PUSH_IF_COMPILING(glUniformMatrix2fv);
    GLuint program = glstate->glsl->program;
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformMatrix2fv(glprogram, location, count, transpose, value);
}

void APIENTRY_GL4ES gl4es_glProgramUniform1f(GLuint program, GLint location, GLfloat v0) {
    DBG(printf("glUniform1f(%d, %f)\n", location, v0);)
    PUSH_IF_COMPILING(glUniform1f);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 1, 1, &v0);
}
void APIENTRY_GL4ES gl4es_glProgramUniform2f(GLuint program, GLint location, GLfloat v0, GLfloat v1) {
    DBG(printf("glUniform2f(%d, %f, %f)\n", location, v0, v1);)
    PUSH_IF_COMPILING(glUniform2f);
    GLfloat fl[2] = {v0, v1};
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 2, 1, fl);
}
void APIENTRY_GL4ES gl4es_glProgramUniform3f(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
    DBG(printf("glUniform3f(%d, %f, %f, %f)\n", location, v0, v1, v2);)
    PUSH_IF_COMPILING(glUniform3f);
    GLfloat fl[3] = {v0, v1, v2};
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 3, 1, fl);
}
void APIENTRY_GL4ES gl4es_glProgramUniform4f(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    DBG(printf("glUniform4f(%d, %f, %f, %f, %f)\n", location, v0, v1, v2, v3);)
    PUSH_IF_COMPILING(glUniform4f);
    GLfloat fl[4] = {v0, v1, v2, v3};
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 4, 1, fl);
}
void APIENTRY_GL4ES gl4es_glProgramUniform1i(GLuint program, GLint location, GLint v0) {
    DBG(printf("glUniform1i(%d, %d)\n", location, v0);)
    PUSH_IF_COMPILING(glUniform1i);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 1, 1, &v0);
}
void APIENTRY_GL4ES gl4es_glProgramUniform2i(GLuint program, GLint location, GLint v0, GLint v1) {
    DBG(printf("glUniform2i(%d, %d, %d)\n", location, v0, v1);)
    PUSH_IF_COMPILING(glUniform2i);
    GLint fl[2] = {v0, v1};
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 2, 1, fl);
}
void APIENTRY_GL4ES gl4es_glProgramUniform3i(GLuint program, GLint location, GLint v0, GLint v1, GLint v2) {
    DBG(printf("glUniform3i(%d, %d, %d, %d)\n", location, v0, v1, v2);)
    PUSH_IF_COMPILING(glUniform3i);
    GLint fl[3] = {v0, v1, v2};
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 3, 1, fl);
}
void APIENTRY_GL4ES gl4es_glProgramUniform4i(GLuint program, GLint location, GLint v0, GLint v1, GLint v2, GLint v3) {
    DBG(printf("glUniform4i(%d, %d, %d, %d, %d)\n", location, v0, v1, v2, v3);)
    PUSH_IF_COMPILING(glUniform4i);
    GLint fl[4] = {v0, v1, v2, v3};
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 4, 1, fl);
}
//TODO: the "v" variant and matrix variant cannot be pushed simply...
void APIENTRY_GL4ES gl4es_glProgramUniform1fv(GLuint program, GLint location, GLsizei count, const GLfloat *value) {
    DBG(printf("glUniform1fv(%d, %d, %p) =>(%f)\n", location, count, value, value[0]);)
    PUSH_IF_COMPILING(glUniform1fv);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 1, count, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniform2fv(GLuint program, GLint location, GLsizei count, const GLfloat *value) {
    DBG(printf("glUniform2fv(%d, %d, %p) =>(%f %f)\n", location, count, value, value[0], value[1]);)
    PUSH_IF_COMPILING(glUniform2fv);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 2, count, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniform3fv(GLuint program, GLint location, GLsizei count, const GLfloat *value) {
    DBG(printf("glUniform3fv(%d, %d, %p) =>(%f %f, %f)\n", location, count, value, value[0], value[1], value[2]);)
    PUSH_IF_COMPILING(glUniform3fv);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 3, count, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniform4fv(GLuint program, GLint location, GLsizei count, const GLfloat *value) {
    DBG(printf("glUniform4fv(%d, %d, %p) =>(%f %f, %f, %f)\n", location, count, value, value[0], value[1], value[2], value[3]);)
    PUSH_IF_COMPILING(glUniform4fv);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformfv(glprogram, location, 4, count, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniform1iv(GLuint program, GLint location, GLsizei count, const GLint *value) {
    PUSH_IF_COMPILING(glUniform1iv);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 1, count, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniform2iv(GLuint program, GLint location, GLsizei count, const GLint *value) {
    PUSH_IF_COMPILING(glUniform2iv);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 2, count, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniform3iv(GLuint program, GLint location, GLsizei count, const GLint *value) {
    PUSH_IF_COMPILING(glUniform3iv);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 3, count, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniform4iv(GLuint program, GLint location, GLsizei count, const GLint *value) {
    PUSH_IF_COMPILING(glUniform4iv);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformiv(glprogram, location, 4, count, value);
}

void APIENTRY_GL4ES gl4es_glProgramUniformMatrix2fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    DBG(printf("glUniformMatrix2fv(%d, %d, %d, %p)\n", location, count, transpose, value);)
    PUSH_IF_COMPILING(glUniformMatrix2fv);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformMatrix2fv(glprogram, location, count, transpose, value);
}

void GoUniformMatrix2fv(program_t *glprogram, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    DBG(printf("GoUniformMatrix2fv(%p[%d], %d, %d, %d, %p)\n", glprogram, glprogram->id, location, count, transpose, value);)
    if(location==-1) {
        noerrorShim();
        return;
    }
    if(count<0) {
        errorShim(GL_INVALID_VALUE);
        return;
    }
    khint_t k;
    uniform_t *m;
    k = kh_get(uniformlist, glprogram->uniform, location);
    if (k==kh_end(glprogram->uniform)) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    m = kh_value(glprogram->uniform, k);
    if(m->type!=GL_FLOAT_MAT2  || count>m->size) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    // transpose if needed
    GLfloat *v = (GLfloat*)value;
    GLfloat tmp[4];
    if(transpose) {
        v = tmp;
        for (int n=0; n<count; n++)
            for (int i=0; i<2; i++)
                for (int j=0; j<2; j++)
                    v[n*2*2+i*2+j]=value[n*2*2+i+j*2];

    }
    // ok, check the value in the cache
    int rsize = sizeof(GLfloat)*2*2*count;
    if (memcmp((char*)glprogram->cache.cache + m->cache_offs, v, rsize)==0) {
        noerrorShim();
        return; // nothing to do, same value already there
    }
    // update uniform
    memcpy((char*)glprogram->cache.cache + m->cache_offs, v, rsize);
    LOAD_GLES2(glUniformMatrix2fv);
    if (gles_glUniformMatrix2fv) {
        gles_glUniformMatrix2fv(m->id, count, GL_FALSE, v);
        errorGL();
    } else
        errorShim(GL_INVALID_OPERATION);    // no GLSL hardware
}

void APIENTRY_GL4ES gl4es_glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    DBG(printf("glUniformMatrix3fv(%d, %d, %d, %p)\n", location, count, transpose, value);)
    PUSH_IF_COMPILING(glUniformMatrix3fv);
    GLuint program = glstate->glsl->program;
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformMatrix3fv(glprogram, location, count, transpose, value);
}

void APIENTRY_GL4ES gl4es_glProgramUniformMatrix3fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    DBG(printf("glUniformMatrix3fv(%d, %d, %d, %p)\n", location, count, transpose, value);)
    PUSH_IF_COMPILING(glUniformMatrix3fv);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformMatrix3fv(glprogram, location, count, transpose, value);
}

void GoUniformMatrix3fv(program_t *glprogram, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    DBG(printf("GoUniformMatrix3fv(%p[%d], %d, %d, %d, %p)\n", glprogram, glprogram->id, location, count, transpose, value);)
    if(location==-1) {
        noerrorShim();
        return;
    }
    if(count<0) {
        errorShim(GL_INVALID_VALUE);
        return;
    }
    khint_t k;
    uniform_t *m;
    k = kh_get(uniformlist, glprogram->uniform, location);
    if (k==kh_end(glprogram->uniform)) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    m = kh_value(glprogram->uniform, k);
    if(m->type!=GL_FLOAT_MAT3  || count>m->size) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    // transpose if needed
    GLfloat *v = (GLfloat*)value;
    GLfloat tmp[9];
    if(transpose) {
        v = tmp;
        for (int n=0; n<count; n++)
            for (int i=0; i<3; i++)
                for (int j=0; j<3; j++)
                    v[n*3*3+i*3+j]=value[n*3*3+i+j*3];

    }
    // ok, check the value in the cache
    int rsize = sizeof(GLfloat)*3*3*count;
    if (memcmp((char*)glprogram->cache.cache + m->cache_offs, v, rsize)==0) {
        noerrorShim();
        return; // nothing to do, same value already there
    }
    // update uniform
    memcpy((char*)glprogram->cache.cache + m->cache_offs, v, rsize);
    LOAD_GLES2(glUniformMatrix3fv);
    if (gles_glUniformMatrix3fv) {
        gles_glUniformMatrix3fv(m->id, count, GL_FALSE, v);
        errorGL();
    } else
        errorShim(GL_INVALID_OPERATION);    // no GLSL hardware
}
void APIENTRY_GL4ES gl4es_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    DBG(printf("glUniformMatrix4fv(%d, %d, %d, %p) p=>(%f, %f, %f, %f, %f...)\n", location, count, transpose, value, value[0], value[1], value[2], value[3], value[4]);)
    PUSH_IF_COMPILING(glUniformMatrix4fv);
    GLuint program = glstate->glsl->program;
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformMatrix4fv(glprogram, location, count, transpose, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniformMatrix4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    DBG(printf("glUniformMatrix4fv(%d, %d, %d, %p) p=>(%f, %f, %f, %f, %f...)\n", location, count, transpose, value, value[0], value[1], value[2], value[3], value[4]);)
    PUSH_IF_COMPILING(glUniformMatrix4fv);
    CHECK_PROGRAM(void, program);
    APPLY_PROGRAM(program, glprogram);
    GoUniformMatrix4fv(glprogram, location, count, transpose, value);
}

void GoUniformMatrix4fv(program_t *glprogram, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    DBG(printf("GoUniformMatrix4fv(%p[%d], %d, %d, %d, %p) p=>(%f, %f, %f, %f, %f...)\n", glprogram, glprogram->id, location, count, transpose, value, value[0], value[1], value[2], value[3], value[4]);)
    if(location==-1) {
        noerrorShim();
        return;
    }
    if(count<0) {
        errorShim(GL_INVALID_VALUE);
        return;
    }
    khint_t k;
    uniform_t *m;
    k = kh_get(uniformlist, glprogram->uniform, location);
    if (k==kh_end(glprogram->uniform)) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    m = kh_value(glprogram->uniform, k);
    if(m->type!=GL_FLOAT_MAT4  || count>m->size) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    // transpose if needed
    GLfloat *v = (GLfloat*)value;
    GLfloat tmp[16];
    if(transpose) {
        v = tmp;
        for (int n=0; n<count; n++)
            matrix_transpose(value+n*4*4, v+n*4*4);

    }
    // ok, check the value in the cache
    int rsize = sizeof(GLfloat)*4*4*count;
    if (memcmp((char*)glprogram->cache.cache + m->cache_offs, v, rsize)==0) {
        noerrorShim();
        return; // nothing to do, same value already there
    }
    // update uniform
    memcpy((char*)glprogram->cache.cache + m->cache_offs, v, rsize);
    LOAD_GLES2(glUniformMatrix4fv);
    if (gles_glUniformMatrix4fv) {
        gles_glUniformMatrix4fv(m->id, count, GL_FALSE, v);
        errorGL();
    } else {
        //printf("No GLES2 function\n");
        errorShim(GL_INVALID_OPERATION);    // no GLSL hardware
    }
}

int GetUniformi(program_t *glprogram, GLint location)
{
    if(location==-1) {
        noerrorShim();
        return 0;
    }

    khint_t k;
    uniform_t *m;
    k = kh_get(uniformlist, glprogram->uniform, location);
    if (k==kh_end(glprogram->uniform)) {
        return 0;
    }
    m = kh_value(glprogram->uniform, k);

    // ok, grab the value in the cache
    GLint ret;
    memcpy(&ret, (char*)glprogram->cache.cache + m->cache_offs, sizeof(GLint));
    return ret;
}

const char* GetUniformName(program_t *glprogram, GLint location)
{
    if(location==-1) {
        noerrorShim();
        return 0;
    }

    khint_t k;
    uniform_t *m;
    k = kh_get(uniformlist, glprogram->uniform, location);
    if (k==kh_end(glprogram->uniform)) {
        return 0;
    }
    m = kh_value(glprogram->uniform, k);

    // ok, grab the value in the cache
    return m->name;
}

// ---- GLSL ES 3.x uniform types (hybrid contexts): unsigned ints and non-square matrices ----
// The program is GL4ES's (bound lazily, uniforms cached per program), so these go through GL4ES as well: GLADIATOR's
// glUniform*ui would reach whatever program GLES has bound at that moment.
typedef void (APIENTRY_GLES * glUniform1uiv_PTR)(GLint location, GLsizei count, const GLuint *value);
typedef void (APIENTRY_GLES * glUniform2uiv_PTR)(GLint location, GLsizei count, const GLuint *value);
typedef void (APIENTRY_GLES * glUniform3uiv_PTR)(GLint location, GLsizei count, const GLuint *value);
typedef void (APIENTRY_GLES * glUniform4uiv_PTR)(GLint location, GLsizei count, const GLuint *value);
typedef void (APIENTRY_GLES * glUniformMatrix2x3fv_PTR)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
typedef void (APIENTRY_GLES * glUniformMatrix3x2fv_PTR)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
typedef void (APIENTRY_GLES * glUniformMatrix2x4fv_PTR)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
typedef void (APIENTRY_GLES * glUniformMatrix4x2fv_PTR)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
typedef void (APIENTRY_GLES * glUniformMatrix3x4fv_PTR)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
typedef void (APIENTRY_GLES * glUniformMatrix4x3fv_PTR)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

static uniform_t* go_uniform_find(program_t *glprogram, GLint location, GLsizei count)
{
    if(location==-1) {
        noerrorShim();
        return NULL;
    }
    if(count<0) {
        errorShim(GL_INVALID_VALUE);
        return NULL;
    }
    khint_t k = kh_get(uniformlist, glprogram->uniform, location);
    if (k==kh_end(glprogram->uniform)) {
        errorShim(GL_INVALID_OPERATION);
        return NULL;
    }
    return kh_value(glprogram->uniform, k);
}

void GoUniformuiv(program_t *glprogram, GLint location, int size, int count, const GLuint *value)
{
    DBG(printf("GoUniformuiv(%p[%d], %d, %d, %d, %p)\n", glprogram, glprogram->id, location, size, count, value);)
    uniform_t *m = go_uniform_find(glprogram, location, count);
    if(!m) return;
    // GLSL ES 3.00 lets glUniform*ui set bool uniforms too
    int is_bool = (m->type==GL_BOOL || m->type==GL_BOOL_VEC2 || m->type==GL_BOOL_VEC3 || m->type==GL_BOOL_VEC4);
    if(size != n_uniform(m->type) || !(is_uniform_uint(m->type) || is_bool) || count>m->size) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    int rsize = sizeof(GLuint)*size*count;
    if (memcmp((char*)glprogram->cache.cache + m->cache_offs, value, rsize)==0) {
        noerrorShim();
        return;
    }
    memcpy((char*)glprogram->cache.cache + m->cache_offs, value, rsize);
    LOAD_GLES2(glUniform1uiv);
    LOAD_GLES2(glUniform2uiv);
    LOAD_GLES2(glUniform3uiv);
    LOAD_GLES2(glUniform4uiv);
    if(gles_glUniform1uiv) {
        switch (size) {
            case 1: gles_glUniform1uiv(m->id, count, value); break;
            case 2: gles_glUniform2uiv(m->id, count, value); break;
            case 3: gles_glUniform3uiv(m->id, count, value); break;
            case 4: gles_glUniform4uiv(m->id, count, value); break;
        }
        errorGL();
    } else
        errorShim(GL_INVALID_OPERATION);    // GLES 2 backend: no unsigned int uniforms
}

// non-square matrices, cached column-major like the square ones (transpose done here)
void GoUniformMatrixNxMfv(program_t *glprogram, GLenum mtype, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    int cols = 0, rows = 0;
    uniform_t *m = go_uniform_find(glprogram, location, count);
    if(!m) return;
    if(m->type!=mtype || !uniform_nonsquare(mtype, &cols, &rows) || count>m->size) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    int nf = cols*rows*count;
    GLfloat *tmp = NULL;
    const GLfloat *v = value;
    if(transpose) {
        tmp = (GLfloat*)malloc(sizeof(GLfloat)*nf);
        if(!tmp) { errorShim(GL_OUT_OF_MEMORY); return; }
        for (int n=0; n<count; n++)
            for (int c=0; c<cols; c++)
                for (int r=0; r<rows; r++)
                    tmp[n*cols*rows + c*rows + r] = value[n*cols*rows + r*cols + c];
        v = tmp;
    }
    int rsize = sizeof(GLfloat)*nf;
    if (memcmp((char*)glprogram->cache.cache + m->cache_offs, v, rsize)==0) {
        free(tmp);
        noerrorShim();
        return;
    }
    memcpy((char*)glprogram->cache.cache + m->cache_offs, v, rsize);
    LOAD_GLES2(glUniformMatrix2x3fv);
    LOAD_GLES2(glUniformMatrix3x2fv);
    LOAD_GLES2(glUniformMatrix2x4fv);
    LOAD_GLES2(glUniformMatrix4x2fv);
    LOAD_GLES2(glUniformMatrix3x4fv);
    LOAD_GLES2(glUniformMatrix4x3fv);
    glUniformMatrix2x3fv_PTR f = NULL;     // all six have the same signature
    switch (mtype) {
        case 0x8B65: f = gles_glUniformMatrix2x3fv; break;
        case 0x8B66: f = (glUniformMatrix2x3fv_PTR)gles_glUniformMatrix2x4fv; break;
        case 0x8B67: f = (glUniformMatrix2x3fv_PTR)gles_glUniformMatrix3x2fv; break;
        case 0x8B68: f = (glUniformMatrix2x3fv_PTR)gles_glUniformMatrix3x4fv; break;
        case 0x8B69: f = (glUniformMatrix2x3fv_PTR)gles_glUniformMatrix4x2fv; break;
        case 0x8B6A: f = (glUniformMatrix2x3fv_PTR)gles_glUniformMatrix4x3fv; break;
    }
    if(f) {
        f(m->id, count, GL_FALSE, v);
        errorGL();
    } else
        errorShim(GL_INVALID_OPERATION);
    free(tmp);
}

#define U_PROG_CURRENT() \
    GLuint program = glstate->glsl->program; \
    CHECK_PROGRAM(void, program); \
    APPLY_PROGRAM(program, glprogram)
#define U_PROG_GIVEN() \
    FLUSH_BEGINEND; \
    CHECK_PROGRAM(void, program); \
    APPLY_PROGRAM(program, glprogram)

void APIENTRY_GL4ES gl4es_glUniform1ui(GLint location, GLuint v0) {
    FLUSH_BEGINEND; U_PROG_CURRENT(); GoUniformuiv(glprogram, location, 1, 1, &v0);
}
void APIENTRY_GL4ES gl4es_glUniform2ui(GLint location, GLuint v0, GLuint v1) {
    GLuint v[2] = {v0, v1};
    FLUSH_BEGINEND; U_PROG_CURRENT(); GoUniformuiv(glprogram, location, 2, 1, v);
}
void APIENTRY_GL4ES gl4es_glUniform3ui(GLint location, GLuint v0, GLuint v1, GLuint v2) {
    GLuint v[3] = {v0, v1, v2};
    FLUSH_BEGINEND; U_PROG_CURRENT(); GoUniformuiv(glprogram, location, 3, 1, v);
}
void APIENTRY_GL4ES gl4es_glUniform4ui(GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3) {
    GLuint v[4] = {v0, v1, v2, v3};
    FLUSH_BEGINEND; U_PROG_CURRENT(); GoUniformuiv(glprogram, location, 4, 1, v);
}
void APIENTRY_GL4ES gl4es_glUniform1uiv(GLint location, GLsizei count, const GLuint *value) {
    FLUSH_BEGINEND; U_PROG_CURRENT(); GoUniformuiv(glprogram, location, 1, count, value);
}
void APIENTRY_GL4ES gl4es_glUniform2uiv(GLint location, GLsizei count, const GLuint *value) {
    FLUSH_BEGINEND; U_PROG_CURRENT(); GoUniformuiv(glprogram, location, 2, count, value);
}
void APIENTRY_GL4ES gl4es_glUniform3uiv(GLint location, GLsizei count, const GLuint *value) {
    FLUSH_BEGINEND; U_PROG_CURRENT(); GoUniformuiv(glprogram, location, 3, count, value);
}
void APIENTRY_GL4ES gl4es_glUniform4uiv(GLint location, GLsizei count, const GLuint *value) {
    FLUSH_BEGINEND; U_PROG_CURRENT(); GoUniformuiv(glprogram, location, 4, count, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniform1ui(GLuint program, GLint location, GLuint v0) {
    U_PROG_GIVEN(); GoUniformuiv(glprogram, location, 1, 1, &v0);
}
void APIENTRY_GL4ES gl4es_glProgramUniform2ui(GLuint program, GLint location, GLuint v0, GLuint v1) {
    GLuint v[2] = {v0, v1};
    U_PROG_GIVEN(); GoUniformuiv(glprogram, location, 2, 1, v);
}
void APIENTRY_GL4ES gl4es_glProgramUniform3ui(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2) {
    GLuint v[3] = {v0, v1, v2};
    U_PROG_GIVEN(); GoUniformuiv(glprogram, location, 3, 1, v);
}
void APIENTRY_GL4ES gl4es_glProgramUniform4ui(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3) {
    GLuint v[4] = {v0, v1, v2, v3};
    U_PROG_GIVEN(); GoUniformuiv(glprogram, location, 4, 1, v);
}
void APIENTRY_GL4ES gl4es_glProgramUniform1uiv(GLuint program, GLint location, GLsizei count, const GLuint *value) {
    U_PROG_GIVEN(); GoUniformuiv(glprogram, location, 1, count, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniform2uiv(GLuint program, GLint location, GLsizei count, const GLuint *value) {
    U_PROG_GIVEN(); GoUniformuiv(glprogram, location, 2, count, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniform3uiv(GLuint program, GLint location, GLsizei count, const GLuint *value) {
    U_PROG_GIVEN(); GoUniformuiv(glprogram, location, 3, count, value);
}
void APIENTRY_GL4ES gl4es_glProgramUniform4uiv(GLuint program, GLint location, GLsizei count, const GLuint *value) {
    U_PROG_GIVEN(); GoUniformuiv(glprogram, location, 4, count, value);
}

#define U_NSQ(NAME, TYPE) \
void APIENTRY_GL4ES gl4es_glUniformMatrix##NAME##fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) { \
    FLUSH_BEGINEND; U_PROG_CURRENT(); GoUniformMatrixNxMfv(glprogram, TYPE, location, count, transpose, value); \
} \
void APIENTRY_GL4ES gl4es_glProgramUniformMatrix##NAME##fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) { \
    U_PROG_GIVEN(); GoUniformMatrixNxMfv(glprogram, TYPE, location, count, transpose, value); \
}
U_NSQ(2x3, 0x8B65)
U_NSQ(2x4, 0x8B66)
U_NSQ(3x2, 0x8B67)
U_NSQ(3x4, 0x8B68)
U_NSQ(4x2, 0x8B69)
U_NSQ(4x3, 0x8B6A)
#undef U_NSQ
#undef U_PROG_CURRENT
#undef U_PROG_GIVEN

AliasExport(void,glGetUniformfv,,(GLuint program, GLint location, GLfloat *params));
AliasExport(void,glGetUniformiv,,(GLuint program, GLint location, GLint *params));
AliasExport(void,glUniform1f,,(GLint location, GLfloat v0));
AliasExport(void,glUniform2f,,(GLint location, GLfloat v0, GLfloat v1));
AliasExport(void,glUniform3f,,(GLint location, GLfloat v0, GLfloat v1, GLfloat v2));
AliasExport(void,glUniform4f,,(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3));
AliasExport(void,glUniform1i,,(GLint location, GLint v0));
AliasExport(void,glUniform2i,,(GLint location, GLint v0, GLint v1));
AliasExport(void,glUniform3i,,(GLint location, GLint v0, GLint v1, GLint v2));
AliasExport(void,glUniform4i,,(GLint location, GLint v0, GLint v1, GLint v2, GLint v3));
AliasExport(void,glUniform1fv,,(GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glUniform2fv,,(GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glUniform3fv,,(GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glUniform4fv,,(GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glUniform1iv,,(GLint location, GLsizei count, const GLint *value));
AliasExport(void,glUniform2iv,,(GLint location, GLsizei count, const GLint *value));
AliasExport(void,glUniform3iv,,(GLint location, GLsizei count, const GLint *value));
AliasExport(void,glUniform4iv,,(GLint location, GLsizei count, const GLint *value));
AliasExport(void,glUniformMatrix2fv,,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glUniformMatrix3fv,,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glUniformMatrix4fv,,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));

// ============ GL_ARB_shader_objects ================

AliasExport(GLvoid,glUniform1f,ARB,(GLint location, GLfloat v0));
AliasExport(GLvoid,glUniform2f,ARB,(GLint location, GLfloat v0, GLfloat v1));
AliasExport(GLvoid,glUniform3f,ARB,(GLint location, GLfloat v0, GLfloat v1, GLfloat v2));
AliasExport(GLvoid,glUniform4f,ARB,(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3));

AliasExport(GLvoid,glUniform1i,ARB,(GLint location, GLint v0));
AliasExport(GLvoid,glUniform2i,ARB,(GLint location, GLint v0, GLint v1));
AliasExport(GLvoid,glUniform3i,ARB,(GLint location, GLint v0, GLint v1, GLint v2));
AliasExport(GLvoid,glUniform4i,ARB,(GLint location, GLint v0, GLint v1, GLint v2, GLint v3));

AliasExport(GLvoid,glUniform1fv,ARB,(GLint location, GLsizei count, const GLfloat *value));
AliasExport(GLvoid,glUniform2fv,ARB,(GLint location, GLsizei count, const GLfloat *value));
AliasExport(GLvoid,glUniform3fv,ARB,(GLint location, GLsizei count, const GLfloat *value));
AliasExport(GLvoid,glUniform4fv,ARB,(GLint location, GLsizei count, const GLfloat *value));

AliasExport(GLvoid,glUniform1iv,ARB,(GLint location, GLsizei count, const GLint *value));
AliasExport(GLvoid,glUniform2iv,ARB,(GLint location, GLsizei count, const GLint *value));
AliasExport(GLvoid,glUniform3iv,ARB,(GLint location, GLsizei count, const GLint *value));
AliasExport(GLvoid,glUniform4iv,ARB,(GLint location, GLsizei count, const GLint *value));

AliasExport(GLvoid,glUniformMatrix2fv,ARB,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(GLvoid,glUniformMatrix3fv,ARB,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(GLvoid,glUniformMatrix4fv,ARB,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));

AliasExport(GLvoid,glGetUniformfv,ARB,(GLhandleARB programObj, GLint location, GLfloat *params));
AliasExport(GLvoid,glGetUniformiv,ARB,(GLhandleARB programObj, GLint location, GLint *params));

// ===============
AliasExport(void,glProgramUniform1f,,(GLuint program, GLint location, GLfloat v0));
AliasExport(void,glProgramUniform2f,,(GLuint program, GLint location, GLfloat v0, GLfloat v1));
AliasExport(void,glProgramUniform3f,,(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2));
AliasExport(void,glProgramUniform4f,,(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3));
AliasExport(void,glProgramUniform1i,,(GLuint program, GLint location, GLint v0));
AliasExport(void,glProgramUniform2i,,(GLuint program, GLint location, GLint v0, GLint v1));
AliasExport(void,glProgramUniform3i,,(GLuint program, GLint location, GLint v0, GLint v1, GLint v2));
AliasExport(void,glProgramUniform4i,,(GLuint program, GLint location, GLint v0, GLint v1, GLint v2, GLint v3));
AliasExport(void,glProgramUniform1fv,,(GLuint program, GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glProgramUniform2fv,,(GLuint program, GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glProgramUniform3fv,,(GLuint program, GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glProgramUniform4fv,,(GLuint program, GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glProgramUniform1iv,,(GLuint program, GLint location, GLsizei count, const GLint *value));
AliasExport(void,glProgramUniform2iv,,(GLuint program, GLint location, GLsizei count, const GLint *value));
AliasExport(void,glProgramUniform3iv,,(GLuint program, GLint location, GLsizei count, const GLint *value));
AliasExport(void,glProgramUniform4iv,,(GLuint program, GLint location, GLsizei count, const GLint *value));
AliasExport(void,glProgramUniformMatrix2fv,,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix3fv,,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix4fv,,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
// ===============	EXT_direct_state_access (part of it)
AliasExport(void,glProgramUniform1f,EXT,(GLuint program, GLint location, GLfloat v0));
AliasExport(void,glProgramUniform2f,EXT,(GLuint program, GLint location, GLfloat v0, GLfloat v1));
AliasExport(void,glProgramUniform3f,EXT,(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2));
AliasExport(void,glProgramUniform4f,EXT,(GLuint program, GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3));
AliasExport(void,glProgramUniform1i,EXT,(GLuint program, GLint location, GLint v0));
AliasExport(void,glProgramUniform2i,EXT,(GLuint program, GLint location, GLint v0, GLint v1));
AliasExport(void,glProgramUniform3i,EXT,(GLuint program, GLint location, GLint v0, GLint v1, GLint v2));
AliasExport(void,glProgramUniform4i,EXT,(GLuint program, GLint location, GLint v0, GLint v1, GLint v2, GLint v3));
AliasExport(void,glProgramUniform1fv,EXT,(GLuint program, GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glProgramUniform2fv,EXT,(GLuint program, GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glProgramUniform3fv,EXT,(GLuint program, GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glProgramUniform4fv,EXT,(GLuint program, GLint location, GLsizei count, const GLfloat *value));
AliasExport(void,glProgramUniform1iv,EXT,(GLuint program, GLint location, GLsizei count, const GLint *value));
AliasExport(void,glProgramUniform2iv,EXT,(GLuint program, GLint location, GLsizei count, const GLint *value));
AliasExport(void,glProgramUniform3iv,EXT,(GLuint program, GLint location, GLsizei count, const GLint *value));
AliasExport(void,glProgramUniform4iv,EXT,(GLuint program, GLint location, GLsizei count, const GLint *value));
AliasExport(void,glProgramUniformMatrix2fv,EXT,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix3fv,EXT,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix4fv,EXT,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
// =============== GLSL ES 3.x types (hybrid contexts)
AliasExport(void,glUniform1ui,,(GLint location, GLuint v0));
AliasExport(void,glUniform1uiv,,(GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glProgramUniform1ui,,(GLuint program, GLint location, GLuint v0));
AliasExport(void,glProgramUniform1uiv,,(GLuint program, GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glProgramUniform1ui,EXT,(GLuint program, GLint location, GLuint v0));
AliasExport(void,glProgramUniform1uiv,EXT,(GLuint program, GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glUniform2ui,,(GLint location, GLuint v0, GLuint v1));
AliasExport(void,glUniform2uiv,,(GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glProgramUniform2ui,,(GLuint program, GLint location, GLuint v0, GLuint v1));
AliasExport(void,glProgramUniform2uiv,,(GLuint program, GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glProgramUniform2ui,EXT,(GLuint program, GLint location, GLuint v0, GLuint v1));
AliasExport(void,glProgramUniform2uiv,EXT,(GLuint program, GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glUniform3ui,,(GLint location, GLuint v0, GLuint v1, GLuint v2));
AliasExport(void,glUniform3uiv,,(GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glProgramUniform3ui,,(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2));
AliasExport(void,glProgramUniform3uiv,,(GLuint program, GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glProgramUniform3ui,EXT,(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2));
AliasExport(void,glProgramUniform3uiv,EXT,(GLuint program, GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glUniform4ui,,(GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3));
AliasExport(void,glUniform4uiv,,(GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glProgramUniform4ui,,(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3));
AliasExport(void,glProgramUniform4uiv,,(GLuint program, GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glProgramUniform4ui,EXT,(GLuint program, GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3));
AliasExport(void,glProgramUniform4uiv,EXT,(GLuint program, GLint location, GLsizei count, const GLuint *value));
AliasExport(void,glUniformMatrix2x3fv,,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix2x3fv,,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix2x3fv,EXT,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glUniformMatrix2x4fv,,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix2x4fv,,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix2x4fv,EXT,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glUniformMatrix3x2fv,,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix3x2fv,,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix3x2fv,EXT,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glUniformMatrix3x4fv,,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix3x4fv,,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix3x4fv,EXT,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glUniformMatrix4x2fv,,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix4x2fv,,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix4x2fv,EXT,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glUniformMatrix4x3fv,,(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix4x3fv,,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glProgramUniformMatrix4x3fv,EXT,(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value));
AliasExport(void,glGetUniformuiv,,(GLuint program, GLint location, GLuint *params));
