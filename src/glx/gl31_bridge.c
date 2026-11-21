/* gl31_bridge.c - puente GL4ES <-> GLADIATOR (pasos 1 a 3). Ver gl31_bridge.h.
 *
 * No se incluye gl31.h: sus tipos GLES chocan con los de GL4ES; se declaran solo las 3 funciones usadas. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "gl31_bridge.h"
#include "hardext.h"
#include "../gl/loader.h"
#include "../gl/init.h"
#include "../gl/logs.h"

#ifndef NOEGL

#ifndef EGL_CONTEXT_MINOR_VERSION_KHR
#define EGL_CONTEXT_MINOR_VERSION_KHR 0x30FB
#endif
#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif

extern int  gl31_init(void* (*loader)(const char* name));
extern void gl31_shutdown(void);
extern void gl31_set_max_version(int major, int minor);
extern void* gl31_get_proc_address(const char* name);
extern int  gl4es_glx_current_is_gl31(void);   /* glx.c */
extern int  gl4es_glx_current_is_hybrid(void); /* glx.c */
extern char* gl31_glsl_convert_for_legacy_ex(const char* src, unsigned shader_type, int staged, int fs_cd, int* cd_n,
                                             char* err, size_t errlen); /* GLADIATOR */
char* gl4es_gl31_convert_shader_ex(const char* src, unsigned type, unsigned flags, int fs_cd, int* cd_n);
extern void gl31_ubo_fit(unsigned program);
extern void gl31_ubo_invalidate(void);
extern void gl31_glBindBufferBase(unsigned target, unsigned index, unsigned buffer);
extern void gl31_glBindBufferRange(unsigned target, unsigned index, unsigned buffer, long offset, long size);
extern const unsigned char* gl31_glGetString(unsigned name);
typedef int (*gl31_compat_builtin_fn)(const char* name, int vertex, char* repl, size_t replen, char* decl, size_t decllen);
extern void gl31_glsl_set_compat_builtins(gl31_compat_builtin_fn fn);
extern int gl4es_compat_builtin(const char* name, int vertex, char* repl, size_t replen, char* decl, size_t decllen); /* shaderconv.c */

/* Mayor minor de ES 3.x que se intentara pedir. Baja cuando el driver rechaza uno;
 * -1 = ES 3.x no disponible (se deja de intentar). Solo se baja si la config SI tenia el bit ES3,
 * para no culpar a la version de un fallo que es de la config. */
static int es3_try_minor = 2;

static int gl31_live   = 0;   /* contextos ES3 contados */
static int gl31_ready  = 0;   /* gl31_init OK */
static int gl31_failed = 0;   /* gl31_init fallo: no reintentar en cada MakeCurrent */

static int config_has_es3(EGLDisplay dpy, EGLConfig cfg)
{
    LOAD_EGL(eglGetConfigAttrib);
    EGLint rt = 0;
    if (!egl_eglGetConfigAttrib) return 0;
    if (!egl_eglGetConfigAttrib(dpy, cfg, EGL_RENDERABLE_TYPE, &rt)) return 0;
    return (rt & EGL_OPENGL_ES3_BIT) ? 1 : 0;
}

static EGLint config_attr(EGLDisplay dpy, EGLConfig cfg, EGLint a)
{
    LOAD_EGL(eglGetConfigAttrib);
    EGLint v = 0;
    if (egl_eglGetConfigAttrib) egl_eglGetConfigAttrib(dpy, cfg, a, &v);
    return v;
}

/* Diferencia entre dos configs: perder un canal (alpha, stencil, profundidad) pesa mas que cambiar su tamano */
static long config_distance(EGLDisplay dpy, EGLConfig want, EGLConfig c)
{
    static const EGLint at[] = { EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE, EGL_ALPHA_SIZE, EGL_DEPTH_SIZE,
                                 EGL_STENCIL_SIZE, EGL_SAMPLES };
    long d = 0;
    unsigned i;
    for (i = 0; i < sizeof at / sizeof at[0]; i++) {
        EGLint w = config_attr(dpy, want, at[i]), v = config_attr(dpy, c, at[i]);
        if (w > 0 && v == 0) d += 1000;          /* falta un canal que se pidio */
        d += (w > v) ? (w - v) * 4 : (v - w);    /* menos bits que los pedidos pesa mas que mas bits */
    }
    if ((config_attr(dpy, want, EGL_SURFACE_TYPE) & ~config_attr(dpy, c, EGL_SURFACE_TYPE)) != 0) d += 100000;
    return d;
}

