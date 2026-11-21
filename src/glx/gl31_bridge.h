#ifndef _GL4ES_GL31_BRIDGE_H_
#define _GL4ES_GL31_BRIDGE_H_

/* Puente GL4ES <-> GLADIATOR (gl31_*), pasos 1 y 2 de la integracion:
 *   1. contextos EGL de ES 3.x cuando la app pide GL >= 3.1
 *   2. arranque / parada de GLADIATOR (gl31_init / gl31_shutdown) con un cargador del backend GLES
 * El despacho por contexto cubre solo glGetProcAddress (paso 3); el framebuffer por defecto (paso 4)
 * se resuelve dejando LIBGL_FB=2 en ES2 (ver gl4es_gl31_create_backend_context). */

#ifndef NOEGL
#include <EGL/egl.h>

/* GL version (major*10+minor) a partir de la cual el contexto de backend pasa a ES 3.x */
#define GL31_BRIDGE_MIN_GL_VERSION 31

/* Desde que version GL (de contexto o de LIBGL_GL_COMPAT) el backend es ES 3.x y GL4ES trabaja en modo hibrido.
 * Un contexto GL 2.x tambien: el driver ES3 tiene lo que GLSL ES 1.00 no expresa (sampler3D, sampler2DShadow,
 * MRT, texturas float, lectura de profundidad/stencil...) y GLADIATOR lo convierte, igual que en un 3.2 de
 * compatibilidad; el contexto sigue anunciando su version (2.1). LORICA_GL4ES_ONLY=1 vuelve a GL4ES puro sobre
 * ES2 por debajo de 3.1 / 3.2 (para comparar o si el driver no tiene ES3). */
#include <stdlib.h>
static inline int gl31_bridge_min_gl_version(void)    { return getenv("LORICA_GL4ES_ONLY") ? 31 : 20; }
static inline int gl31_bridge_hybrid_min_version(void) { return getenv("LORICA_GL4ES_ONLY") ? 32 : 20; }

/* Crea el EGLContext de backend para un contexto GLX.
 *   gl_version : version GL negociada (0 = legacy)
 *   es2only    : contexto pedido con perfil ES2
 *   configs / nconfigs / *cfgidx : configs devueltas por eglChooseConfig; *cfgidx se cambia
 *                si hace falta otra con el bit ES3
 *   *es_major / *es_minor : version ES del contexto creado (2.0 si cae a ES2)
 * Con gl_version < 31 (o es2only, o backend ES1, o LIBGL_FB=2 / globals4es.usefbo) se comporta exactamente
 * como antes: ES2. */
EGLContext gl4es_gl31_create_backend_context(EGLDisplay dpy, EGLConfig* configs, int nconfigs, int* cfgidx,
                                             EGLContext shared, int gl_version, int es2only,
                                             int* es_major, int* es_minor);

/* Cuenta de contextos ES3 vivos (decide cuando se apaga GLADIATOR) */
void gl4es_gl31_bridge_retain(void);
void gl4es_gl31_bridge_release(void);   /* llamar con el contexto que se destruye aun activo */

/* Arranca GLADIATOR si el contexto EGL actual es ES3+. Devuelve 1 si GLADIATOR esta listo.
 * Barato si ya esta iniciado o si el contexto actual es ES2. */
int gl4es_gl31_bridge_ensure(void);
void gl4es_gl31_debug_context(void);   /* LIBGL_DEBUG: callback de debug del ES en el contexto actual */

int gl4es_gl31_bridge_ready(void);

/* Funcion gl31_* para `name` si el contexto GLX actual es core GL>=3.1 con backend ES 3.x y GLADIATOR
 * iniciado; NULL si no. La usa el despacho para elegir el destino de cada trampolin. */
void* gl4es_gl31_proc_lookup(const char* name);

/* Paso 3: glGetProcAddress. `own` es la funcion de GL4ES (o NULL). Devuelve la de GLADIATOR si GL4ES no la
 * tiene, `own` si GLADIATOR no la tiene, y si las dos la tienen un trampolin que elige la capa segun el
 * contexto actual EN CADA LLAMADA (Wine resuelve todo una vez, sin contexto). En modo hibrido el trampolin
 * va a GL4ES, que sigue siendo el dueno de lo que implementa. */
void* gl4es_gl31_resolve(const char* name, void* own);
const char* gl4es_gl31_hybrid_extensions(void);          /* lista "A B C" de extensiones de GLADIATOR */

/* Paso 3b: repunta los trampolines (simbolos exportados y pool de glGetProcAddress): GLADIATOR con contexto
 * core GL>=3.1, GL4ES si no. Llamar tras cada cambio del contexto GLX actual. Barato si el modo no cambia. */
void gl4es_gl31_dispatch_update(void);
#endif /* NOEGL */

#endif
