#include "shader.h"
#include "hybrid.h"

#include "../glx/hardext.h"
#include "debug.h"
#include "init.h"
#include "gl4es.h"
#include "glstate.h"
#include "loader.h"
#include "shaderconv.h"
#include "logs.h"

#ifndef NOEGL
// glx/gl31_bridge.c: GLSL >= 1.30 -> GLSL ES through GLADIATOR (hybrid contexts only), NULL if it does not apply
extern char* gl4es_gl31_convert_shader(const char* src, unsigned type);
// same, with GLADIATOR's emulated clip distances (fs_cd: FS discards on them; *cd_n: how many a VS writes)
extern char* gl4es_gl31_convert_shader_ex(const char* src, unsigned type, unsigned flags, int fs_cd, int* cd_n);
#endif

//#define DEBUG
#ifdef DEBUG
#define DBG(a) a
#else
#define DBG(a)
#endif

KHASH_MAP_IMPL_INT(shaderlist, shader_t *);

GLuint APIENTRY_GL4ES gl4es_glCreateShader(GLenum shaderType) {
    DBG(printf("glCreateShader(%s)\n", PrintEnum(shaderType));)
    // sanity check
    if (shaderType!=GL_VERTEX_SHADER && shaderType!=GL_FRAGMENT_SHADER
        && !(isShaderStage(shaderType) && gl4es_hybrid())) {
        DBG(printf("Invalid shader type\n");)
        errorShim(GL_INVALID_ENUM);
        return 0;
    }
    static GLuint lastshader = 0;
    GLuint shader;
    // create the shader
    LOAD_GLES2(glCreateShader);
    if(gles_glCreateShader) {
        shader = gles_glCreateShader(shaderType);
        if(!shader) {
            DBG(printf("Failed to create shader\n");)
            errorGL();
            return 0;
        }
    } else {
        shader = ++lastshader;
        noerrorShim();
    }
    // store the new empty shader in the list for later use
   	khint_t k;
   	int ret;
	khash_t(shaderlist) *shaders = glstate->glsl->shaders;
    k = kh_get(shaderlist, shaders, shader);
    shader_t *glshader = NULL;
    if (k == kh_end(shaders)){
        k = kh_put(shaderlist, shaders, shader, &ret);
        glshader = kh_value(shaders, k) = (shader_t*)calloc(1, sizeof(shader_t));
    } else {
        glshader = kh_value(shaders, k);
    }
    glshader->id = shader;
    glshader->type = shaderType;
    if(glshader->source) {
        free(glshader->source);
        glshader->source = NULL;
    }
    glshader->need.need_texcoord = -1;

    // all done
    return shader;
}

void actually_deleteshader(GLuint shader) {
    khint_t k;
    khash_t(shaderlist) *shaders = glstate->glsl->shaders;
    k = kh_get(shaderlist, shaders, shader);
    if (k != kh_end(shaders)) {
        shader_t *glshader = kh_value(shaders, k);
        if(glshader->deleted && !glshader->attached) {
            kh_del(shaderlist, shaders, k);
            if(glshader->source)
                free(glshader->source);
            if(glshader->converted)
                free(glshader->converted);
            free(glshader);
        }
    }
}

void actually_detachshader(GLuint shader) {
    khint_t k;
    khash_t(shaderlist) *shaders = glstate->glsl->shaders;
    k = kh_get(shaderlist, shaders, shader);
    if (k != kh_end(shaders)) {
        shader_t *glshader = kh_value(shaders, k);
        if((--glshader->attached)<1 && glshader->deleted)
            actually_deleteshader(shader); 
    }
}

void APIENTRY_GL4ES gl4es_glDeleteShader(GLuint shader) {
    DBG(printf("glDeleteShader(%d)\n", shader);)
    // sanity check...
    CHECK_SHADER(void, shader)
    // delete the shader from the list
    if(!glshader) {
        noerrorShim();
        return;
    }
    glshader->deleted = 1;
    noerrorShim();
    if(!glshader->attached) {
        actually_deleteshader(shader);

        // delete the shader in GLES2 hardware (if any)
        LOAD_GLES2(glDeleteShader);
        if(gles_glDeleteShader) {
            errorGL();
            gles_glDeleteShader(shader);
        }   
    }
}