EGLContext gl4es_gl31_create_backend_context(EGLDisplay dpy, EGLConfig* configs, int nconfigs, int* cfgidx,
                                             EGLContext shared, int gl_version, int es2only,
                                             int* es_major, int* es_minor)
{
    static const EGLint attrib_es2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    static const EGLint attrib_none[] = { EGL_NONE };
    EGLContext ctx = EGL_NO_CONTEXT;
    int idx = cfgidx ? *cfgidx : 0;

    LOAD_EGL(eglCreateContext);

    if (es_major) *es_major = (hardext.esversion == 1) ? 1 : 2;
    if (es_minor) *es_minor = (hardext.esversion == 1) ? 1 : 0;

    /* Paso 4, framebuffer por defecto. Con LIBGL_FB=2 (globals4es.usefbo) el "framebuffer 0" de GL4ES es un
     * FBO propio (main FBO) que se vuelca en cada glXSwapBuffers con el pipeline de GL4ES (shaders, VAO,
     * texturas): pisaria el estado que GLADIATOR cree tener y GLADIATOR dibujaria en el framebuffer real,
     * no en el main FBO. Sin usefbo el framebuffer por defecto es la superficie EGL y GLADIATOR ya lo trata
     * (GL_BACK*, sin attach, etc.), asi que solo ese caso se deja en ES2. */
    if (globals4es.usefbo && !es2only && gl_version >= gl31_bridge_min_gl_version() && hardext.esversion >= 2)
        LOGE("LIBGL: GL %d.%d pedido con LIBGL_FB=2 (main FBO): contexto ES2 (GLADIATOR inactivo)\n",
             gl_version / 10, gl_version % 10);

    if (hardext.esversion >= 2 && !es2only && !globals4es.usefbo &&
        gl_version >= gl31_bridge_min_gl_version() && es3_try_minor >= 0) {
        int i, found = -1;
        /* config con bit ES3: la actual si lo tiene, si no la primera de la lista que lo tenga */
        if (idx >= 0 && idx < nconfigs && config_has_es3(dpy, configs[idx])) {
            found = idx;
        } else {
            /* la config ES3 que mas se parece a la pedida (canales, profundidad, stencil, muestras) */
            long best = -1;
            for (i = 0; i < nconfigs; i++) {
                long d;
                if (!config_has_es3(dpy, configs[i])) continue;
                d = (idx >= 0 && idx < nconfigs) ? config_distance(dpy, configs[idx], configs[i]) : 0;
                if (best < 0 || d < best) { best = d; found = i; }
            }
        }
        if (found < 0) {
            LOGE("LIBGL: GL %d.%d pedido pero ninguna config EGL tiene el bit ES3: contexto ES2 (GLADIATOR inactivo)\n",
                 gl_version / 10, gl_version % 10);
        } else {
            int minor;
            for (minor = es3_try_minor; minor >= 0; minor--) {
                EGLint attr[8];
                int n = 0;
                attr[n++] = EGL_CONTEXT_CLIENT_VERSION; attr[n++] = 3;
                /* ES 3.0 se pide sin atributo de minor: no exige EGL_KHR_create_context */
                if (minor > 0) { attr[n++] = EGL_CONTEXT_MINOR_VERSION_KHR; attr[n++] = minor; }
                attr[n++] = EGL_NONE;
                ctx = egl_eglCreateContext(dpy, configs[found], shared, attr);
                if (ctx != EGL_NO_CONTEXT) {
                    if (cfgidx) *cfgidx = found;
                    if (es_major) *es_major = 3;
                    if (es_minor) *es_minor = minor;
                    es3_try_minor = minor;      /* la proxima vez no se vuelven a probar las que fallaron */
                    LOGD("LIBGL: contexto de backend ES 3.%d para GL %d.%d\n", minor, gl_version / 10, gl_version % 10);
                    if ((gl_version >= 43 && minor < 2) || (gl_version >= 40 && minor < 1))
                        LOGD("LIBGL: aviso: GL %d.%d necesita ES 3.%d; con ES 3.%d GLADIATOR anunciara menos\n",
                             gl_version / 10, gl_version % 10, gl_version >= 43 ? 2 : 1, minor);
                    return ctx;
                }
            }
            /* ni ES 3.0 se pudo con una config que tiene el bit ES3: ES3 no esta disponible */
            es3_try_minor = -1;
            LOGE("LIBGL: el driver no crea contextos ES 3.x: contexto ES2 (GLADIATOR inactivo)\n");
        }
    }

    /* ES 3.0 por defecto (LIBGL_ES3=0 lo desactiva): contexto de backend ES 3.0 tambien para contextos GL legados
     * (< 2.0, o con LORICA_GL4ES_ONLY); los GL >= 2.0 ya salen arriba con ES 3.x. El camino AHB
     * (glx_ahb.c) usa glBlitFramebuffer, "#version 300 es" y VAOs, que en un contexto ES 2.0 de ANGLE dan
     * GL_INVALID_OPERATION y dejan la ventana negra. es_major/es_minor se dejan en 2.0: GL4ES sigue siendo
     * quien despacha (no se activa GLADIATOR). */
    {
        const char* e3 = getenv("LIBGL_ES3");
        if (!(e3 && e3[0] == '0') && hardext.esversion >= 2) {
            static const EGLint attrib_es3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            int i, found = -1;
            if (idx >= 0 && idx < nconfigs && config_has_es3(dpy, configs[idx])) found = idx;
            else for (i = 0; i < nconfigs; i++) if (config_has_es3(dpy, configs[i])) { found = i; break; }
            if (found >= 0) {
                ctx = egl_eglCreateContext(dpy, configs[found], shared, attrib_es3);
                if (ctx != EGL_NO_CONTEXT) {
                    if (cfgidx) *cfgidx = found;
                    LOGD("LIBGL: ES 3.0 backend context for legacy GL\n");
                    return ctx;
                }
            }
            LOGE("LIBGL: could not create an ES 3.0 context, using ES 2.0\n");
        }
    }

    /* camino de siempre */
    ctx = egl_eglCreateContext(dpy, configs[idx], shared,
                               (hardext.esversion == 1) ? attrib_none : attrib_es2);
    return ctx;
}