void APIENTRY_GL4ES gl4es_glCompileShader(GLuint shader) {
    DBG(printf("glCompileShader(%d)\n", shader);)
    // look for the shader
    CHECK_SHADER(void, shader)

    glshader->compiled = 1;
    LOAD_GLES2(glCompileShader);
    if(gles_glCompileShader) {
        gles_glCompileShader(glshader->id);
        errorGL();
        #ifndef NOEGL
        // Hybrid context, GLSL <= 1.20 that did not compile as GL4ES's GLSL ES 1.00 (sampler3D, sampler2DShadow,
        // array constructors...): the same source through GLADIATOR, as GLSL ES 3.x. The program then uses one
        // dialect for all its stages, chosen when it links (program.c).
        if(!glshader->gladiator && !glshader->old && gl4es_hybrid()
           && (glshader->type==GL_VERTEX_SHADER || glshader->type==GL_FRAGMENT_SHADER)) {
            LOAD_GLES2(glGetShaderiv);
            GLint st = 0;
            gles_glGetShaderiv(glshader->id, GL_COMPILE_STATUS, &st);
            if(!st)
                gladiatorConvertShader(shader, 2, 0);
        }
        #endif
        if(glshader->gladiator) {
            LOAD_GLES2(glGetShaderiv);
            LOAD_GLES2(glGetShaderInfoLog);
            GLint st = 0; char lg[2000]; lg[0] = 0;
            gles_glGetShaderiv(glshader->id, GL_COMPILE_STATUS, &st);
            if(!st) {
                gles_glGetShaderInfoLog(glshader->id, sizeof(lg), NULL, lg);
                LOGD("LIBGL: shader %d (GLADIATOR) failed to compile: %s\n", glshader->id, lg);
                // the lines of the GLSL ES that was compiled which the driver rejected (+-1), numbered like its messages:
                // "ERROR: unit:line:". A "#line 1 N" starts unit N, a plain "#line 1" unit 0.
                if(glshader->converted) {
                    int eu[16], el[16], ne = 0;
                    for(const char* q = lg; (q = strstr(q, "ERROR: ")) && ne < 16; ) {
                        int u, l;
                        q += 7;
                        if(sscanf(q, "%d:%d:", &u, &l) == 2) { eu[ne] = u; el[ne] = l; ne++; }
                    }
                    const char* p = glshader->converted;
                    int ln = 1, unit = 0;
                    while(*p) {
                        const char* e = strchr(p, '\n');
                        int len = e ? (int)(e - p) : (int)strlen(p);
                        if(len >= 7 && !strncmp(p, "#line 1", 7) && (len == 7 || p[7] == ' ' || p[7] == '\r')) { ln = 0; unit = (len > 8) ? atoi(p + 8) : 0; }
                        for(int k = 0; k < ne; k++)
                            if(eu[k] == unit && ln >= el[k] - 1 && ln <= el[k] + 1) {
                                LOGD("LIBGL:   %d:%3d| %.*s\n", unit, ln, len > 300 ? 300 : len, p);
                                break;
                            }
                        ++ln;
                        if(!e) break;
                        p = e + 1;
                    }
                }
            }
        }
        if(globals4es.logshader) {
            // get compile status and print shaders sources if compile fail...
            LOAD_GLES2(glGetShaderiv);
            LOAD_GLES2(glGetShaderInfoLog);
            GLint status = 0;
            gles_glGetShaderiv(glshader->id, GL_COMPILE_STATUS, &status);
            if(status!=GL_TRUE) {
                printf("LIBGL: Error while compiling shader %d. Original source is:\n%s\n=======\n", glshader->id, glshader->source);
                printf("ShaderConv Source is:\n%s\n=======\n", glshader->converted);
                char tmp[500];
                GLint length;
                gles_glGetShaderInfoLog(glshader->id, 500, &length, tmp);
                printf("Compiler message is\n%s\nLIBGL: End of Error log\n", tmp);
            }
        }
    } else
        noerrorShim();
}

void APIENTRY_GL4ES gl4es_glShaderSource(GLuint shader, GLsizei count, const GLchar * const *string, const GLint *length) {
    DBG(printf("glShaderSource(%d, %d, %p, %p)\n", shader, count, string, length);)
    // sanity check
    if(count<=0) {
        errorShim(GL_INVALID_VALUE);
        return;
    }
    CHECK_SHADER(void, shader)
    // get the size of the shader sources and than concatenate in a single string
    int l = 0;
    for (int i=0; i<count; i++) l+=(length && length[i] >= 0)?length[i]:strlen(string[i]);
    if(glshader->source) free(glshader->source);
    glshader->source = malloc(l+1);
    memset(glshader->source, 0, l+1);
    if(length) {
        for (int i=0; i<count; i++) {
            if(length[i] >= 0)
                strncat(glshader->source, string[i], length[i]);
            else
                strcat(glshader->source, string[i]);
        }
    } else {
        for (int i=0; i<count; i++)
            strcat(glshader->source, string[i]);
    }
    LOAD_GLES2(glShaderSource);
    if (gles_glShaderSource) {
        // adapt shader if needed (i.e. not an es2 context and shader is not #version 100)
        char* gladiator_es = NULL;
        #ifndef NOEGL
        // hybrid context: GLSL >= 1.30 is converted to GLSL ES by GLADIATOR, GL4ES compiles the result as a native ES shader
        glshader->gl31_cd = 0;
        glshader->gl31_flags = 0;
        gladiator_es = gl4es_gl31_convert_shader_ex(glshader->source, glshader->type, 0, 0,
                                                    glshader->type==GL_VERTEX_SHADER ? &glshader->gl31_cd : NULL);
        #endif
        glshader->gladiator = gladiator_es ? 1 : 0;
        if(gladiator_es)
            glshader->converted = gladiator_es;
        else if(glstate->glsl->es2 && !strncmp(glshader->source, "#version 100", 12))
            glshader->converted = strdup(glshader->source);
        else if(isShaderStage(glshader->type))
            glshader->converted = strdup(glshader->source);   // GLADIATOR could not convert it; GL4ES has no geometry/tess path
        else
            glshader->converted = ConvertShader(glshader->source, glshader->type==GL_VERTEX_SHADER?1:0, &glshader->need);
        // send source to GLES2 hardware if any
        gles_glShaderSource(shader, 1, (const GLchar * const*)((glshader->converted)?(&glshader->converted):(&glshader->source)), NULL);
        errorGL();
    } else
        noerrorShim();
}

#define SUPER()     \
    GO(color)       \
    GO(secondary)   \
    GO(fogcoord)    \
    GO(texcoord)    \
    GO(normalmatrix)\
    GO(mvmatrix)    \
    GO(mvpmatrix)   \
    GO(notexarray)  \
    GO(clean)       \
    GO(clipvertex)  \
    GO2(texs)

void accumShaderNeeds(GLuint shader, shaderconv_need_t *need) {
    CHECK_SHADER(void, shader)
    if(!glshader->converted) 
        return;
    #define GO(A) if(need->need_##A < glshader->need.need_##A) need->need_##A = glshader->need.need_##A;
    #define GO2(A) need->need_##A |= glshader->need.need_##A;
    SUPER()
    #undef GO
    #undef GO2
}
int isShaderGladiator(GLuint shader) {
    CHECK_SHADER(int, shader)
    return glshader->gladiator;
}
// A GLADIATOR fragment shader linked with a vertex shader that writes `cd` emulated clip distances: convert it again
// so it reads and discards on them (GLADIATOR's own link does the same in its contexts). Returns 1 if recompiled.
int matchShaderClipDistances(GLuint shader, int cd) {
    CHECK_SHADER(int, shader)
    if(!glshader->gladiator || glshader->type!=GL_FRAGMENT_SHADER || glshader->gl31_cd==cd)
        return 0;
    return gladiatorConvertShader(shader, glshader->gl31_flags, cd);
}

int isShaderStage(GLenum type) {
    return type==0x8DD9 /* GL_GEOMETRY_SHADER */ || type==0x8E88 /* GL_TESS_CONTROL_SHADER */
        || type==0x8E87 /* GL_TESS_EVALUATION_SHADER */;
}