/* ---- arranque de GLADIATOR ---- */

/* Cargador del backend: primero la libreria GLES (simbolos core), luego eglGetProcAddress
 * (las variantes EXT/OES/KHR que gl31_init prueba con sufijo no estan exportadas). */
static void* bridge_loader(const char* name)
{
    void* p = NULL;
    if (gles) p = proc_address(gles, name);
    if (!p) {
        LOAD_EGL(eglGetProcAddress);
        if (egl_eglGetProcAddress) p = (void*)egl_eglGetProcAddress(name);
    }
    return p;
}

void gl4es_gl31_bridge_retain(void)
{
    gl31_live++;
}

void gl4es_gl31_bridge_release(void)
{
    if (gl31_live > 0) gl31_live--;
    if (gl31_live == 0) {
        if (gl31_ready) { gl31_shutdown(); gl31_ready = 0; }
        gl31_failed = 0;
    }
}

int gl4es_gl31_bridge_ready(void)
{
    return gl31_ready;
}

#if defined(__ANDROID__)
#include <unwind.h>
#include <dlfcn.h>
/* Backtrace de la llamada GL que el driver rechazo: con GL_DEBUG_OUTPUT_SYNCHRONOUS el callback corre dentro de ella.
 * Las direcciones salen como modulo+offset (llvm-addr2line sobre la libGL sin strip de la misma compilacion). */
static _Unwind_Reason_Code bridge_bt_frame(struct _Unwind_Context* c, void* arg)
{
    int* k = (int*)arg;
    uintptr_t pc = (uintptr_t)_Unwind_GetIP(c);
    Dl_info di;
    if (!pc) return _URC_END_OF_STACK;
    if (dladdr((void*)pc, &di) && di.dli_fname) {
        const char* b = strrchr(di.dli_fname, '/');
        LOGD("LIBGL:     #%02d %s+0x%lx (%s)\n", *k, b ? b + 1 : di.dli_fname, (unsigned long)(pc - (uintptr_t)di.dli_fbase),
             di.dli_sname ? di.dli_sname : "?");
    } else LOGD("LIBGL:     #%02d pc %p\n", *k, (void*)pc);
    return ++*k >= 14 ? _URC_END_OF_STACK : _URC_NO_REASON;
}
static void bridge_backtrace(void) { int k = 0; _Unwind_Backtrace(bridge_bt_frame, &k); }
#else
static void bridge_backtrace(void) {}
#endif

/* LIBGL_DEBUG: text of the ES driver's own validation errors (KHR_debug message callback), to see why a draw is rejected.
 * The first time each distinct error appears, the backtrace of the GL call that produced it is logged too. */
static void bridge_dbg_cb(unsigned src, unsigned type, unsigned id, unsigned sev, int len, const char* msg, const void* user)
{
    static int n = 0, dropped = 0;
    int first = 0;
    (void)src; (void)len; (void)user;
    /* distinct messages are what matters: drop repeats of the same text beyond a few, but keep every new one */
    {
        static char seen[32][96]; static int seen_cnt[32]; static int nseen = 0;
        int i;
        for (i = 0; i < nseen; i++) if (!strncmp(seen[i], msg, 95)) break;
        if (i == nseen && nseen < 32) { strncpy(seen[nseen], msg, 95); seen[nseen][95] = 0; seen_cnt[nseen] = 0; nseen++; first = 1; }
        if (i < nseen && ++seen_cnt[i] > 5) { dropped++; return; }
    }
    if (n < 400) { n++; LOGD("LIBGL: ES: %s (type 0x%x id %u severity 0x%x) [n=%d, repeats dropped=%d]\n", msg, type, id, sev, n, dropped); }
    if (first && type == 0x824C /* GL_DEBUG_TYPE_ERROR */) bridge_backtrace();
}

/* LIBGL_DEBUG: el callback de debug del driver ES es por contexto. Se activa en cada contexto EGL la primera vez que es
 * el actual, sea ES2 (GL4ES) o ES3 (GLADIATOR), para ver los errores de los draws de GL4ES tambien. */
void gl4es_gl31_debug_context(void)
{
    typedef void (*dbg_cb_fn)(void (*)(unsigned, unsigned, unsigned, unsigned, int, const char*, const void*), const void*);
    typedef void (*dbg_en_fn)(unsigned);
    static EGLContext seen[64];
    static int nseen;
    EGLContext ctx;
    int i;
    dbg_cb_fn dcb;
    dbg_en_fn den;
    if (!getenv("LIBGL_DEBUG")) return;
    LOAD_EGL(eglGetCurrentContext);
    if (!egl_eglGetCurrentContext) return;
    ctx = egl_eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return;
    for (i = 0; i < nseen; i++) if (seen[i] == ctx) return;
    if (nseen < 64) seen[nseen++] = ctx;
    dcb = (dbg_cb_fn)bridge_loader("glDebugMessageCallback");
    den = (dbg_en_fn)bridge_loader("glEnable");
    if (!dcb) dcb = (dbg_cb_fn)bridge_loader("glDebugMessageCallbackKHR");
    if (dcb && den) { dcb(bridge_dbg_cb, NULL); den(0x92E0 /* GL_DEBUG_OUTPUT */); den(0x8242 /* GL_DEBUG_OUTPUT_SYNCHRONOUS */); }
    LOGD("LIBGL: ES driver debug output %s in context %p\n", (dcb && den) ? "enabled" : "unavailable", (void*)ctx);
}