// Converts the shader's source with GLADIATOR (flags: see gl4es_gl31_convert_shader_ex; fs_cd: emulated clip distances
// a fragment shader discards on) and compiles it. On success the shader is GLADIATOR's from then on; on failure it is
// left as it was. Returns 1 if the new source compiled.
int gladiatorConvertShader(GLuint shader, unsigned flags, int fs_cd) {
    CHECK_SHADER(int, shader)
#ifndef NOEGL
    if(!glshader->source || glshader->old)
        return 0;
    int cd = 0;
    char* es = gl4es_gl31_convert_shader_ex(glshader->source, glshader->type, flags, fs_cd,
                                            glshader->type==GL_VERTEX_SHADER ? &cd : NULL);
    if(!es)
        return 0;
    LOAD_GLES2(glShaderSource);
    LOAD_GLES2(glCompileShader);
    LOAD_GLES2(glGetShaderiv);
    GLint st = 0;
    gles_glShaderSource(glshader->id, 1, (const GLchar * const*)&es, NULL);
    gles_glCompileShader(glshader->id);
    gles_glGetShaderiv(glshader->id, GL_COMPILE_STATUS, &st);
    if(!st && !glshader->gladiator && glshader->converted) {
        // keep the GL4ES version that was there (the caller reports the failure as it was)
        gles_glShaderSource(glshader->id, 1, (const GLchar * const*)&glshader->converted, NULL);
        gles_glCompileShader(glshader->id);
        free(es);
        return 0;
    }
    free(glshader->converted);
    glshader->converted = es;
    glshader->gladiator = 1;
    glshader->gl31_flags = flags;
    glshader->gl31_cd = (glshader->type==GL_VERTEX_SHADER) ? cd : fs_cd;
    return st ? 1 : 0;
#else
    (void)flags; (void)fs_cd;
    return 0;
#endif
}
int isShaderCompatible(GLuint shader, shaderconv_need_t *need) {
    CHECK_SHADER(int, shader)
    // a shader converted by GLADIATOR is native GLSL ES 3.x: it does not take part in GL4ES's fixed-function varying
    // negotiation, and reconverting its source with ConvertShader would throw that conversion away
    if(glshader->gladiator)
        return 1;
    if(!glshader->converted)
        return 0;
    #define GO(A) if(need->need_##A > glshader->need.need_##A) return 0;
    #define GO2(A) if(need->need_##A & glshader->need.need_##A) return 0;
    SUPER()
    #undef GO
    #undef GO2
    return 1;
}
#undef SUPER

void redoShader(GLuint shader, shaderconv_need_t *need) {
    LOAD_GLES2(glShaderSource);
    if(!gles_glShaderSource)
        return;
    CHECK_SHADER(void, shader)
    if(!glshader->converted || glshader->gladiator)
        return;
    // test, if no changes, no need to reconvert & recompile...
    if (memcmp(&glshader->need, need, sizeof(shaderconv_need_t))==0)
        return;
    free(glshader->converted);
    memcpy(&glshader->need, need, sizeof(shaderconv_need_t));
    glshader->converted = ConvertShader(glshader->source, glshader->type==GL_VERTEX_SHADER?1:0, &glshader->need);
    // send source to GLES2 hardware if any
    gles_glShaderSource(shader, 1, (const GLchar * const*)((glshader->converted)?(&glshader->converted):(&glshader->source)), NULL);
    // recompile...
    gl4es_glCompileShader(glshader->id);
}

void APIENTRY_GL4ES gl4es_glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source) {
    DBG(printf("glGetShaderSource(%d, %d, %p, %p)\n", shader, bufSize, length, source);)
    // find shader
    CHECK_SHADER(void, shader)
    if (bufSize<=0) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    // if no source, then it's an empty string
    if(glshader->source==NULL) {
        noerrorShim();
        if(length) *length = 0;
        source[0] = '\0';
        return;
    }
    // copy concatenated sources
    GLsizei size = strlen(glshader->source);
    if (size+1>bufSize) size = bufSize-1;
    strncpy(source, glshader->source, size);
    source[size]='\0';
    if(length) *length=size;
    noerrorShim();
}

GLboolean APIENTRY_GL4ES gl4es_glIsShader(GLuint shader) {
    DBG(printf("glIsShader(%d)\n", shader);)
    // find shader
    shader_t *glshader = NULL;
    khint_t k;
    {
        khash_t(shaderlist) *shaders = glstate->glsl->shaders;
        k = kh_get(shaderlist, shaders, shader);
        if (k != kh_end(shaders))
            glshader = kh_value(shaders, k);
    }
    return (glshader)?GL_TRUE:GL_FALSE;
}

shader_t *getShader(GLuint shader) {
    khint_t k;
    {
        khash_t(shaderlist) *shaders = glstate->glsl->shaders;
        k = kh_get(shaderlist, shaders, shader);
        if (k != kh_end(shaders))
            return kh_value(shaders, k);
    }
    return NULL;
}

static const char* GLES_NoGLSLSupport = "No Shader support with current backend";

void APIENTRY_GL4ES gl4es_glGetShaderInfoLog(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog) {
    DBG(printf("glGetShaderInfoLog(%d, %d, %p, %p)\n", shader, maxLength, length, infoLog);)
    // find shader
    CHECK_SHADER(void, shader)
    if(maxLength<=0) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    LOAD_GLES2(glGetShaderInfoLog);
    if(gles_glGetShaderInfoLog) {
        gles_glGetShaderInfoLog(glshader->id, maxLength, length, infoLog);
        errorGL();
    } else {
        strncpy(infoLog, GLES_NoGLSLSupport, maxLength);
        if(length) *length = strlen(infoLog);
    }
}