int gl4es_gl31_bridge_ensure(void)
{
    EGLContext ctx;
    EGLDisplay dpy;
    EGLint ver = 0;

    if (gl31_ready) return 1;
    if (gl31_failed || gl31_live <= 0) return 0;

    LOAD_EGL(eglGetCurrentContext);
    LOAD_EGL(eglGetCurrentDisplay);
    LOAD_EGL(eglQueryContext);
    if (!egl_eglGetCurrentContext || !egl_eglGetCurrentDisplay || !egl_eglQueryContext) return 0;

    ctx = egl_eglGetCurrentContext();
    dpy = egl_eglGetCurrentDisplay();
    if (ctx == EGL_NO_CONTEXT || dpy == EGL_NO_DISPLAY) return 0;
    /* gl31_init consulta GL_MAJOR_VERSION / glGetStringi: solo vale con un contexto ES3 actual */
    if (!egl_eglQueryContext(dpy, ctx, EGL_CONTEXT_CLIENT_VERSION, &ver) || ver < 3) return 0;

    if (gl31_init(bridge_loader) != 0) {
        gl31_failed = 1;
        LOGE("LIBGL: gl31_init fallo (backend sin funciones ES 3.0 requeridas): GLADIATOR inactivo\n");
        return 0;
    }
    /* El tope de GLADIATOR sigue a LIBGL_GL, salvo que el usuario fije LORICA_GL_MAX_VERSION
     * (gl31_init ya la leyo). */
    if (!getenv("LORICA_GL_MAX_VERSION") && globals4es.gl >= GL31_BRIDGE_MIN_GL_VERSION)
        gl31_set_max_version(globals4es.gl / 10, globals4es.gl % 10);

    if (getenv("LIBGL_DEBUG")) {
        typedef void (*dbg_cb_fn)(void (*)(unsigned, unsigned, unsigned, unsigned, int, const char*, const void*), const void*);
        typedef void (*dbg_en_fn)(unsigned);
        dbg_cb_fn dcb = (dbg_cb_fn)bridge_loader("glDebugMessageCallback");
        dbg_en_fn den = (dbg_en_fn)bridge_loader("glEnable");
        if (!dcb) dcb = (dbg_cb_fn)bridge_loader("glDebugMessageCallbackKHR");
        if (dcb && den) { dcb(bridge_dbg_cb, NULL); den(0x92E0 /* GL_DEBUG_OUTPUT */); den(0x8242 /* GL_DEBUG_OUTPUT_SYNCHRONOUS */); }
        LOGD("LIBGL: ES driver debug output %s\n", (dcb && den) ? "enabled" : "unavailable");
    }
    /* modo hibrido: GL4ES es el dueno de los built-ins de compatibilidad (atributos y uniforms de estado) */
    gl31_glsl_set_compat_builtins(gl4es_compat_builtin);
    gl31_ready = 1;
    LOGD("LIBGL: GLADIATOR iniciado (backend ES 3.x)\n");
    return 1;
}

/* ---- despacho (paso 3): glGetProcAddress ---- */

/* Devuelve la funcion gl31_* para `name` si el contexto actual es un contexto core GL>=3.1 con
 * GLADIATOR iniciado; NULL en cualquier otro caso (el llamador sigue con la tabla de GL4ES).
 * Los nombres con sufijo ARB se resuelven al nombre base si este esta en la tabla de GLADIATOR
 * (glBindBufferARB -> glBindBuffer): mezclar nombres ARB de GL4ES con nombres core de GLADIATOR
 * dejaria el estado de buffers/texturas repartido en dos sitios.
 * El sufijo EXT se traduce como los alias declarados de gl_API.xml de Mesa: al nombre base cuando la firma es la misma
 * (glBlendFuncSeparateEXT, glFramebufferTexture2DEXT, glProgramUniform1fEXT, glNamedBufferDataEXT...), salvo las familias
 * de EXT_direct_state_access que agregan el parametro `target` (glTexture*EXT, glMultiTex*EXT, glMatrix*EXT...), y con
 * los renombres de EXT_draw_buffers2 (glEnableIndexedEXT -> glEnablei...). */
static const char* const k_ext_sig_differs[] = {   /* prefijos: la version EXT no tiene la firma del nombre base */
    "glTexture", "glMultiTex", "glMatrix", "glCopyTexture", "glCompressedTexture", "glGetTexture", "glGetCompressedTexture",
    "glGetMultiTex", "glCompressedMultiTex", "glCopyMultiTex", "glGetCompressedMultiTex", "glGenerateTextureMipmap",
    "glNamedProgram", "glGetNamedProgram", "glVertexArray", "glFramebufferDrawBuffer", "glFramebufferReadBuffer", NULL
};
static const char* const k_ext_rename[][2] = {      /* EXT_draw_buffers2 -> GL 3.0 */
    { "glEnableIndexedEXT", "glEnablei" },           { "glDisableIndexedEXT", "glDisablei" },
    { "glIsEnabledIndexedEXT", "glIsEnabledi" },     { "glGetIntegerIndexedvEXT", "glGetIntegeri_v" },
    { "glGetBooleanIndexedvEXT", "glGetBooleani_v" }, { "glColorMaskIndexedEXT", "glColorMaski" },
    { NULL, NULL }
};
static void* lookup_gl31(const char* name)
{
    void* p;
    size_t n;
    int i;
    p = gl31_get_proc_address(name);
    if (p) return p;
    n = strlen(name);
    if (n > 3 && n < 96 && strcmp(name + n - 3, "ARB") == 0) {
        char base[96];
        memcpy(base, name, n - 3);
        base[n - 3] = '\0';
        return gl31_get_proc_address(base);
    }
    if (n > 3 && n < 96 && strcmp(name + n - 3, "EXT") == 0) {
        char base[96];
        for (i = 0; k_ext_rename[i][0]; i++)
            if (!strcmp(k_ext_rename[i][0], name)) return gl31_get_proc_address(k_ext_rename[i][1]);
        for (i = 0; k_ext_sig_differs[i]; i++)
            if (!strncmp(name, k_ext_sig_differs[i], strlen(k_ext_sig_differs[i]))) return NULL;
        memcpy(base, name, n - 3);
        base[n - 3] = '\0';
        return gl31_get_proc_address(base);
    }
    return NULL;
}

void* gl4es_gl31_proc_lookup(const char* name)
{
    int cur = (name && gl31_ready) ? gl4es_glx_current_is_gl31() : 0;
    void* p = (name && gl31_ready && cur) ? lookup_gl31(name) : NULL;
    return p;
}

/* "#version NNN" de un shader (0 si no hay): salta espacios y comentarios iniciales */
static int glsl_version_of(const char* s)
{
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (s[0] == '/' && s[1] == '/') { while (*s && *s != '\n') s++; }
        else if (s[0] == '/' && s[1] == '*') { s += 2; while (*s && !(s[0] == '*' && s[1] == '/')) s++; if (*s) s += 2; }
        else break;
    }
    if (strncmp(s, "#version", 8) != 0) return 0;
    return atoi(s + 8);
}

/* Shaders en modo hibrido: GLSL <= 1.20 (GL 2.x) los compila GL4ES como siempre; a partir de #version 130 la
 * conversion a GLSL ES la hace GLADIATOR y GL4ES compila el resultado como un shader ES nativo (conserva su link,
 * sus uniforms y su dibujo). Devuelve el fuente ES (malloc) o NULL si no aplica / GLADIATOR no pudo convertirlo,
 * en cuyo caso GL4ES sigue por su camino de siempre. */
char* gl4es_gl31_convert_shader(const char* src, unsigned type)
{
    return gl4es_gl31_convert_shader_ex(src, type, 0, 0, NULL);
}

/* fs_cd / cd_n: clip distances emuladas (ver gl31_glsl_convert_for_legacy_ex). GL4ES guarda cd_n de cada VS y, al
 * linkear, reconvierte los FS con el fs_cd del VS del programa.
 * flags: 1 = el programa tiene geometry/teselacion (el VS no es la ultima etapa de vertices);
 *        2 = aceptar tambien GLSL <= 1.20 (GL4ES no pudo expresarlo en GLSL ES 1.00, ver shader.c). */
char* gl4es_gl31_convert_shader_ex(const char* src, unsigned type, unsigned flags, int fs_cd, int* cd_n)
{
    char err[512];
    char* out;
    int ver;
    if (cd_n) *cd_n = 0;
    if (!src || !gl31_ready || gl4es_glx_current_is_gl31() || !gl4es_glx_current_is_hybrid()) return NULL;
    ver = glsl_version_of(src);
    if (ver < 130 && !(flags & 2)) return NULL;
    err[0] = 0;
    out = gl31_glsl_convert_for_legacy_ex(src, type, (flags & 1) != 0, fs_cd, cd_n, err, sizeof err);
    if (!out) {
        LOGD("LIBGL: GLADIATOR could not convert a GLSL %d shader (%s), left to GL4ES\n", ver, err);
        return NULL;
    }
    return out;
}

/* El contexto actual es hibrido y GLADIATOR esta listo (lo usan los buffers de UBO & co. de buffers.c) */
int gl4es_gl31_hybrid_active(void)
{
    return gl31_ready && gl4es_glx_current_is_hybrid();
}

/* glReadPixels de profundidad / stencil en un contexto hibrido: GLES no los lee; GLADIATOR los copia por GPU (blit a una
 * textura + compute, requiere ES 3.1) desde el GL_READ_FRAMEBUFFER que el backend tenga ligado. GL4ES liga antes su
 * framebuffer de lectura y resincroniza su estado despues (GLADIATOR restaura su propio FBO de dibujo y su programa).
 * Devuelve 1 si leyo, 0 si fallo, -1 si el contexto no es hibrido (no se intento; GL4ES sigue por su camino). */