void APIENTRY_GL4ES gl4es_glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    DBG(printf("glGetShaderiv(%d, %s, %p)\n", shader, PrintEnum(pname), params);)
    // find shader
    CHECK_SHADER(void, shader)
    LOAD_GLES2(glGetShaderiv);
    noerrorShim();
    switch (pname) {
        case GL_SHADER_TYPE:
            *params = glshader->type;
            break;
        case GL_DELETE_STATUS:
            *params = (glshader->deleted)?GL_TRUE:GL_FALSE;
            break;
        case GL_COMPILE_STATUS:
            if(gles_glGetShaderiv) {
                gles_glGetShaderiv(glshader->id, pname, params);
                errorGL();
            } else {
                *params = GL_FALSE; // stub, compile always fail
            }
            break;
        case GL_INFO_LOG_LENGTH:
            if(gles_glGetShaderiv) {
                gles_glGetShaderiv(glshader->id, pname, params);
                errorGL();
            } else {
                *params = strlen(GLES_NoGLSLSupport); // stub, compile always fail
            }
            break;
        case GL_SHADER_SOURCE_LENGTH:
            if(glshader->source)
                *params = strlen(glshader->source)+1;
            else
                *params = 0;
            break;
        default:
            errorShim(GL_INVALID_ENUM);
    }
}

void APIENTRY_GL4ES gl4es_glGetShaderPrecisionFormat(GLenum shaderType, GLenum precisionType, GLint *range, GLint *precision) {
    LOAD_GLES2(glGetShaderPrecisionFormat);
    if(gles_glGetShaderPrecisionFormat) {
        gles_glGetShaderPrecisionFormat(shaderType, precisionType, range, precision);
        errorGL();
    } else {
        errorShim(GL_INVALID_ENUM);
    }
}

void APIENTRY_GL4ES gl4es_glShaderBinary(GLsizei count, const GLuint *shaders, GLenum binaryFormat, const void *binary, GLsizei length) {
    // TODO: check consistancy of "shaders" values
    LOAD_GLES2(glShaderBinary);
    if (gles_glShaderBinary) {
        gles_glShaderBinary(count, shaders, binaryFormat, binary, length);
        errorGL();
    } else {
        errorShim(GL_INVALID_ENUM);
    }
}

void APIENTRY_GL4ES gl4es_glReleaseShaderCompiler(void) {
    LOAD_GLES2(glReleaseShaderCompiler);
    if(gles_glReleaseShaderCompiler) {
        gles_glReleaseShaderCompiler();
        errorGL();
    } else
        noerrorShim();
}

// ========== GL_ARB_shader_objects ==============

AliasExport(GLuint,glCreateShader,,(GLenum shaderType));
AliasExport(void,glDeleteShader,,(GLuint shader));
AliasExport(void,glCompileShader,,(GLuint shader));
AliasExport(void,glShaderSource,,(GLuint shader, GLsizei count, const GLchar * const *string, const GLint *length));
AliasExport(void,glGetShaderSource,,(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source));
AliasExport(GLboolean,glIsShader,,(GLuint shader));
AliasExport(void,glGetShaderInfoLog,,(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog));
AliasExport(void,glGetShaderiv,,(GLuint shader, GLenum pname, GLint *params));
AliasExport(void,glGetShaderPrecisionFormat,,(GLenum shaderType, GLenum precisionType, GLint *range, GLint *precision));
AliasExport(void,glShaderBinary,,(GLsizei count, const GLuint *shaders, GLenum binaryFormat, const void *binary, GLsizei length));
AliasExport_V(void,glReleaseShaderCompiler);


GLhandleARB APIENTRY_GL4ES gl4es_glCreateShaderObject(GLenum shaderType) {
    return gl4es_glCreateShader(shaderType);
}

AliasExport(GLhandleARB,glCreateShaderObject,ARB,(GLenum shaderType));
AliasExport(GLvoid,glShaderSource,ARB,(GLhandleARB shaderObj, GLsizei count, const GLcharARB **string, const GLint *length));
AliasExport(GLvoid,glCompileShader,ARB,(GLhandleARB shaderObj));
AliasExport(GLvoid,glGetShaderSource,ARB,(GLhandleARB obj, GLsizei maxLength, GLsizei *length, GLcharARB *source));