extern int  gl31_read_pixels_ex(int x, int y, int w, int h, unsigned format, unsigned type, void* pixels);
extern void gl31_glPixelStorei(unsigned pname, int param);
int gl4es_gl31_read_depth_stencil(int x, int y, int w, int h, unsigned format, unsigned type, void* pixels,
                                  int pack_align, int pack_row_length, int pack_skip_rows, int pack_skip_pixels)
{
    if (!gl4es_gl31_hybrid_active()) return -1;
    /* el empaquetado lo hace GLADIATOR con su estado de pack: el de GL4ES es el que vale */
    gl31_glPixelStorei(0x0D05 /* GL_PACK_ALIGNMENT */, pack_align);
    gl31_glPixelStorei(0x0D02 /* GL_PACK_ROW_LENGTH */, pack_row_length);
    gl31_glPixelStorei(0x0D03 /* GL_PACK_SKIP_ROWS */, pack_skip_rows);
    gl31_glPixelStorei(0x0D04 /* GL_PACK_SKIP_PIXELS */, pack_skip_pixels);
    return gl31_read_pixels_ex(x, y, w, h, format, type, pixels);
}

/* glEnable / glDisable / glIsEnabled de una capacidad que GL4ES no conoce, en un contexto hibrido: GLADIATOR la traduce o la
 * emula (GL_PRIMITIVE_RESTART, GL_TEXTURE_CUBE_MAP_SEAMLESS, GL_FRAMEBUFFER_SRGB, GL_DEPTH_CLAMP...) y pasa el resto a GLES.
 * enable: 1 / 0, o -1 para consultar (resultado en *is). Devuelve 0 si el contexto no es hibrido. */
extern void gl31_glEnable(unsigned cap);
extern void gl31_glDisable(unsigned cap);
extern unsigned char gl31_glIsEnabled(unsigned cap);
int gl4es_gl31_hybrid_enable(unsigned cap, int enable, unsigned char* is)
{
    if (!gl4es_gl31_hybrid_active()) return 0;
    if (enable > 0) gl31_glEnable(cap);
    else if (enable == 0) gl31_glDisable(cap);
    else if (is) *is = gl31_glIsEnabled(cap);
    return 1;
}

/* Capacidades del contexto de backend actual (no del tipo de contexto GL): GL4ES las usa para dejar de emular lo que el ES ya tiene */
extern void gl4es_glx_current_es_version(int* major, int* minor);   /* glx.c */

/* ES >= 3: GL_TEXTURE_BASE_LEVEL / GL_TEXTURE_MAX_LEVEL, glBlitFramebuffer, cadenas de mips de la app */
int gl4es_backend_es3(void)
{
    int major, minor;
    gl4es_glx_current_es_version(&major, &minor);
    return major >= 3;
}

/* GL_CLAMP_TO_BORDER + GL_TEXTURE_BORDER_COLOR: ES 3.2, o EXT/OES/NV_texture_border_clamp */
int gl4es_backend_border_clamp(void)
{
    int major, minor;
    gl4es_glx_current_es_version(&major, &minor);
    if (major >= 3 && minor >= 2) return 1;
    return major >= 2 && hardext.border_clamp;
}

/* Antes de cada draw de GL4ES en un contexto hibrido: bloques uniform mas grandes que su rango ligado (gl31_ubo_fit) */
void gl4es_gl31_ubo_fit(unsigned program)
{
    if (gl31_ready && gl4es_glx_current_is_hybrid()) gl31_ubo_fit(program);
}

/* glLinkProgram de GL4ES: los bloques del programa pueden haber cambiado */
/* Target GLES con el que GLADIATOR creo una textura (3D, 2D_ARRAY...); GL_TEXTURE_2D si no la conoce (las de GL4ES) */
extern unsigned gl31_tex_be_target_of(unsigned id);
unsigned gl4es_gl31_tex_target(unsigned name)
{
    return gl31_ready ? gl31_tex_be_target_of(name) : 0x0DE1 /* GL_TEXTURE_2D */;
}

/* Modo hibrido: formato ES con el que GLADIATOR reservaria un renderbuffer (sin tamano resuelto, lo no renderizable del
 * backend sustituido por capacidades). GL4ES lo usa para sus renderbuffers en vez de sus sustituciones de ES 2. */
extern unsigned gl31_rb_storage_format(unsigned f);
unsigned gl4es_gl31_rb_format(unsigned f)
{
    return (gl31_ready && gl4es_glx_current_is_hybrid()) ? gl31_rb_storage_format(f) : f;
}

/* Modo hibrido (gl/hybrid.c): punto de entrada de GLADIATOR por nombre GL, para lo que GL4ES le delega */
void* gl4es_gl31_fn(const char* name)
{
    return (gl31_ready && name) ? gl31_get_proc_address(name) : NULL;
}

/* GL4ES enlaza el programa en GLES al dibujar; GLADIATOR (transform feedback, ...) necesita saber cual es */
extern void gl31_hybrid_set_program(unsigned program);
void gl4es_gl31_hybrid_program(unsigned program)
{
    if (gl31_ready && gl4es_glx_current_is_hybrid()) gl31_hybrid_set_program(program);
}

/* Modo hibrido: valores de los uniforms propios de GLADIATOR (lorica_*) en un programa que linkeo GL4ES */
extern void gl31_lu_values(float point_size, int pps, unsigned sprite_origin, unsigned mask_lo, float out[3][4]);
int gl4es_gl31_lorica_values(float point_size, int program_point_size, unsigned sprite_origin,
                             unsigned clip_mask_lo, float out[3][4])
{
    if (!gl31_ready || !gl4es_glx_current_is_hybrid()) return 0;
    gl31_lu_values(point_size, program_point_size, sprite_origin, clip_mask_lo, out);
    return 1;
}

void gl4es_gl31_ubo_invalidate(void)
{
    if (gl31_ready && gl4es_glx_current_is_hybrid()) gl31_ubo_invalidate();
}

/* glBindBufferBase / glBindBufferRange con el nombre real del buffer GLES (buffers.c traduce el id de GL4ES) */
void gl4es_gl31_bind_buffer_base(unsigned target, unsigned index, unsigned buffer)
{
    gl31_glBindBufferBase(target, index, buffer);
}

void gl4es_gl31_bind_buffer_range(unsigned target, unsigned index, unsigned buffer, long offset, long size)
{
    gl31_glBindBufferRange(target, index, buffer, offset, size);
}

const char* gl4es_gl31_hybrid_extensions(void)
{
    if (!gl31_ready || !gl4es_glx_current_is_hybrid()) return NULL;
    return (const char*)gl31_glGetString(0x1F03 /* GL_EXTENSIONS */);
}

/* ---- despacho indirecto independiente del contexto (paso 3b, modelo glapi de Mesa) ----
 * Wine (win32u/opengl.c, display_funcs_init) resuelve TODAS las funciones GL una sola vez al arrancar,
 * antes de que exista ningun contexto: dlsym primero y glXGetProcAddressARB despues. Un puntero que
 * dependa del contexto actual en ese momento deja a los contextos core con las funciones de GL4ES para
 * siempre (GLSL compilado como ES 1.00, glGetString de GL4ES...). Por eso, como el glapi de Mesa, se
 * entrega un trampolin fijo cuyo destino se repunta en cada glXMakeCurrent:
 *  - simbolos exportados: el salto de _AliasExport_ (gl/attributes.h), registrado en la seccion lorica_disp;
 *  - nombres que no se exportan pero que implementan GL4ES y GLADIATOR: un trampolin de un pool estatico.
 * Destino: GLADIATOR con un contexto core GL>=3.1 (si lo tiene), GL4ES en cualquier otro caso.
 * Limite conocido: el modo es del proceso, no del hilo (como antes). Se desactiva con -DLORICA_NO_DISPATCH. */
#if defined(__linux__) && !defined(LORICA_NO_DISPATCH) && (defined(__x86_64__) || defined(__aarch64__))
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#define gettid() ((long)syscall(SYS_gettid))

typedef struct { const char* name; void** slot; void* def; void* stub; } lorica_disp_t;
extern lorica_disp_t __start_lorica_disp[] __attribute__((weak, visibility("hidden")));
extern lorica_disp_t __stop_lorica_disp[]  __attribute__((weak, visibility("hidden")));
static int disp_mode = -1;   /* -1 desconocido, 0 GL4ES, 1 GLADIATOR */

/* Pool de trampolines: el i-esimo salta por lorica_dyn_slots[i] y mide 16 bytes en las dos arquitecturas.
 * Los indices se generan con el preprocesador (0x000..0x3ff) para no depender de .set dentro de .rept. */
#define LORICA_DYN_STUBS 1024
void* lorica_dyn_slots[LORICA_DYN_STUBS] __attribute__((visibility("hidden")));
extern const char lorica_dyn_stub0[] __attribute__((visibility("hidden")));
#ifdef __x86_64__
#define LDS_STUB(i) "endbr64\n\tjmp *lorica_dyn_slots+" #i "*8(%rip)\n\t.p2align 4,0xcc\n\t"
#else
#define LDS_STUB(i) "hint 34\n\tadrp x16, lorica_dyn_slots+" #i "*8\n\tldr x16, [x16, :lo12:lorica_dyn_slots+" #i "*8]\n\tbr x16\n\t"
#endif
/* Los parametros de estas macros no pueden llamarse como un digito hexadecimal (a..f): el preprocesador
 * sustituiria ese digito en el indice del stub. */
#define LDS_H(HI,MID,LO) LDS_STUB(0x##HI##MID##LO)
#define LDS_16(HI,MID) LDS_H(HI,MID,0) LDS_H(HI,MID,1) LDS_H(HI,MID,2) LDS_H(HI,MID,3) LDS_H(HI,MID,4) LDS_H(HI,MID,5) \
                       LDS_H(HI,MID,6) LDS_H(HI,MID,7) LDS_H(HI,MID,8) LDS_H(HI,MID,9) LDS_H(HI,MID,a) LDS_H(HI,MID,b) \
                       LDS_H(HI,MID,c) LDS_H(HI,MID,d) LDS_H(HI,MID,e) LDS_H(HI,MID,f)
#define LDS_256(HI) LDS_16(HI,0) LDS_16(HI,1) LDS_16(HI,2) LDS_16(HI,3) LDS_16(HI,4) LDS_16(HI,5) LDS_16(HI,6) LDS_16(HI,7) \
                    LDS_16(HI,8) LDS_16(HI,9) LDS_16(HI,a) LDS_16(HI,b) LDS_16(HI,c) LDS_16(HI,d) LDS_16(HI,e) LDS_16(HI,f)
__asm__(".text\n\t.p2align 4\n\t.globl lorica_dyn_stub0\n\t.hidden lorica_dyn_stub0\n\t.type lorica_dyn_stub0,@function\n"
        "lorica_dyn_stub0:\n\t"
        LDS_256(0) LDS_256(1) LDS_256(2) LDS_256(3)
        ".size lorica_dyn_stub0,.-lorica_dyn_stub0\n");

typedef struct { const char* name; void* own; void* gl31; } lorica_dyn_t;
static lorica_dyn_t dyn_tab[LORICA_DYN_STUBS];
static int dyn_count = 0;
static pthread_mutex_t dyn_lock = PTHREAD_MUTEX_INITIALIZER;

/* Perfil core: las funciones exportadas que GLADIATOR no implementa son las que el perfil core elimino (glBegin,
 * matrices, listas...). Como el dispatch "generic nop" de Mesa, generan GL_INVALID_OPERATION y devuelven 0
 * (glGenLists, glIsList, glRenderMode) en lugar de tocar el estado de GL4ES, que en un contexto core no se usa. */
extern void gl31_set_error(unsigned e);
static long core_removed_nop(void)
{
    gl31_set_error(0x0502 /* GL_INVALID_OPERATION */);
    return 0;
}

static void* disp_pick(int mode, void* own, void* gl31)
{
    if (mode == 1) return gl31 ? gl31 : own;
    return own ? own : gl31;
}

void gl4es_gl31_dispatch_update(void)
{
    lorica_disp_t* e;
    int i, mode = (gl31_ready && gl4es_glx_current_is_gl31()) ? 1 : 0;
    if (mode == disp_mode) return;
    pthread_mutex_lock(&dyn_lock);
    for (e = __start_lorica_disp; e < __stop_lorica_disp; e++) {
        void* p = mode ? gl4es_gl31_proc_lookup(e->name) : NULL;
        if (!p && mode && strncmp(e->name, "glX", 3) != 0) p = (void*)core_removed_nop;
        *e->slot = p ? p : e->def;
    }
    for (i = 0; i < dyn_count; i++)
        lorica_dyn_slots[i] = disp_pick(mode, dyn_tab[i].own, dyn_tab[i].gl31);
    disp_mode = mode;
    pthread_mutex_unlock(&dyn_lock);
}

/* Trampolin para `name` (exportado o del pool); NULL si no queda ninguno */
static void* dispatch_stub(const char* name, void* own, void* gl31)
{
    lorica_disp_t* e;
    void* r = NULL;
    int i;
    for (e = __start_lorica_disp; e < __stop_lorica_disp; e++)
        if (e->name[0] == name[0] && strcmp(e->name, name) == 0) return e->stub;
    pthread_mutex_lock(&dyn_lock);
    for (i = 0; i < dyn_count; i++)
        if (strcmp(dyn_tab[i].name, name) == 0) { r = (void*)(lorica_dyn_stub0 + 16 * i); break; }
    if (!r && dyn_count < LORICA_DYN_STUBS) {
        char* nm = strdup(name);
        if (nm) {
            i = dyn_count;
            dyn_tab[i].name = nm; dyn_tab[i].own = own; dyn_tab[i].gl31 = gl31;
            lorica_dyn_slots[i] = disp_pick(disp_mode < 0 ? 0 : disp_mode, own, gl31);
            __atomic_store_n(&dyn_count, i + 1, __ATOMIC_RELEASE);
            r = (void*)(lorica_dyn_stub0 + 16 * i);
        }
    } else if (!r) {
        static int warned = 0;
        if (!warned++) LOGE("LIBGL: no free dispatch stubs (%d): %s resolves to the current context\n",
                            LORICA_DYN_STUBS, name);
    }
    pthread_mutex_unlock(&dyn_lock);
    return r;
}
#define LORICA_HAVE_DISPATCH 1
#else
void gl4es_gl31_dispatch_update(void) {}
#endif

/* glGetProcAddress: `own` es lo que GL4ES tiene para `name` (o NULL). Si GLADIATOR tambien lo implementa,
 * se devuelve un trampolin que elige la capa segun el contexto actual en cada llamada, no en esta. */
void* gl4es_gl31_resolve(const char* name, void* own)
{
    void* g;
    if (!name) return own;
    if (globals4es.gl < gl31_bridge_min_gl_version()) return own;
    g = lookup_gl31(name);
    if (!g) return own;
    if (!own || own == g) return g;
#ifdef LORICA_HAVE_DISPATCH
    {
        void* s = dispatch_stub(name, own, g);
        if (s) return s;
    }
#endif
    /* sin despacho: lo que corresponde al contexto actual */
    return (gl31_ready && gl4es_glx_current_is_gl31()) ? g : own;
}

#endif /* NOEGL */
