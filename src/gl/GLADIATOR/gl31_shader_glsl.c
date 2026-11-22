/* gl31_shader_glsl.c - conversor GLSL 1.10-1.50 / 3.30  ->  GLSL ES 3.00 / 3.10 / 3.20
 * (la version de salida es la del backend: gl31_caps.glsl_es)
 *
 * Transformaciones (textuales, salvo las conversiones de tipo, que usan un parser con tipos):
 *   - #version NNN [core]  ->  #version 300 es  + precisiones por defecto
 *   - #extension GL_ARB_*  -> se elimina; el resto se sube a la cabecera
 *   - layout(): se quitan los calificadores que ES 3.00 no admite
 *     (binding, index, offset, ...) y location en varyings
 *   - GLSL < 1.30: attribute/varying -> in/out ; gl_FragColor/gl_FragData
 *   - texture2D/textureCube/... -> texture/textureLod/...
 *   - noperspective se elimina
 *   - sampler1D / 1DArray / 2DRect (y variantes i/u) -> sampler2D / 2DArray; las llamadas
 *     texture/Lod/Proj/ProjLod/Grad/texelFetch/textureSize sobre ellos se redirigen a funciones
 *     auxiliares lorica_* inyectadas en la cabecera (coordenada extra 0.5, o p/size en Rect), y
 *     la familia *Offset a macros (el offset tiene que seguir siendo una expresion constante)
 *   - samplerBuffer, sampler2DMS, gl_ClipDistance y shaders de geometria: se pasan tal cual
 *     si el backend los tiene (con el #extension EXT/OES necesario bajo ES 3.20); si no, error
 *   - glBindFragDataLocation: se inyecta layout(location=N) en las salidas del fragment shader
 * Se rechaza (error claro) lo que no tiene equivalente: sombras 1D/Rect y built-ins del
 * perfil de compatibilidad (FPE).
 *   - conversiones implicitas int -> uint -> float (GLSL de escritorio las permite, ES no): se hacen explicitas con un
 *     parser con tipos (gl31_glsl_tc.c)
 * Los numeros de linea se conservan (#line 1) para que los logs sean utiles. */
#include "gl31.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <ctype.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

char* gl31_glsl_implicit_conv(const char* src);        /* gl31_glsl_tc.c */

typedef struct { char* p; size_t n, cap; int oom; } sb_t;

static void sb_cat(sb_t* b, const char* s, size_t len)
{
    if (b->oom) return;
    if (b->n + len + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        char* p;
        while (c < b->n + len + 1) c *= 2;
        p = (char*)realloc(b->p, c);
        if (!p) { b->oom = 1; return; }
        b->p = p; b->cap = c;
    }
    if (len) memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = 0;
}
static void sb_str(sb_t* b, const char* s) { sb_cat(b, s, strlen(s)); }

static int is_id(unsigned char c) { return isalnum(c) || c == '_'; }

static void set_err(char* err, size_t len, const char* msg, const char* arg)
{
    if (err && len) snprintf(err, len, "ERROR: 0:1: %s%s%s", msg, arg ? ": " : "", arg ? arg : "");
}

/* comentarios -> espacios (se conservan los \n) */
static void strip_comments(char* s)
{
    while (*s) {
        if (s[0] == '/' && s[1] == '/') {
            /* un `\` al final de la linea (con \r o sin el) continua el comentario en la siguiente, como en C */
            while (*s && *s != '\n') {
                if (*s == '\\' && (s[1] == '\n' || (s[1] == '\r' && s[2] == '\n'))) {
                    *s++ = ' ';
                    if (*s == '\r') *s++ = ' ';
                    s++;                         /* el salto de linea se conserva y el comentario sigue */
                    continue;
                }
                *s++ = ' ';
            }
        } else if (s[0] == '/' && s[1] == '*') {
            s[0] = s[1] = ' '; s += 2;
            while (*s && !(s[0] == '*' && s[1] == '/')) { if (*s != '\n') *s = ' '; s++; }
            if (*s) { s[0] = s[1] = ' '; s += 2; }
        } else s++;
    }
}

static const char* find_word(const char* s, const char* w)
{
    size_t n = strlen(w);
    const char* p = s;
    while ((p = strstr(p, w)) != NULL) {
        int pre  = (p == s) || !is_id((unsigned char)p[-1]);
        int post = !is_id((unsigned char)p[n]);
        if (pre && post) return p;
        p += n;
    }
    return NULL;
}

/* Reemplaza palabras completas. Libera s y devuelve la nueva cadena (NULL si OOM). */
static char* repl_word(char* s, const char* from, const char* to)
{
    sb_t b = {0, 0, 0, 0};
    const char *p, *q;
    size_t fl = strlen(from);
    if (!s || !find_word(s, from)) return s;
    p = s;
    while ((q = find_word(p, from)) != NULL) {
        sb_cat(&b, p, (size_t)(q - p));
        sb_str(&b, to);
        p = q + fl;
    }
    sb_str(&b, p);
    free(s);
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

/* ---------- GL_EXT_shader_integer_mix ---------- */
/* mix(genIType/genUType/genBType, ..., genBType): GLSL 4.50 / EXT_shader_integer_mix. ES 3.x solo trae mix() de float con
 * selector bool; los demas tipos se anaden como sobrecargas de usuario (ES permite sobrecargar funciones incorporadas). */
static GL31_TLS int g_imix;

static void emit_integer_mix(sb_t* sb)
{
    static const char* const base[3] = { "int", "uint", "bool" };
    static const char* const pre[3]  = { "i", "u", "b" };
    int t, n;
    for (t = 0; t < 3; t++) {
        for (n = 1; n <= 4; n++) {
            char ty[16], bt[16], line[400];
            static const char* const sw[4] = { "x", "y", "z", "w" };
            if (n == 1) { snprintf(ty, sizeof ty, "%s", base[t]); snprintf(bt, sizeof bt, "bool"); }
            else { snprintf(ty, sizeof ty, "%svec%d", pre[t], n); snprintf(bt, sizeof bt, "bvec%d", n); }
            if (n == 1) {
                snprintf(line, sizeof line, "%s lorica_mix(%s x, %s y, bool a){ return a ? y : x; }\n", ty, ty, ty);
            } else {
                char comps[200] = "";
                int k;
                for (k = 0; k < n; k++) {
                    char c[40];
                    snprintf(c, sizeof c, "%sa.%s ? y.%s : x.%s", k ? ", " : "", sw[k], sw[k], sw[k]);
                    strcat(comps, c);
                }
                snprintf(line, sizeof line, "%s lorica_mix(%s x, %s y, %s a){ return %s(%s); }\n", ty, ty, ty, bt, ty, comps);
            }
            sb_str(sb, line);
        }
    }
    /* mix() de float: el nombre incorporado no se puede sobrecargar, asi que todo mix() del shader pasa a lorica_mix() */
    for (n = 1; n <= 4; n++) {
        char ty[16], bt[16], line[300];
        if (n == 1) { snprintf(ty, sizeof ty, "float"); snprintf(bt, sizeof bt, "bool"); }
        else { snprintf(ty, sizeof ty, "vec%d", n); snprintf(bt, sizeof bt, "bvec%d", n); }
        snprintf(line, sizeof line, "%s lorica_mix(%s x, %s y, %s a){ return mix(x, y, a); }\n", ty, ty, ty, ty);
        sb_str(sb, line);
        snprintf(line, sizeof line, "%s lorica_mix(%s x, %s y, %s a){ return mix(x, y, a); }\n", ty, ty, ty, bt);
        sb_str(sb, line);
        if (n > 1) {
            snprintf(line, sizeof line, "%s lorica_mix(%s x, %s y, float a){ return mix(x, y, a); }\n", ty, ty, ty);
            sb_str(sb, line);
        }
    }
}

/* ---------- #extension ---------- */
/* extensiones EXT/NV de escritorio que ES 3.x ya trae en el nucleo (o no tiene): el compilador ES las rechaza */
static int desktop_only_ext(const char* n)
{
    static const char* const k[] = {
        "GL_EXT_texture_array", "GL_EXT_gpu_shader4", "GL_EXT_texture_buffer_object", "GL_EXT_geometry_shader4",
        "GL_EXT_draw_buffers2", "GL_EXT_texture_integer", "GL_EXT_packed_float", "GL_EXT_texture_shared_exponent",
        "GL_EXT_texture_swizzle", "GL_EXT_shadow_funcs", "GL_EXT_framebuffer_object", "GL_EXT_framebuffer_blit",
        "GL_EXT_draw_instanced", "GL_EXT_bindable_uniform", "GL_EXT_transform_feedback", "GL_NV_gpu_program4",
        "GL_NV_gpu_shader5", "GL_NV_shader_buffer_load", "GL_EXT_texture3D", "GL_EXT_timer_query", NULL };
    int i;
    for (i = 0; k[i]; i++) {
        size_t l = strlen(k[i]);
        if (strncmp(n, k[i], l) == 0 && !isalnum((unsigned char)n[l]) && n[l] != '_') return 1;
    }
    return 0;
}

static void process_extensions(char* s, sb_t* ext)
{
    char* line = s;
    while (*line) {
        char* eol = strchr(line, '\n');
        char* t = line;
        if (!eol) eol = line + strlen(line);
        while (t < eol && (*t == ' ' || *t == '\t' || *t == '\r')) t++;
        if (eol - t >= 10 && strncmp(t, "#extension", 10) == 0) {
            const char* name = t + 10;
            while (name < eol && isspace((unsigned char)*name)) name++;
            if (strncmp(name, "GL_EXT_shader_integer_mix", 25) == 0) {
                g_imix = 1;                          /* ES no la tiene: se emula con sobrecargas de mix() */
            } else if (strncmp(name, "GL_ARB_", 7) != 0 && !desktop_only_ext(name)) {   /* ARB: ya es core en ES */
                sb_cat(ext, t, (size_t)(eol - t));
                sb_str(ext, "\n");
            }
            while (t < eol) *t++ = ' ';
        }
        line = *eol ? eol + 1 : eol;
    }
}

/* ---------- layout() ---------- */
/* calificadores que ES (cualquier version) no admite */
static const char* const k_layout_strip[] = {
    "index", "component", "origin_upper_left", "pixel_center_integer",
    "xfb_buffer", "xfb_offset", "xfb_stride",
    "depth_any", "depth_greater", "depth_less", "depth_unchanged", NULL
};
/* calificadores que ES 3.00 no admite pero ES 3.10+ si (binding de UBO/SSBO/samplers/imagenes/atomics) */
static const char* const k_layout_strip_300[] = { "binding", "offset", "early_fragment_tests", NULL };

static GL31_TLS int g_saw_index;      /* un layout(index = N) quedo en el shader */

static int layout_strip_name(const char* n, size_t len, int es)
{
    int i;
    if (len == 5 && strncmp(n, "index", 5) == 0 && gl31_caps.dual_src) { g_saw_index = 1; return 0; }
    for (i = 0; k_layout_strip[i]; i++)
        if (strlen(k_layout_strip[i]) == len && strncmp(k_layout_strip[i], n, len) == 0) return 1;
    if (es < 310)
        for (i = 0; k_layout_strip_300[i]; i++)
            if (strlen(k_layout_strip_300[i]) == len && strncmp(k_layout_strip_300[i], n, len) == 0) return 1;
    return 0;
}

/* 1 = in, 2 = out, 0 = otra cosa; salta calificadores intermedios */
static int next_direction(const char* p)
{
    static const char* const skip[] = { "flat", "smooth", "centroid", "noperspective", "invariant",
                                        "highp", "mediump", "lowp", NULL };
    int iter;
    for (iter = 0; iter < 8; iter++) {
        const char* w; size_t n; int i, found = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        w = p;
        while (is_id((unsigned char)*p)) p++;
        n = (size_t)(p - w);
        if (!n) return 0;
        if (n == 2 && strncmp(w, "in", 2) == 0)  return 1;
        if (n == 3 && strncmp(w, "out", 3) == 0) return 2;
        for (i = 0; skip[i]; i++)
            if (strlen(skip[i]) == n && strncmp(skip[i], w, n) == 0) { found = 1; break; }
        if (!found) return 0;
    }
    return 0;
}

static void emit_layout(sb_t* b, const char* a, const char* z, int drop_location, int es)
{
    sb_t t = {0, 0, 0, 0};
    const char* it = a;
    const char* c;
    int kept = 0;
    while (it <= z) {
        const char *end = it, *s0, *e0, *nm;
        while (end < z && *end != ',') end++;
        s0 = it; e0 = end;
        while (s0 < e0 && isspace((unsigned char)*s0)) s0++;
        while (e0 > s0 && isspace((unsigned char)e0[-1])) e0--;
        if (e0 > s0) {
            size_t nlen = 0;
            nm = s0;
            while (nm + nlen < e0 && is_id((unsigned char)nm[nlen])) nlen++;
            if (!layout_strip_name(nm, nlen, es) &&
                !(drop_location && nlen == 8 && strncmp(nm, "location", 8) == 0)) {
                if (kept++) sb_str(&t, ", ");
                sb_cat(&t, s0, (size_t)(e0 - s0));
            }
        }
        if (end >= z) break;
        it = end + 1;
    }
    if (kept) { sb_str(b, "layout("); sb_cat(b, t.p, t.n); sb_str(b, ")"); }
    for (c = a; c < z; c++) if (*c == '\n') sb_str(b, "\n");   /* conservar lineas */
    free(t.p);
}

static char* filter_layouts(const char* s, GLenum type, int es)
{
    sb_t b = {0, 0, 0, 0};
    const char* p = s;
    for (;;) {
        const char* q = find_word(p, "layout");
        const char *o, *close;
        int dir, drop;
        if (!q) break;
        o = q + 6;
        while (*o && isspace((unsigned char)*o)) o++;
        close = (*o == '(') ? strchr(o, ')') : NULL;
        if (!close) { sb_cat(&b, p, (size_t)(q + 6 - p)); p = q + 6; continue; }
        sb_cat(&b, p, (size_t)(q - p));
        dir = next_direction(close + 1);
        /* Segunda fuente de mezcla (layout(location = 0, index = 1) out ...) sin EXT_blend_func_extended: ES no tiene dual-source
         * y dos salidas en location 0 no compilan (S0059). La segunda salida pasa a ser una variable global privada: el shader
         * compila y solo se pierde esa fuente del mezclado. */
        if (type == GL_FRAGMENT_SHADER && dir == 2 && !gl31_caps.dual_src) {
            const char* ix = o + 1;
            int is1 = 0;
            while (ix < close) {
                const char* w = find_word(ix, "index");
                if (!w || w >= close) break;
                w += 5;
                while (w < close && isspace((unsigned char)*w)) w++;
                if (w < close && *w == '=') {
                    w++;
                    while (w < close && isspace((unsigned char)*w)) w++;
                    if (w < close && *w == '1' && !is_id((unsigned char)w[1])) is1 = 1;
                }
                ix = w;
            }
            if (is1) {
                const char* a = close + 1;
                const char* c;
                while (*a && isspace((unsigned char)*a)) a++;
                while (*a == 'f' || *a == 's' || *a == 'c') {   /* flat/smooth/centroid antes de out: no hacen falta */
                    const char* e2 = a; while (is_id((unsigned char)*e2)) e2++;
                    if ((size_t)(e2 - a) == 4 || (size_t)(e2 - a) == 6 || (size_t)(e2 - a) == 8) { a = e2; while (*a && isspace((unsigned char)*a)) a++; } else break;
                }
                if (a[0] == 'o' && a[1] == 'u' && a[2] == 't' && !is_id((unsigned char)a[3])) {
                    for (c = o; c < a + 3; c++) if (*c == '\n') sb_str(&b, "\n");
                    p = a + 3;
                    continue;
                }
            }
        }
        /* ES 3.00: location solo en entradas de vertice y salidas de fragmento; ES 3.10+ lo admite entre etapas */
        drop = es < 310 && ((type == GL_VERTEX_SHADER && dir == 2) || (type == GL_FRAGMENT_SHADER && dir == 1));
        emit_layout(&b, o + 1, close, drop, es);
        p = close + 1;
    }
    sb_str(&b, p);
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

/* ---------- tablas ---------- */
/* siempre sin equivalente: perfil de compatibilidad / FPE y sombras 1D/Rect */
static const char* const k_unsupported[] = {
    "sampler1DShadow", "sampler1DArrayShadow", "sampler2DRectShadow",
    "image1D", "iimage1D", "uimage1D", "image1DArray", "iimage1DArray", "uimage1DArray",
    "image2DRect", "iimage2DRect", "uimage2DRect", "subroutine", "textureQueryLod", "textureQueryLevels",
    "gl_Vertex", "gl_Normal", "gl_Color", "gl_SecondaryColor", "gl_MultiTexCoord0",
    "gl_ModelViewMatrix", "gl_ProjectionMatrix", "gl_ModelViewProjectionMatrix",
    "gl_NormalMatrix", "gl_TexCoord", "gl_FogFragCoord", "gl_FrontColor", "gl_BackColor",
    "gl_ClipVertex", NULL
};

static const char* const k_rename[][2] = {
    { "texture2D", "texture" },         { "texture2DProj", "textureProj" },
    { "texture2DLod", "textureLod" },   { "texture2DProjLod", "textureProjLod" },
    { "texture3D", "texture" },         { "texture3DLod", "textureLod" },
    { "textureCube", "texture" },       { "textureCubeLod", "textureLod" },
    { "texture1D", "texture" },         { "texture1DLod", "textureLod" },
    { "texture1DArray", "texture" },    { "texture1DArrayLod", "textureLod" },
    { "texture2DRect", "texture" },     { "texture2DRectProj", "textureProj" },
    { "texture1DProj", "textureProj" }, { "texture1DProjLod", "textureProjLod" },
    { "texture3DProj", "textureProj" }, { "texture3DProjLod", "textureProjLod" },
    { NULL, NULL }
};

/* ---------- samplers que ES no tiene ---------- */
enum { K_1D, K_1DA, K_RECT, K_COUNT };
enum { V_F, V_I, V_U };

typedef struct { const char* from; const char* to; int kind, v; GLenum gl; } smp_t;
static const smp_t k_smp[] = {
    { "sampler1D",        "sampler2D",       K_1D,   V_F, GL_SAMPLER_1D },
    { "isampler1D",       "isampler2D",      K_1D,   V_I, GL_INT_SAMPLER_1D },
    { "usampler1D",       "usampler2D",      K_1D,   V_U, GL_UNSIGNED_INT_SAMPLER_1D },
    { "sampler1DArray",   "sampler2DArray",  K_1DA,  V_F, GL_SAMPLER_1D_ARRAY },
    { "isampler1DArray",  "isampler2DArray", K_1DA,  V_I, GL_INT_SAMPLER_1D_ARRAY },
    { "usampler1DArray",  "usampler2DArray", K_1DA,  V_U, GL_UNSIGNED_INT_SAMPLER_1D_ARRAY },
    { "sampler2DRect",    "sampler2D",       K_RECT, V_F, GL_SAMPLER_2D_RECT },
    { "isampler2DRect",   "isampler2D",      K_RECT, V_I, GL_INT_SAMPLER_2D_RECT },
    { "usampler2DRect",   "usampler2D",      K_RECT, V_U, GL_UNSIGNED_INT_SAMPLER_2D_RECT },
    { NULL, NULL, 0, 0, 0 }
};

/* Los samplers solo pueden ser uniforms (ambito global) o parametros de funcion: no hay samplers locales en GLSL. Un nombre
 * se resuelve como en GLSL: dentro del cuerpo de una funcion, un parametro con ese nombre esconde los uniforms globales. */
#define MAX_SMP_NAMES 64
#define MAX_FN_SCOPES 512
typedef struct { const char *pl, *pr, *bl, *br; } fnscope_t;     /* ( ) de los parametros y { } del cuerpo */
typedef struct { char name[64]; int kind, v; GLenum gl; int scope; } smpname_t;   /* scope: -1 global, si no indice de funcion */
typedef struct { smpname_t n[MAX_SMP_NAMES]; int cnt; const char* base; fnscope_t f[MAX_FN_SCOPES]; int nf; } smpnames_t;

static const smpname_t* smp_lookup_scope(const smpnames_t* t, const char* nm, size_t len, int scope)
{
    int i;
    for (i = 0; i < t->cnt; i++)
        if (t->n[i].scope == scope && strlen(t->n[i].name) == len && strncmp(t->n[i].name, nm, len) == 0) return &t->n[i];
    return NULL;
}

/* definiciones de funcion en ambito global: `... ( params ) {` hasta su `}` (structs y bloques no tienen ')' antes de '{') */
static void collect_fn_scopes(const char* s, smpnames_t* t)
{
    int depth = 0;
    const char* p;
    t->nf = 0;
    t->base = s;
    for (p = s; *p; p++) {
        if (p[0] == '/' && p[1] == '/') { while (p[1] && p[1] != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') { p += 2; while (*p && !(p[0] == '*' && p[1] == '/')) p++; if (!*p) break; p++; continue; }
        if (*p == '{') {
            if (depth == 0 && t->nf < MAX_FN_SCOPES) {
                const char* q = p;
                while (q > s && isspace((unsigned char)q[-1])) q--;
                if (q > s && q[-1] == ')') {
                    const char* r = q - 1;
                    int d = 0;
                    for (; r >= s; r--) {
                        if (*r == ')') d++;
                        else if (*r == '(' && --d == 0) break;
                    }
                    if (r >= s) {
                        t->f[t->nf].pl = r; t->f[t->nf].pr = q - 1; t->f[t->nf].bl = p; t->f[t->nf].br = NULL;
                        t->nf++;
                    }
                }
            }
            depth++;
        } else if (*p == '}') {
            if (depth > 0) depth--;
            if (depth == 0 && t->nf > 0 && !t->f[t->nf - 1].br) t->f[t->nf - 1].br = p;
        }
    }
}

/* funcion cuya lista de parametros contiene `q` (-1 ninguna) */
static int fn_of_param(const smpnames_t* t, const char* q)
{
    int i;
    for (i = 0; i < t->nf; i++) if (q > t->f[i].pl && q < t->f[i].pr) return i;
    return -1;
}

/* la lista de parametros [pl, pr] declara el identificador nm */
static int params_declare(const char* pl, const char* pr, const char* nm, size_t nl)
{
    const char* q;
    for (q = pl + 1; q + nl <= pr; q++)
        if (strncmp(q, nm, nl) == 0 && !is_id((unsigned char)q[-1]) && !is_id((unsigned char)q[nl])) return 1;
    return 0;
}

/* `nm` usado en la posicion `at`: el parametro de la funcion que lo contiene, si declara ese nombre, o el uniform global */
static const smpname_t* smp_resolve(const smpnames_t* t, const char* at, const char* nm, size_t nl)
{
    int i;
    for (i = 0; i < t->nf; i++)
        if (t->f[i].br && at > t->f[i].bl && at < t->f[i].br) {
            if (params_declare(t->f[i].pl, t->f[i].pr, nm, nl))
                return smp_lookup_scope(t, nm, nl, i);   /* NULL: el parametro es de un tipo que no se reescribe */
            break;
        }
    return smp_lookup_scope(t, nm, nl, -1);
}

static void skip_ws(const char** p) { while (**p && isspace((unsigned char)**p)) (*p)++; }

/* lee un identificador; devuelve su longitud (0 = no hay) */
static size_t read_id(const char** p, const char** start)
{
    const char* q = *p;
    skip_ws(&q);
    *start = q;
    while (is_id((unsigned char)*q)) q++;
    *p = q;
    return (size_t)(q - *start);
}

/* Recoge los nombres declarados con un tipo de sampler de desktop. 0 = demasiados */
static int collect_smp_names(const char* s, smpnames_t* t)
{
    int k;
    collect_fn_scopes(s, t);
    for (k = 0; k_smp[k].from; k++) {
        const char* p = s;
        const char* q;
        while ((q = find_word(p, k_smp[k].from)) != NULL) {
            const char* c = q + strlen(k_smp[k].from);
            const char* nm;
            size_t nl;
            p = c;
            for (;;) {
                nl = read_id(&c, &nm);
                if (!nl || nl >= sizeof t->n[0].name) break;
                int scope = fn_of_param(t, q);
                if (scope < 0) {   /* fuera de una definicion: uniform global, salvo parametro de un prototipo `f(sampler1D s);` */
                    const char* r = q;
                    while (r > s && r[-1] != ';' && r[-1] != '{' && r[-1] != '}' && r[-1] != '(') r--;
                    if (r > s && r[-1] == '(') break;
                }
                if (!smp_lookup_scope(t, nm, nl, scope)) {
                    if (t->cnt >= MAX_SMP_NAMES) return 0;
                    memcpy(t->n[t->cnt].name, nm, nl);
                    t->n[t->cnt].name[nl] = 0;
                    t->n[t->cnt].kind = k_smp[k].kind;
                    t->n[t->cnt].v = k_smp[k].v;
                    t->n[t->cnt].gl = k_smp[k].gl;
                    t->n[t->cnt].scope = scope;
                    t->cnt++;
                }
                skip_ws(&c);
                while (*c == '[') { while (*c && *c != ']') c++; if (*c) c++; skip_ws(&c); }
                if (*c != ',') break;
                {   /* otro nombre solo si lo que sigue a la coma es `ident` + , ; ) [ */
                    const char* d = c + 1;
                    const char* n2;
                    size_t l2 = read_id(&d, &n2);
                    skip_ws(&d);
                    if (!l2 || !(*d == ',' || *d == ';' || *d == ')' || *d == '[')) break;
                    c = c + 1;
                }
            }
        }
    }
    return 1;
}

static const char* const k_vsfx[] = { "f", "i", "u" };
static const char* const k_ksfx[] = { "1D", "1DA", "RECT" };

/* Reemplaza fn(NAME, ...) por lorica_fn_KIND_V(NAME, ...) cuando NAME es un sampler reescrito (1D -> 2D con y = 0.5,
 * 1DArray -> 2DArray, Rect -> 2D con coordenadas normalizadas). Las funciones cuyo offset debe ser una expresion constante
 * (familia *Offset) se traducen con macros, que conservan la constancia; su nombre lleva el numero de argumentos de la
 * llamada (lorica_fn_KIND_V_N). El resto son funciones auxiliares sobrecargadas por tipo de coordenada (vec2/vec4 en Proj).
 * `used` marca, por (fn, kind, v), los numeros de argumentos vistos (bit N; bit 0 = funcion). Devuelve NULL si hay un
 * error (msg en *bad) o sin memoria (bad == NULL). */
enum { F_TEX, F_LOD, F_FETCH, F_SIZE, F_PROJ, F_PROJLOD, F_GRAD,
       F_OFFSET, F_LODOFFSET, F_FETCHOFFSET, F_GRADOFFSET, F_COUNT };
#define F_FIRST_MACRO F_OFFSET
static const char* const k_fn[F_COUNT] = { "texture", "textureLod", "texelFetch", "textureSize", "textureProj", "textureProjLod",
                                           "textureGrad", "textureOffset", "textureLodOffset", "texelFetchOffset", "textureGradOffset" };
/* sin traduccion: no existen sobre 1D/Rect en GLSL, o necesitarian el divisor de la proyeccion dentro de una macro */
static const char* const k_fn_unsup[] = { "textureProjOffset", "textureProjLodOffset", "textureProjGrad", "textureProjGradOffset",
                                          "textureGather", "textureGatherOffset", "textureGatherOffsets", NULL };

/* Traducciones. $R tipo de retorno (vec4/ivec4/uvec4), $S tipo del sampler ES, $N nombre del auxiliar.
 * nargs = 0: funcion (se emite si la llamada se usa); nargs > 0: macro para ese numero de argumentos.
 * frag = 1: solo en fragment shaders (formas con bias). */
typedef struct { int f, k, nargs, frag; const char* text; } smp_helper_t;
static const smp_helper_t k_helpers[] = {
    { F_TEX,   K_1D,   0, 0, "$R $N($S s,float x){return texture(s,vec2(x,0.5));} " },
    { F_TEX,   K_1D,   0, 1, "$R $N($S s,float x,float b){return texture(s,vec2(x,0.5),b);} " },
    { F_TEX,   K_1DA,  0, 0, "$R $N($S s,vec2 c){return texture(s,vec3(c.x,0.5,c.y));} " },
    { F_TEX,   K_1DA,  0, 1, "$R $N($S s,vec2 c,float b){return texture(s,vec3(c.x,0.5,c.y),b);} " },
    { F_TEX,   K_RECT, 0, 0, "$R $N($S s,vec2 p){return texture(s,p/vec2(textureSize(s,0)));} " },
    { F_LOD,   K_1D,   0, 0, "$R $N($S s,float x,float l){return textureLod(s,vec2(x,0.5),l);} " },
    { F_LOD,   K_1DA,  0, 0, "$R $N($S s,vec2 c,float l){return textureLod(s,vec3(c.x,0.5,c.y),l);} " },
    { F_FETCH, K_1D,   0, 0, "$R $N($S s,int p,int l){return texelFetch(s,ivec2(p,0),l);} " },
    { F_FETCH, K_1DA,  0, 0, "$R $N($S s,ivec2 p,int l){return texelFetch(s,ivec3(p.x,0,p.y),l);} " },
    { F_FETCH, K_RECT, 0, 0, "$R $N($S s,ivec2 p){return texelFetch(s,p,0);} " },
    { F_SIZE,  K_1D,   0, 0, "int $N($S s,int l){return textureSize(s,l).x;} " },
    { F_SIZE,  K_1DA,  0, 0, "ivec2 $N($S s,int l){ivec3 z=textureSize(s,l);return ivec2(z.x,z.z);} " },
    { F_SIZE,  K_RECT, 0, 0, "ivec2 $N($S s){return textureSize(s,0);} " },
    { F_PROJ,  K_1D,   0, 0, "$R $N($S s,vec2 p){return texture(s,vec2(p.x/p.y,0.5));} "
                             "$R $N($S s,vec4 p){return texture(s,vec2(p.x/p.w,0.5));} " },
    { F_PROJ,  K_1D,   0, 1, "$R $N($S s,vec2 p,float b){return texture(s,vec2(p.x/p.y,0.5),b);} "
                             "$R $N($S s,vec4 p,float b){return texture(s,vec2(p.x/p.w,0.5),b);} " },
    { F_PROJ,  K_RECT, 0, 0, "$R $N($S s,vec3 p){return texture(s,(p.xy/p.z)/vec2(textureSize(s,0)));} "
                             "$R $N($S s,vec4 p){return texture(s,(p.xy/p.w)/vec2(textureSize(s,0)));} " },
    { F_PROJLOD, K_1D, 0, 0, "$R $N($S s,vec2 p,float l){return textureLod(s,vec2(p.x/p.y,0.5),l);} "
                             "$R $N($S s,vec4 p,float l){return textureLod(s,vec2(p.x/p.w,0.5),l);} " },
    { F_GRAD,  K_1D,   0, 0, "$R $N($S s,float x,float dx,float dy){return textureGrad(s,vec2(x,0.5),vec2(dx,0.0),vec2(dy,0.0));} " },
    { F_GRAD,  K_1DA,  0, 0, "$R $N($S s,vec2 c,float dx,float dy){return textureGrad(s,vec3(c.x,0.5,c.y),vec2(dx,0.0),vec2(dy,0.0));} " },
    { F_GRAD,  K_RECT, 0, 0, "$R $N($S s,vec2 p,vec2 dx,vec2 dy){vec2 z=vec2(textureSize(s,0));return textureGrad(s,p/z,dx/z,dy/z);} " },
    { F_OFFSET, K_1D,  3, 0, "#define $N(s,x,o) textureOffset(s,vec2(x,0.5),ivec2(o,0))" },
    { F_OFFSET, K_1D,  4, 1, "#define $N(s,x,o,b) textureOffset(s,vec2(x,0.5),ivec2(o,0),b)" },
    { F_OFFSET, K_1DA, 3, 0, "#define $N(s,c,o) textureOffset(s,vec3((c).x,0.5,(c).y),ivec2(o,0))" },
    { F_OFFSET, K_1DA, 4, 1, "#define $N(s,c,o,b) textureOffset(s,vec3((c).x,0.5,(c).y),ivec2(o,0),b)" },
    { F_OFFSET, K_RECT, 3, 0, "#define $N(s,p,o) textureOffset(s,(p)/vec2(textureSize(s,0)),o)" },
    { F_LODOFFSET, K_1D,  4, 0, "#define $N(s,x,l,o) textureLodOffset(s,vec2(x,0.5),l,ivec2(o,0))" },
    { F_LODOFFSET, K_1DA, 4, 0, "#define $N(s,c,l,o) textureLodOffset(s,vec3((c).x,0.5,(c).y),l,ivec2(o,0))" },
    { F_FETCHOFFSET, K_1D,   4, 0, "#define $N(s,p,l,o) texelFetchOffset(s,ivec2(p,0),l,ivec2(o,0))" },
    { F_FETCHOFFSET, K_1DA,  4, 0, "#define $N(s,p,l,o) texelFetchOffset(s,ivec3((p).x,0,(p).y),l,ivec2(o,0))" },
    { F_FETCHOFFSET, K_RECT, 3, 0, "#define $N(s,p,o) texelFetchOffset(s,p,0,o)" },
    { F_GRADOFFSET, K_1D,  5, 0, "#define $N(s,x,dx,dy,o) textureGradOffset(s,vec2(x,0.5),vec2(dx,0.0),vec2(dy,0.0),ivec2(o,0))" },
    { F_GRADOFFSET, K_1DA, 5, 0, "#define $N(s,c,dx,dy,o) textureGradOffset(s,vec3((c).x,0.5,(c).y),vec2(dx,0.0),vec2(dy,0.0),ivec2(o,0))" },
    { F_GRADOFFSET, K_RECT, 5, 0, "#define $N(s,p,dx,dy,o) textureGradOffset(s,(p)/vec2(textureSize(s,0)),(dx)/vec2(textureSize(s,0)),(dy)/vec2(textureSize(s,0)),o)" },
    { -1, 0, 0, 0, NULL }
};

static int has_macro_helper(int f, int k, int nargs)
{
    int i;
    for (i = 0; k_helpers[i].text; i++)
        if (k_helpers[i].f == f && k_helpers[i].k == k && k_helpers[i].nargs == nargs) return 1;
    return 0;
}

/* numero de argumentos de una llamada; `c` apunta justo despues de su '(' (comas al nivel 0 hasta el ')' que la cierra).
 * 0 si no hay argumentos o la llamada no cierra */
static int call_nargs(const char* c)
{
    int depth = 0, n = 1;
    skip_ws(&c);
    if (*c == ')') return 0;
    for (; *c; c++) {
        if (*c == '(' || *c == '[') depth++;
        else if (*c == ')' || *c == ']') { if (depth == 0) return n; depth--; }
        else if (*c == ',' && depth == 0) n++;
    }
    return 0;
}

static char* rewrite_calls(char* s, const smpnames_t* t, unsigned char used[F_COUNT][K_COUNT][3],
                           const char** bad)
{
    sb_t b = {0, 0, 0, 0};
    const char* p = s;
    int f;
    *bad = NULL;
    if (!t->cnt) return s;
    /* funciones sin traduccion sobre samplers reescritos */
    for (f = 0; k_fn_unsup[f]; f++) {
        const char* q = s;
        while ((q = find_word(q, k_fn_unsup[f])) != NULL) {
            const char* c = q + strlen(k_fn_unsup[f]);
            const char* nm;
            size_t nl;
            q = c;
            skip_ws(&c);
            if (*c != '(') continue;
            c++;
            nl = read_id(&c, &nm);
            if (nl && smp_resolve(t, q, nm, nl)) { *bad = k_fn_unsup[f]; free(s); return NULL; }
        }
    }
    while (*p) {
        /* siguiente aparicion de cualquiera de las funciones traducibles */
        const char* best = NULL;
        int bf = -1;
        for (f = 0; f < F_COUNT; f++) {
            const char* q = find_word(p, k_fn[f]);
            if (q && (!best || q < best)) { best = q; bf = f; }
        }
        if (!best) break;
        {
            const char* c = best + strlen(k_fn[bf]);
            const char* nm;
            size_t nl;
            const smpname_t* e = NULL;
            int nargs = 0;
            skip_ws(&c);
            if (*c == '(') {
                c++;
                nargs = call_nargs(c);
                nl = read_id(&c, &nm);
                if (nl) e = smp_resolve(t, best, nm, nl);
            }
            sb_cat(&b, p, (size_t)(best - p));
            if (e) {
                char nmb[64];
                if (bf >= F_FIRST_MACRO) {
                    if (nargs <= 0 || nargs > 7 || !has_macro_helper(bf, e->kind, nargs)) {
                        *bad = k_fn[bf]; free(b.p); free(s); return NULL;
                    }
                    snprintf(nmb, sizeof nmb, "lorica_%s_%s_%s_%d", k_fn[bf], k_ksfx[e->kind], k_vsfx[e->v], nargs);
                    used[bf][e->kind][e->v] |= (unsigned char)(1u << nargs);
                } else {
                    snprintf(nmb, sizeof nmb, "lorica_%s_%s_%s", k_fn[bf], k_ksfx[e->kind], k_vsfx[e->v]);
                    used[bf][e->kind][e->v] |= 1u;
                }
                sb_str(&b, nmb);
            } else {
                sb_str(&b, k_fn[bf]);
            }
            p = best + strlen(k_fn[bf]);
        }
    }
    sb_str(&b, p);
    free(s);
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

/* texto de las funciones / macros auxiliares usadas */
static void emit_helpers(sb_t* o, unsigned char used[F_COUNT][K_COUNT][3], int frag)
{
    static const char* const pre[3]  = { "", "i", "u" };
    static const char* const rty[3]  = { "vec4", "ivec4", "uvec4" };
    int i, v;
    for (v = 0; v < 3; v++)
        for (i = 0; k_helpers[i].text; i++) {
            const smp_helper_t* h = &k_helpers[i];
            char name[64], sty[48];
            const char* q;
            if (h->frag && !frag) continue;
            if (!(used[h->f][h->k][v] & (1u << h->nargs))) continue;
            if (h->nargs) {
                snprintf(name, sizeof name, "lorica_%s_%s_%s_%d", k_fn[h->f], k_ksfx[h->k], k_vsfx[v], h->nargs);
                sb_str(o, "\n");            /* una directiva empieza al inicio de una linea */
            } else
                snprintf(name, sizeof name, "lorica_%s_%s_%s", k_fn[h->f], k_ksfx[h->k], k_vsfx[v]);
            snprintf(sty, sizeof sty, "highp %s%s", pre[v], h->k == K_1DA ? "sampler2DArray" : "sampler2D");
            for (q = h->text; *q; q++) {
                if (q[0] == '$' && q[1] == 'R')      { sb_str(o, rty[v]); q++; }
                else if (q[0] == '$' && q[1] == 'S') { sb_str(o, sty); q++; }
                else if (q[0] == '$' && q[1] == 'N') { sb_str(o, name); q++; }
                else sb_cat(o, q, 1);
            }
            if (h->nargs) sb_str(o, "\n");
        }
}

/* ---------- salidas del fragment shader: layout(location=N) segun glBindFragDataLocation ---------- */
static const gl31_fragbind_t* find_bind(const gl31_fragbind_t* b, int n, const char* nm, size_t len)
{
    int i;
    for (i = 0; i < n; i++)
        if (b[i].name && strlen(b[i].name) == len && strncmp(b[i].name, nm, len) == 0) return &b[i];
    return NULL;
}

static char* inject_frag_locations(char* s, const gl31_fragbind_t* binds, int nbinds)
{
    sb_t b = {0, 0, 0, 0};
    const char* p = s;
    const char* q;
    if (!binds || nbinds <= 0) return s;
    while ((q = find_word(p, "out")) != NULL) {
        /* inicio de la sentencia: tras el ultimo ; { } anterior */
        const char* st = q;
        const gl31_fragbind_t* bd = NULL;
        int has_layout = 0;
        const char* c;
        while (st > p && st[-1] != ';' && st[-1] != '{' && st[-1] != '}') st--;
        {
            const char* w;
            for (w = st; w < q; w++)
                if (strncmp(w, "layout", 6) == 0 && (w == st || !is_id((unsigned char)w[-1]))) has_layout = 1;
        }
        /* despues de `out`: [calificadores] tipo nombre[...] */
        c = q + 3;
        {
            const char* w; size_t n; int tok = 0; const char* last = NULL; size_t lastn = 0;
            for (;;) {
                n = read_id(&c, &w);
                if (!n) break;
                tok++; last = w; lastn = n;
                skip_ws(&c);
                if (*c == '[') { while (*c && *c != ']') c++; if (*c) c++; skip_ws(&c); }
                if (*c == ';' || *c == ',' || *c == '{') break;
                if (tok > 6) { last = NULL; break; }
            }
            if (last && tok >= 2 && *c != '{') bd = find_bind(binds, nbinds, last, lastn);
        }
        sb_cat(&b, p, (size_t)(q - p));
        if (bd && !has_layout) {
            char tmp[48];
            if (bd->index) snprintf(tmp, sizeof tmp, "layout(location = %u, index = %u) ", bd->location, bd->index);
            else           snprintf(tmp, sizeof tmp, "layout(location = %u) ", bd->location);
            sb_str(&b, tmp);
        }
        sb_str(&b, "out");
        p = q + 3;
    }
    sb_str(&b, p);
    free(s);
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

/* precision por defecto: solo los tipos que el backend tiene */
static void emit_precisions(sb_t* o)
{
    sb_str(o, "precision highp float; precision highp int; ");
    sb_str(o, "precision highp sampler2D; precision highp samplerCube; precision highp sampler3D; "
              "precision highp sampler2DArray; precision highp sampler2DShadow; "
              "precision highp samplerCubeShadow; precision highp sampler2DArrayShadow; "
              "precision highp isampler2D; precision highp isampler3D; precision highp isamplerCube; "
              "precision highp isampler2DArray; precision highp usampler2D; precision highp usampler3D; "
              "precision highp usamplerCube; precision highp usampler2DArray; ");
    if (gl31_caps.multisample_tex)
        sb_str(o, "precision highp sampler2DMS; precision highp isampler2DMS; precision highp usampler2DMS; ");
    if (gl31_caps.ms_array)
        sb_str(o, "precision highp sampler2DMSArray; precision highp isampler2DMSArray; "
                  "precision highp usampler2DMSArray; ");
    if (gl31_caps.cube_array)
        sb_str(o, "precision highp samplerCubeArray; precision highp isamplerCubeArray; "
                  "precision highp usamplerCubeArray; precision highp samplerCubeArrayShadow; ");
    if (gl31_caps.es31)
        sb_str(o, "precision highp image2D; precision highp iimage2D; precision highp uimage2D; "
                  "precision highp image3D; precision highp iimage3D; precision highp uimage3D; "
                  "precision highp imageCube; precision highp iimageCube; precision highp uimageCube; "
                  "precision highp image2DArray; precision highp iimage2DArray; precision highp uimage2DArray; ");
    if (gl31_caps.tex_buffer)
        sb_str(o, "precision highp samplerBuffer; precision highp isamplerBuffer; "
                  "precision highp usamplerBuffer; ");
}

static int is_stage(GLenum t)
{
    return t == GL_VERTEX_SHADER || t == GL_FRAGMENT_SHADER ||
           (t == GL_GEOMETRY_SHADER && gl31_caps.geometry) ||
           (t == GL_COMPUTE_SHADER && gl31_caps.es31) ||
           ((t == GL_TESS_CONTROL_SHADER || t == GL_TESS_EVALUATION_SHADER) && gl31_caps.tess);
}

/* gl_PerVertex redeclarado (GLSL de escritorio, programas separables / tessellation): ES lo tiene implicito */
static void strip_pervertex(char* s)
{
    const char* q = s;
    while ((q = find_word(q, "gl_PerVertex")) != NULL) {
        char* a = s + (q - s);
        char* st = a;
        char* e;
        int depth = 0;
        {   /* retroceder sobre espacios y un calificador in / out */
            char* t = a;
            while (t > s && isspace((unsigned char)t[-1])) t--;
            if (t - s >= 3 && strncmp(t - 3, "out", 3) == 0 && (t - 3 == s || !is_id((unsigned char)t[-4]))) st = t - 3;
            else if (t - s >= 2 && strncmp(t - 2, "in", 2) == 0 && (t - 2 == s || !is_id((unsigned char)t[-3]))) st = t - 2;
        }
        e = a;
        while (*e && *e != '{' && *e != ';') e++;
        if (*e == '{') {
            for (; *e; e++) {
                if (*e == '{') depth++;
                else if (*e == '}' && --depth == 0) { e++; break; }
            }
            while (*e && *e != ';') e++;
        }
        if (*e == ';') e++;
        for (; st < e; st++) if (*st != '\n') *st = ' ';
        q = e;
    }
}

/* double / dvecN / dmatN -> float / vecN / matN (ES no tiene fp64: se pierde precision) */
static char* demote_doubles(char* s, int* did)
{
    static const char* const from[] = { "double", "dvec2", "dvec3", "dvec4", "dmat2", "dmat3", "dmat4",
                                        "dmat2x2", "dmat2x3", "dmat2x4", "dmat3x2", "dmat3x3", "dmat3x4",
                                        "dmat4x2", "dmat4x3", "dmat4x4", NULL };
    static const char* const to[]   = { "float", "vec2", "vec3", "vec4", "mat2", "mat3", "mat4",
                                        "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4",
                                        "mat4x2", "mat4x3", "mat4x4" };
    int i;
    size_t k, n;
    *did = 0;
    for (i = 0; from[i]; i++)
        if (find_word(s, from[i])) {
            *did = 1;
            if (!(s = repl_word(s, from[i], to[i]))) return NULL;
        }
    if (!*did) return s;
    /* sufijos de literal lf / LF */
    n = strlen(s);
    for (k = 1; k + 1 < n; k++) {
        if ((s[k] == 'l' || s[k] == 'L') && (s[k + 1] == 'f' || s[k + 1] == 'F') &&
            (isdigit((unsigned char)s[k - 1]) || s[k - 1] == '.') && !is_id((unsigned char)s[k + 2])) {
            size_t j = k;
            while (j > 0 && (isalnum((unsigned char)s[j - 1]) || s[j - 1] == '.' || s[j - 1] == '_')) j--;
            if (!isdigit((unsigned char)s[j]) && s[j] != '.') continue;
            s[k] = ' '; s[k + 1] = ' ';
        }
    }
    return s;
}

/* Devuelve memoria malloc'd (free) o NULL y rellena err. */

/* ---------- uniforms propios y wrapper de main() (ver gl31.h) ---------- */
static GL31_TLS gl31_conv_opts_t g_opts;
static GL31_TLS gl31_conv_info_t g_info;
void gl31_glsl_set_opts(const gl31_conv_opts_t* o) { if (o) g_opts = *o; else memset(&g_opts, 0, sizeof g_opts); }
void gl31_glsl_get_info(gl31_conv_info_t* out) { if (out) *out = g_info; }

/* ---------- inicializadores globales no constantes ---------- */
/* GLSL de escritorio (1.20+) admite `vec4 v = vec4(gl_VertexID, ...);` o con uniforms en el ambito global; ES exige una
 * expresion constante (S0012). wined3d las genera (vs_in0 con gl_VertexID - base_vertex_id). Se parte la declaracion:
 * `vec4 v;` queda donde estaba y la asignacion pasa al principio de main(), en el mismo orden. Solo se mueven las que
 * mencionan un gl_* o un uniform (las demas pueden ser constantes y siguen intactas). */
#define HOIST_MAXU 256
static int hoist_has_word(const char* b, const char* e, const char* w)
{
    size_t n = strlen(w);
    const char* p = b;
    while (p + n <= e) {
        if (strncmp(p, w, n) == 0 && (p == b || !is_id((unsigned char)p[-1])) && (p + n == e || !is_id((unsigned char)p[n])))
            return 1;
        p++;
    }
    return 0;
}

static int hoist_has_glprefix(const char* b, const char* e)
{
    const char* p = b;
    while (p + 3 <= e) {
        if (strncmp(p, "gl_", 3) == 0 && (p == b || !is_id((unsigned char)p[-1]))) return 1;
        p++;
    }
    return 0;
}

static char* hoist_global_inits(char* s)
{
    static const char* const skipkw[] = { "const", "uniform", "in", "out", "layout", "struct", "typedef", "precision",
        "invariant", "buffer", "shared", "flat", "smooth", "centroid", "attribute", "varying", "patch", "sample",
        "coherent", "volatile", "restrict", "readonly", "writeonly", NULL };
    char (*un)[64] = NULL;
    int nu = 0, any = 0;
    sb_t out = {0, 0, 0, 0}, asg = {0, 0, 0, 0};
    const char *p = s, *st = s, *last = s;
    int depth = 0;
    un = (char (*)[64])malloc((size_t)HOIST_MAXU * 64);
    if (!un) return s;
    while (*p) {
        if (*p == '#' ) {                       /* directiva: hasta fin de linea */
            const char* ls = p;
            while (ls > s && (ls[-1] == ' ' || ls[-1] == '\t')) ls--;
            if (ls == s || ls[-1] == '\n') { while (*p && *p != '\n') p++; st = *p ? p + 1 : p; continue; }
        }
        if (*p == '{') { depth++; p++; continue; }
        if (*p == '}') { if (depth > 0) depth--; p++; if (depth == 0) st = p; continue; }
        if (*p == ';' && depth == 0) {
            const char *b = st, *e = p, *eq = NULL, *q;
            int pd = 0, first_ok = 1;
            while (b < e && isspace((unsigned char)*b)) b++;
            /* primera palabra (despues de un `layout(...)` si lo hay) */
            {
                const char* b2 = b; const char* w; size_t wl;
                if (e - b2 > 6 && !strncmp(b2, "layout", 6) && !is_id((unsigned char)b2[6])) {
                    int dd = 0;
                    b2 += 6;
                    while (b2 < e) { if (*b2 == '(') dd++; else if (*b2 == ')' && --dd == 0) { b2++; break; } b2++; }
                    while (b2 < e && isspace((unsigned char)*b2)) b2++;
                }
                w = b2;
                while (w < e && is_id((unsigned char)*w)) w++;
                wl = (size_t)(w - b2);
                if (wl && ((wl == 7 && !strncmp(b2, "uniform", 7)) || (wl == 2 && !strncmp(b2, "in", 2))) &&
                    !memchr(b2, '{', (size_t)(e - b2))) {
                    /* registrar los nombres de los uniforms: cada declarador, sin `[n]` ni `= valor` */
                    const char* ds = w; const char* r; pd = 0;
                    for (q = w; q <= e; q++) {
                        char ch = q < e ? *q : ',';
                        if (ch == '[' || ch == '(') pd++;
                        else if (ch == ']' || ch == ')') pd--;
                        if (pd == 0 && ch == ',') {
                            const char* de = q; const char* nb;
                            for (r = ds; r < q; r++) if (*r == '[' || *r == '=') { de = r; break; }
                            while (de > ds && isspace((unsigned char)de[-1])) de--;
                            nb = de;
                            while (nb > ds && is_id((unsigned char)nb[-1])) nb--;
                            if (de > nb && (size_t)(de - nb) < 64 && nu < HOIST_MAXU) {
                                memcpy(un[nu], nb, (size_t)(de - nb)); un[nu][de - nb] = 0; nu++;
                            }
                            ds = q + 1;
                        }
                    }
                    first_ok = 0;
                } else {
                    int k;
                    for (k = 0; skipkw[k]; k++)
                        if (strlen(skipkw[k]) == wl && !strncmp(skipkw[k], b2, wl)) { first_ok = 0; break; }
                }
            }
            if (first_ok) {
                pd = 0;
                for (q = b; q < e; q++) {
                    if (*q == '(' || *q == '[') pd++;
                    else if (*q == ')' || *q == ']') pd--;
                    else if (*q == '=' && pd == 0 && q[1] != '=' && q > b && !strchr("<>!=", q[-1])) { eq = q; break; }
                    else if (*q == '(' ) break;
                }
                if (eq) {
                    const char* decl_end = eq;
                    int has_comma = 0, has_br = 0, has_paren = 0, nonconst, k;
                    while (decl_end > b && isspace((unsigned char)decl_end[-1])) decl_end--;
                    for (q = b; q < decl_end; q++) { if (*q == ',') has_comma = 1; if (*q == '[') has_br = 1; if (*q == '(') has_paren = 1; }
                    nonconst = hoist_has_glprefix(eq + 1, e);
                    for (k = 0; !nonconst && k < nu; k++) if (hoist_has_word(eq + 1, e, un[k])) nonconst = 1;
                    if (!has_comma && !has_br && !has_paren && nonconst) {
                        const char* ne = decl_end; const char* nb = ne;
                        const char* ib = eq + 1;
                        while (nb > b && is_id((unsigned char)nb[-1])) nb--;
                        while (ib < e && isspace((unsigned char)*ib)) ib++;
                        sb_cat(&out, last, (size_t)(decl_end - last));   /* hasta el final de `TIPO nombre` */
                        last = e;                                         /* se omite `= inicializador` */
                        sb_str(&asg, "    "); sb_cat(&asg, nb, (size_t)(ne - nb)); sb_str(&asg, " = ");
                        sb_cat(&asg, ib, (size_t)(e - ib)); sb_str(&asg, ";\n");
                        any = 1;
                    }
                }
            }
            p++; st = p; continue;
        }
        p++;
    }
    free(un);
    if (!any) { free(out.p); free(asg.p); return s; }
    sb_str(&out, last);
    /* insertar al principio de main() */
    {
        char* m = out.p; char* hit = NULL; const char* w = m;
        while ((w = find_word(w, "main")) != NULL) {
            const char* q = w + 4; const char* bb = w;
            while (bb > m && isspace((unsigned char)bb[-1])) bb--;
            while (isspace((unsigned char)*q)) q++;
            if (bb - m >= 4 && !strncmp(bb - 4, "void", 4) && *q == '(') { hit = (char*)w; break; }
            w += 4;
        }
        if (hit) {
            char* br = strchr(hit, '{');
            if (br) {
                sb_t o2 = {0, 0, 0, 0};
                sb_cat(&o2, out.p, (size_t)(br + 1 - out.p));
                sb_str(&o2, "\n"); sb_cat(&o2, asg.p, asg.n);
                sb_str(&o2, br + 1);
                free(out.p); free(asg.p); free(s);
                if (o2.oom) { free(o2.p); return NULL; }
                return o2.p;
            }
        }
    }
    free(out.p); free(asg.p);
    return s;     /* sin main(): se deja como estaba */
}

/* Renombra todas las definiciones `void main(` a `void lorica_main(`. *found = 0 si no existe.
 * Todas, no solo la primera: un shader de fuente unica puede tener un main por rama #if/#ifdef. */
static char* rename_main(char* s, int* found)
{
    const char* p = s;
    sb_t o = {0, 0, 0, 0};
    const char* last = s;       /* inicio del tramo aun no copiado */
    *found = 0;
    while ((p = find_word(p, "main")) != NULL) {
        const char* b = p; const char* q = p + 4;
        while (b > s && isspace((unsigned char)b[-1])) b--;
        while (isspace((unsigned char)*q)) q++;
        if (b - s >= 4 && strncmp(b - 4, "void", 4) == 0 && (b - 4 == s || !is_id((unsigned char)b[-5])) && *q == '(') {
            sb_cat(&o, last, (size_t)(p - last));
            sb_str(&o, "lorica_main");
            last = p + 4;
            *found = 1;
        }
        p += 4;
    }
    if (!*found) return s;
    sb_str(&o, last);
    free(s);
    if (o.oom) { free(o.p); *found = 0; return NULL; }
    return o.p;
}

/* Cuantas clip distances usa el VS (1..8). Una redeclaracion con tamano cuenta como tamano. */
static int clipdist_count(const char* s)
{
    static const char w[] = "gl_ClipDistance";
    size_t L = sizeof w - 1;
    const char* p = s; int n = 0;
    while ((p = find_word(p, w)) != NULL) {
        const char* b = p; const char* q = p + L; int decl = 0;
        while (b > s && isspace((unsigned char)b[-1])) b--;
        if (b - s >= 5 && strncmp(b - 5, "float", 5) == 0 && (b - 5 == s || !is_id((unsigned char)b[-6]))) decl = 1;
        while (isspace((unsigned char)*q)) q++;
        if (*q == '[') {
            q++;
            while (isspace((unsigned char)*q)) q++;
            if (isdigit((unsigned char)*q)) { int v = atoi(q); int need = decl ? v : v + 1; if (need > n) n = need; }
            else if (!decl) n = 8;                 /* indice no constante */
        } else n = 8;
        p += L;
    }
    if (n < 1) n = 8;
    return n > 8 ? 8 : n;
}

/* Borra (con espacios) la redeclaracion `[calificadores] float lorica_cd[...];` del usuario */
static void strip_cd_decl(char* s)
{
    char* p = s;
    while ((p = (char*)find_word(p, "lorica_cd")) != NULL) {
        char* b = p; char* q = p + 9;
        while (b > s && isspace((unsigned char)b[-1])) b--;
        if (b - s >= 5 && strncmp(b - 5, "float", 5) == 0 && (b - 5 == s || !is_id((unsigned char)b[-6]))) {
            char* st = b - 5;
            for (;;) {      /* retroceder sobre los calificadores */
                char* t = st; size_t k;
                static const char* const quals[] = { "out", "highp", "mediump", "lowp", "smooth", "flat", "centroid", "invariant", NULL };
                int hit = 0;
                while (t > s && isspace((unsigned char)t[-1])) t--;
                for (k = 0; quals[k]; k++) {
                    size_t ql = strlen(quals[k]);
                    if (t - s >= (long)ql && strncmp(t - ql, quals[k], ql) == 0 && (t - ql == s || !is_id((unsigned char)t[-(long)ql - 1]))) { st = t - ql; hit = 1; break; }
                }
                if (!hit) break;
            }
            while (*q && *q != ';') q++;
            for (; st <= q && *st; st++) if (*st != '\n') *st = ' ';
            p = q;
        } else p += 9;
    }
}

/* Aplica las reescrituras de uniforms propios. hdr va antes del codigo; tail, despues.
 * Devuelve 0 solo si no hay memoria. */
static int lu_transform(char** ps, GLenum type, const gl31_conv_opts_t* op, sb_t* hdr, sb_t* tail)
{
    char* s = *ps; char buf[256]; int found = 0;
    s = hoist_global_inits(s);
    if (!s) { *ps = NULL; return 0; }
    *ps = s;
    if (type == GL_VERTEX_SHADER && op->last_vs) {
        int wrote_ps = find_word(s, "gl_PointSize") != NULL;
        int cdn = 0, gate_n = 0;
        if (!gl31_caps.clip_distance && find_word(s, "gl_ClipDistance")) {
            cdn = clipdist_count(s);
            if (!(s = repl_word(s, "gl_ClipDistance", "lorica_cd"))) { *ps = NULL; return 0; }
            strip_cd_decl(s);
            snprintf(buf, sizeof buf, "out highp float lorica_cd[%d];\n", cdn);
            sb_str(hdr, buf);
        }
        /* Con EXT_clip_cull_distance el backend recorta siempre lo que escribe el shader; en GL de
         * escritorio solo si GL_CLIP_DISTANCEi esta habilitado: el wrapper neutraliza las no habilitadas. */
        if (gl31_caps.clip_distance && find_word(s, "gl_ClipDistance")) gate_n = clipdist_count(s);
        s = rename_main(s, &found);
        if (!s) { *ps = NULL; return 0; }
        if (found) {
            sb_str(hdr, "uniform highp vec4 lorica_vs; uniform highp vec4 lorica_vs2;\n");
            sb_str(tail, "\nvoid main() {\n    lorica_main();\n"
                         "    if (lorica_vs.z > 0.5) gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n"
                         "    if (lorica_vs2.x > 0.5 && gl_Position.w > 0.0) gl_Position.z = clamp(gl_Position.z, -gl_Position.w, gl_Position.w);\n"
                         "    if (lorica_vs.w > 0.5) gl_Position.y = -gl_Position.y;\n");
            sb_str(tail, wrote_ps ? "    if (lorica_vs.y > 0.5) gl_PointSize = lorica_vs.x;\n"
                                  : "    gl_PointSize = lorica_vs.x > 0.0 ? lorica_vs.x : 1.0;\n");
            if (gate_n > 0) {
                int gi;
                sb_str(tail, "    int lorica_m = int(lorica_vs2.y + 0.5);\n");
                for (gi = 0; gi < gate_n; gi++) {
                    snprintf(buf, sizeof buf, "    if (((lorica_m >> %d) & 1) == 0) gl_ClipDistance[%d] = 1.0;\n", gi, gi);
                    sb_str(tail, buf);
                }
            }
            sb_str(tail, "}\n");
            g_info.lu = 1;
        }
        g_info.cd_n = cdn;
    } else if (type == GL_FRAGMENT_SHADER) {
        int pc = find_word(s, "gl_PointCoord") != NULL;
        int n = op->fs_cd;
        if (pc || n > 0) { sb_str(hdr, "uniform highp vec4 lorica_fs;\n"); g_info.lu = 1; }
        if (pc) {
            sb_str(hdr, "highp vec2 lorica_pc() { return lorica_fs.x > 0.5 ? vec2(gl_PointCoord.x, 1.0 - gl_PointCoord.y) : gl_PointCoord; }\n");
            if (!(s = repl_word(s, "gl_PointCoord", "lorica_pc()"))) { *ps = NULL; return 0; }
        }
        if (n > 0) {
            int i;
            snprintf(buf, sizeof buf, "in highp float lorica_cd[%d];\n", n);
            sb_str(hdr, buf);
            s = rename_main(s, &found);
            if (!s) { *ps = NULL; return 0; }
            if (found) {
                sb_str(tail, "\nvoid main() {\n    int lorica_m = int(lorica_fs.y + 0.5);\n");
                for (i = 0; i < n; i++) {
                    snprintf(buf, sizeof buf, "    if (((lorica_m >> %d) & 1) != 0 && lorica_cd[%d] < 0.0) discard;\n", i, i);
                    sb_str(tail, buf);
                }
                sb_str(tail, "    lorica_main();\n}\n");
            }
        }
    }
    *ps = s;
    return 1;
}

/* ---- SSBO sin layout(binding): binding unico para poder resolver glShaderStorageBlockBinding ---- */
static int ssbo_insert(char** ps, size_t pos, const char* txt)
{
    size_t n = strlen(*ps), t = strlen(txt);
    char* p = (char*)realloc(*ps, n + t + 1);
    if (!p) return 0;
    memmove(p + pos + t, p + pos, n - pos + 1);
    memcpy(p + pos, txt, t);
    *ps = p;
    return 1;
}

static char* assign_ssbo_bindings(char* s, gl31_conv_info_t* info)
{
    unsigned used = 0;
    size_t i, n;
    int pass;
    for (pass = 0; pass < 2; pass++) {
        n = strlen(s);
        for (i = 0; i < n; ) {
            size_t b, e, j, q;
            if (!(is_id((unsigned char)s[i]) && (i == 0 || !is_id((unsigned char)s[i - 1])))) { i++; continue; }
            b = i;
            while (i < n && is_id((unsigned char)s[i])) i++;
            if (i - b != 6 || memcmp(s + b, "buffer", 6) != 0) continue;
            j = i;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
            e = j;
            while (e < n && is_id((unsigned char)s[e])) e++;
            if (e == j) continue;                                  /* no hay nombre de bloque */
            q = e;
            while (q < n && (s[q] == ' ' || s[q] == '\t' || s[q] == '\n' || s[q] == '\r')) q++;
            if (q >= n || s[q] != '{') continue;                   /* no es un bloque */
            {   /* calificadores previos: layout(...) y readonly/writeonly/coherent/volatile/restrict */
                size_t k = b;
                long lay_open = -1, lay_close = -1;
                for (;;) {
                    size_t w;
                    while (k > 0 && (s[k - 1] == ' ' || s[k - 1] == '\t' || s[k - 1] == '\n' || s[k - 1] == '\r')) k--;
                    if (k > 0 && s[k - 1] == ')') {
                        long depth = 0; size_t m = k;
                        lay_close = (long)k - 1;
                        while (m > 0) { m--; if (s[m] == ')') depth++; else if (s[m] == '(') { if (--depth == 0) break; } }
                        lay_open = (long)m;
                        k = m;
                        while (k > 0 && (s[k - 1] == ' ' || s[k - 1] == '\t')) k--;
                        if (k >= 6 && memcmp(s + k - 6, "layout", 6) == 0) { k -= 6; break; }
                        lay_open = lay_close = -1;
                        break;
                    }
                    w = k;
                    while (w > 0 && is_id((unsigned char)s[w - 1])) w--;
                    if (w == k) break;
                    if ((k - w == 8 && !memcmp(s + w, "readonly", 8)) || (k - w == 9 && !memcmp(s + w, "writeonly", 9)) ||
                        (k - w == 8 && !memcmp(s + w, "coherent", 8)) || (k - w == 8 && !memcmp(s + w, "volatile", 8)) ||
                        (k - w == 8 && !memcmp(s + w, "restrict", 8))) { k = w; continue; }
                    break;
                }
                if (lay_open >= 0) {
                    char* bd = NULL; size_t z;
                    for (z = (size_t)lay_open; z + 7 <= (size_t)lay_close; z++)
                        if (!memcmp(s + z, "binding", 7) && (z == 0 || !is_id((unsigned char)s[z - 1]))) { bd = s + z; break; }
                    if (bd) {
                        if (pass == 0) { const char* eq = strchr(bd, '='); if (eq && eq < s + lay_close) used |= 1u << (atoi(eq + 1) & 31); }
                        continue;
                    }
                }
                if (pass == 0) continue;
                {   /* sin binding: el primero libre */
                    int free_b = 0; char ins[40]; char nm[64]; size_t nl = e - j;
                    while (free_b < 31 && (used & (1u << free_b))) free_b++;
                    used |= 1u << free_b;
                    if (info && info->n_ssbo_unb < 8) {
                        if (nl >= sizeof nm) nl = sizeof nm - 1;
                        memcpy(nm, s + j, nl); nm[nl] = 0;
                        strcpy(info->ssbo_unb[info->n_ssbo_unb++], nm);
                    }
                    if (lay_open >= 0) {
                        snprintf(ins, sizeof ins, "binding=%d, ", free_b);
                        if (!ssbo_insert(&s, (size_t)lay_open + 1, ins)) { free(s); return NULL; }
                    } else {
                        snprintf(ins, sizeof ins, "layout(binding=%d) ", free_b);
                        if (!ssbo_insert(&s, b, ins)) { free(s); return NULL; }
                    }
                    n = strlen(s);
                    i = e + strlen(ins);
                }
            }
        }
    }
    return s;
}

/* gl31_glsl_convert_for_legacy (camino hibrido) marca esta conversion: ademas de la comun, baja los built-ins de compatibilidad
 * y quita las redeclaraciones de built-ins que el GLSL de escritorio admite. */
static int g_promote_int_next;

/* Contrato de built-ins de compatibilidad (modo hibrido, etapa 2). Los atributos (gl_Vertex, gl_MultiTexCoordN...) y los
 * uniforms de estado (gl_ModelViewProjectionMatrix, gl_NormalMatrix, gl_TextureMatrix[]...) dependen de la funcion fija,
 * que en un contexto hibrido es de GL4ES. GLADIATOR no duplica sus tablas: le pregunta al dueno del contrato (registrado
 * por el puente) el nombre y la declaracion de cada uno, y el runtime de GL4ES los alimenta por nombre al linkear y al
 * dibujar. Sin contrato (contextos core) siguen rechazados. */
static gl31_compat_builtin_fn g_compat_builtin = NULL;
void gl31_glsl_set_compat_builtins(gl31_compat_builtin_fn fn) { g_compat_builtin = fn; }

/* Baja los built-ins de compatibilidad que el contrato conoce. Devuelve s (o NULL si OOM); las declaraciones van a d. */
static char* lower_compat_builtins(char* s, int vertex, sb_t* d)
{
    char seen[48][48];
    int nseen = 0, k;
    const char* p = s;
    char repl[96], decl[160];
    if (!g_compat_builtin) return s;
    /* ftransform(): MVP * gl_Vertex (lo resuelven los dos built-ins de abajo) */
    char ft_mvp[96] = "", ft_vtx[96] = "";
    if (vertex && find_word(s, "ftransform")) {
        repl[0] = decl[0] = 0;
        if (g_compat_builtin("gl_ModelViewProjectionMatrix", 1, ft_mvp, sizeof ft_mvp, decl, sizeof decl) && ft_mvp[0]) {
            if (decl[0] && !strstr(d->p ? d->p : "", decl)) sb_str(d, decl);
            decl[0] = 0;
            if (g_compat_builtin("gl_Vertex", 1, ft_vtx, sizeof ft_vtx, decl, sizeof decl) && ft_vtx[0]) {
                if (decl[0] && !strstr(d->p ? d->p : "", decl)) sb_str(d, decl);
                s = repl_word(s, "ftransform", "lorica_ftransform");
                if (!s) return NULL;
            }
        }
    }
    p = s;
    while ((p = strstr(p, "gl_")) != NULL) {
        const char* e = p + 3;
        size_t n;
        if (p != s && is_id((unsigned char)p[-1])) { p += 3; continue; }
        while (is_id((unsigned char)*e)) e++;
        n = (size_t)(e - p);
        if (n < sizeof seen[0]) {
            char nm[48];
            memcpy(nm, p, n); nm[n] = 0;
            for (k = 0; k < nseen; k++) if (!strcmp(seen[k], nm)) break;
            if (k == nseen && nseen < 48) {
                strcpy(seen[nseen++], nm);
                repl[0] = decl[0] = 0;
                if (g_compat_builtin(nm, vertex, repl, sizeof repl, decl, sizeof decl) && repl[0]) {
                    size_t off = (size_t)(p - s);
                    s = repl_word(s, nm, repl);
                    if (!s) return NULL;
                    if (decl[0] && !strstr(d->p ? d->p : "", decl)) sb_str(d, decl);
                    p = s + off;          /* el texto cambio: seguir desde la misma posicion */
                    continue;
                }
            }
        }
        p = e;
    }
    if (ft_vtx[0] && find_word(s, "lorica_ftransform")) {
        sb_str(d, "vec4 lorica_ftransform() { return "); sb_str(d, ft_mvp); sb_str(d, " * "); sb_str(d, ft_vtx);
        sb_str(d, "; }\n");
    }
    return s;
}
static int lcg_idc(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* Camino hibrido: el GLSL de escritorio permite redeclarar built-ins (`out vec4 gl_Position;`, `in vec4 gl_FragCoord;`...) y ES no. */
static size_t lcg_strip_builtin_decl(char* s, size_t len)
{
    static const char* const decls[][3] = {
        { "out", "vec4", "gl_Position" }, { "in", "vec4", "gl_FragCoord" },
        { "out", "vec4", "gl_FragColor" }, { "out", "float", "gl_FragDepth" }
    };
    size_t i = 0, o = 0;
    while (i < len) {
        int hit = 0;
        if (i == 0 || s[i - 1] == '\n') {
            size_t j = i;
            int d;
            for (d = 0; d < 4 && !hit; d++) {
                int k, ok = 1;
                j = i;
                while (j < len && (s[j] == ' ' || s[j] == '\t')) j++;
                for (k = 0; k < 3 && ok; k++) {
                    size_t l = strlen(decls[d][k]);
                    if (j + l <= len && memcmp(s + j, decls[d][k], l) == 0 && (j + l == len || !lcg_idc((unsigned char)s[j + l]))) {
                        j += l;
                        while (j < len && (s[j] == ' ' || s[j] == '\t')) j++;
                    } else ok = 0;
                }
                if (ok && j < len && s[j] == ';') {
                    j++;
                    while (j < len && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r')) j++;
                    if (j >= len || s[j] == '\n') { hit = 1; i = (j < len) ? j + 1 : j; }
                }
            }
        }
        if (hit) continue;
        s[o++] = s[i++];
    }
    s[o] = '\0';
    return o;
}

/* ---------- extensiones de ES que cada construccion de GLSL necesita ----------
 * Una tabla en vez de un `if` por construccion: palabra (o etapa) que la activa, desde que version de GLSL ES es core, que
 * capacidad del backend la sostiene y que extension se pide cuando no es core. Si la construccion aparece y el backend no la
 * tiene, el shader se rechaza con un mensaje claro (antes se dejaba al compilador del driver, que no dice por que). */
enum { ST_V = 1, ST_F = 2, ST_G = 4, ST_TC = 8, ST_TE = 16, ST_C = 32, ST_ALL = 63 };
#define NOCAP ((size_t)-1)
#define CAP(f) offsetof(gl31_caps_t, f)
typedef struct {
    const char* const* words;   /* palabras que la activan; NULL = la activa la etapa */
    int stages;                 /* etapas a las que se aplica */
    int es_core;                /* GLSL ES desde el que es core; 0 = hace falta la extension siempre */
    size_t cap;                 /* CAP(campo) que dice que el backend la tiene; NOCAP = sin requisito */
    const char* ext;            /* extension que se pide si no es core (NULL = ninguna) */
    const char* err;            /* mensaje si el backend no la tiene */
} ext_rule_t;
static const char* const w_cubearr[] = { "samplerCubeArray", "isamplerCubeArray", "usamplerCubeArray", "samplerCubeArrayShadow", NULL };
static const char* const w_ms[]      = { "sampler2DMS", "isampler2DMS", "usampler2DMS", NULL };
static const char* const w_msarr[]   = { "sampler2DMSArray", "isampler2DMSArray", "usampler2DMSArray", NULL };
static const char* const w_texbuf[]  = { "samplerBuffer", "isamplerBuffer", "usamplerBuffer", NULL };
static const char* const w_psize[]   = { "gl_PointSize", NULL };
static const ext_rule_t k_ext_rules[] = {
    { NULL,       ST_G,           320, NOCAP, "GL_EXT_geometry_shader", NULL },
    { NULL,       ST_TC | ST_TE,  320, NOCAP, "GL_EXT_tessellation_shader", NULL },
    { w_cubearr,  ST_ALL,         320, CAP(cube_array), "GL_EXT_texture_cube_map_array", "requiere cube map arrays (ES 3.2 o EXT_texture_cube_map_array)" },
    { w_ms,       ST_ALL,         310, CAP(multisample_tex), NULL, "requiere ES 3.1 en el backend" },
    { w_msarr,    ST_ALL,         0,   CAP(ms_array), "GL_OES_texture_storage_multisample_2d_array", "requiere OES_texture_storage_multisample_2d_array" },
    { w_texbuf,   ST_ALL,         320, CAP(tex_buffer), "GL_EXT_texture_buffer", "requiere texture buffers (ES 3.2 o EXT_texture_buffer)" },
    /* gl_PointSize fuera del vertex shader es una extension aparte tambien en ES 3.2 */
    { w_psize,    ST_G,           0,   CAP(geometry_point_size), "GL_EXT_geometry_point_size", "gl_PointSize en geometry shaders requiere EXT_geometry_point_size en el backend" },
    { w_psize,    ST_TC | ST_TE,  0,   CAP(tess_point_size), "GL_EXT_tessellation_point_size", "gl_PointSize en teselacion requiere EXT_tessellation_point_size en el backend" },
};

static int stage_bit(GLenum t)
{
    return t == GL_VERTEX_SHADER ? ST_V : t == GL_FRAGMENT_SHADER ? ST_F : t == GL_GEOMETRY_SHADER ? ST_G :
           t == GL_TESS_CONTROL_SHADER ? ST_TC : t == GL_TESS_EVALUATION_SHADER ? ST_TE : ST_C;
}

/* Aplica la tabla: emite las `#extension` que hacen falta en `ext`. Devuelve 0, o 1 si falta una capacidad (*word y *err dicen cual). */
static int apply_ext_rules(const char* s, GLenum type, int es, sb_t* ext, const char** word, const char** err)
{
    size_t r;
    for (r = 0; r < sizeof k_ext_rules / sizeof k_ext_rules[0]; r++) {
        const ext_rule_t* e = &k_ext_rules[r];
        const char* hit = NULL;
        if (!(e->stages & stage_bit(type))) continue;
        if (!e->words) hit = "";
        else { int i; for (i = 0; e->words[i] && !hit; i++) if (find_word(s, e->words[i])) hit = e->words[i]; }
        if (!hit) continue;
        if (e->cap != NOCAP && !*(const int*)((const char*)&gl31_caps + e->cap)) { *word = hit; *err = e->err; return 1; }
        if (e->ext && (e->es_core == 0 || es < e->es_core)) { sb_str(ext, "#extension "); sb_str(ext, e->ext); sb_str(ext, " : require\n"); }
    }
    return 0;
}

char* gl31_glsl_convert_ex(const char* src, GLenum shader_type,
                           const gl31_fragbind_t* binds, int nbinds,
                           gl31_sampler_info_t* samplers, char* err, size_t errlen)
{
    char *s = NULL, *p;
    sb_t ext = {0, 0, 0, 0}, o = {0, 0, 0, 0};
    long ver = 110;
    int i, uses_color = 0, uses_data = 0, uses_shadow = 0, es = gl31_caps.glsl_es ? gl31_caps.glsl_es : 300;
    char num[32];
    static GL31_TLS smpnames_t names;
    unsigned char used[F_COUNT][K_COUNT][3];
    const char* bad = NULL;

    gl31_conv_opts_t opts = g_opts;                /* las opciones valen para una sola conversion */
    sb_t lu_hdr = {0, 0, 0, 0}, lu_tail = {0, 0, 0, 0};
    memset(&g_opts, 0, sizeof g_opts);
    int promote_int = g_promote_int_next;
    g_promote_int_next = 0;
    memset(&g_info, 0, sizeof g_info);
    memset(&names, 0, sizeof names);
    memset(used, 0, sizeof used);
    if (samplers) samplers->n = 0;
    if (err && errlen) err[0] = 0;
    if (!src || !is_stage(shader_type)) {
        set_err(err, errlen, "tipo de shader o fuente invalido", NULL);
        return NULL;
    }
    s = (char*)malloc(strlen(src) + 1);
    if (!s) goto oom;
    strcpy(s, src);
    strip_comments(s);

    /* --- #version --- */
    p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "#version", 8) == 0) {
        char* q = p + 8;
        char* endp;
        ver = strtol(q, &endp, 10);
        while (*endp == ' ' || *endp == '\t') endp++;
        if (endp[0] == 'e' && endp[1] == 's' && !is_id((unsigned char)endp[2])) {
            char* same = (char*)malloc(strlen(src) + 1);     /* ya es GLSL ES: sin tocar */
            if (!same) goto oom;
            strcpy(same, src);
            free(s);
            return same;
        }
        while (*p && *p != '\n') *p++ = ' ';
    }
    if (!(ver == 110 || ver == 120 || ver == 130 || ver == 140 || ver == 150 || ver == 330 ||
          ver == 400 || ver == 410 || ver == 420 || ver == 430 || ver == 440 || ver == 450 || ver == 460)) {
        snprintf(num, sizeof num, "%ld", ver);
        set_err(err, errlen, "version GLSL no soportada (admitido 110-150, 330, 400-460)", num);
        goto fail;
    }
    if (shader_type == GL_GEOMETRY_SHADER && ver < 150) {
        set_err(err, errlen, "los geometry shaders requieren #version 150", NULL);
        goto fail;
    }
    if ((shader_type == GL_TESS_CONTROL_SHADER || shader_type == GL_TESS_EVALUATION_SHADER) && ver < 400) {
        set_err(err, errlen, "los shaders de tessellation requieren #version 400", NULL);
        goto fail;
    }
    if (shader_type == GL_COMPUTE_SHADER && ver < 430) {
        set_err(err, errlen, "los compute shaders requieren #version 430", NULL);
        goto fail;
    }

    /* solo el codigo activo: todas las comprobaciones textuales de abajo dependen de esto */
    {   /* preprocesador de una pasada: condicionales resueltos y macros expandidas, de modo que el conversor tipado y las
         * comprobaciones textuales ven las expresiones reales (#define TAM textureSize(t, 0) dejaria sin tipo una asignacion a vec2) */
        const char* why = NULL;
        char* x = gl31_glsl_preprocess(s, ver, &why);
        if (x) { free(s); s = x; }
        else if (gl31_diag()) gl31_log("LORICA GLSL: preprocessing skipped (%s): conditionals and macros are left to the driver", why ? why : "?");
    }

    /* built-ins de compatibilidad que son un varying puro (sin estado externo, como VARYING_SLOT_COL0 en Mesa):
     * par de color, gl_TexCoord[] y gl_FogFragCoord pasan a varyings genericos. Los que necesitan estado de funcion fija
     * (gl_Vertex, gl_ModelViewMatrix, atributos...) siguen rechazados abajo. */
    {
        sb_t d = {0, 0, 0, 0};
        int vsx = (shader_type == GL_VERTEX_SHADER), fsx = (shader_type == GL_FRAGMENT_SHADER);
        if (vsx && find_word(s, "gl_FrontColor")) { s = repl_word(s, "gl_FrontColor", "lorica_fc"); if (s && !strstr(s, "vec4 lorica_fc;")) sb_str(&d, "out vec4 lorica_fc;\n"); }
        if (s && fsx && find_word(s, "gl_Color")) { s = repl_word(s, "gl_Color", "lorica_fc"); if (s && !strstr(s, "vec4 lorica_fc;")) sb_str(&d, "in vec4 lorica_fc;\n"); }
        if (s && (vsx || fsx) && find_word(s, "gl_TexCoord")) {
            s = repl_word(s, "gl_TexCoord", "lorica_tc"); if (s && !strstr(s, "vec4 lorica_tc[")) sb_str(&d, vsx ? "out vec4 lorica_tc[8];\n" : "in vec4 lorica_tc[8];\n");
        }
        if (s && (vsx || fsx) && find_word(s, "gl_FogFragCoord")) {
            s = repl_word(s, "gl_FogFragCoord", "lorica_ffc"); if (s && !strstr(s, "float lorica_ffc;")) sb_str(&d, vsx ? "out float lorica_ffc;\n" : "in float lorica_ffc;\n");
        }
        /* atributos y uniforms de estado: solo en el camino hibrido (promote_int marca gl31_glsl_convert_for_legacy) */
        if (s && promote_int && (vsx || fsx)) s = lower_compat_builtins(s, vsx, &d);
        if (!s || d.oom) { free(d.p); goto oom; }
        if (d.n) {
            size_t dl = strlen(d.p), sl = strlen(s);
            char* t = (char*)malloc(dl + sl + 1);
            if (!t) { free(d.p); goto oom; }
            memcpy(t, d.p, dl); memcpy(t + dl, s, sl + 1);
            free(s); s = t;
        }
        free(d.p);
    }
    if (promote_int) lcg_strip_builtin_decl(s, strlen(s));
    {
        /* conversiones implicitas de escritorio (int -> uint -> float) hechas explicitas, con tipos (gl31_glsl_tc.c) */
        char* t = gl31_glsl_implicit_conv(s);
        if (!t) goto oom;
        free(s); s = t;
    }

    /* --- construcciones sin equivalente o que dependen del backend --- */
    for (i = 0; k_unsupported[i]; i++)
        if (find_word(s, k_unsupported[i])) {
            set_err(err, errlen, "no disponible en GL 3.x sobre GLES", k_unsupported[i]);
            goto fail;
        }
    {
        static const char* const im[] = { "image2D", "iimage2D", "uimage2D", "image3D", "iimage3D", "uimage3D",
                                          "imageCube", "iimageCube", "uimageCube", "image2DArray",
                                          "iimage2DArray", "uimage2DArray", "atomic_uint", NULL };
        for (i = 0; im[i]; i++)
            if (find_word(s, im[i]) && !gl31_caps.es31) {
                set_err(err, errlen, "requiere ES 3.1 en el backend", im[i]);
                goto fail;
            }
        if ((find_word(s, "buffer") && find_word(s, "std430")) && !gl31_caps.es31) {
            set_err(err, errlen, "los SSBO requieren ES 3.1 en el backend", "buffer");
            goto fail;
        }
    }
    if (find_word(s, "gl_ClipDistance") && !gl31_caps.clip_distance &&
        !(shader_type == GL_VERTEX_SHADER && opts.last_vs)) {
        set_err(err, errlen, "gl_ClipDistance requiere EXT_clip_cull_distance en el backend", "gl_ClipDistance");
        goto fail;
    }
    if (shader_type == GL_FRAGMENT_SHADER) {
        uses_color = find_word(s, "gl_FragColor") != NULL;
        uses_data  = find_word(s, "gl_FragData")  != NULL;
        /* Tras el preprocesador condicional solo queda el codigo activo: usar los dos a la vez es un error real. */
        if (uses_color && uses_data) {
            set_err(err, errlen, "gl_FragColor y gl_FragData a la vez", NULL);
            goto fail;
        }
    }

    g_imix = 0;
    process_extensions(s, &ext);
    if (ext.oom) goto oom;
    if (g_imix) { s = repl_word(s, "mix", "lorica_mix"); if (!s) goto oom; }

    /* extensiones que cada construccion pide al backend (tabla k_ext_rules) */
    {
        const char *bw = NULL, *be = NULL;
        if (apply_ext_rules(s, shader_type, es, &ext, &bw, &be)) { set_err(err, errlen, be, bw); goto fail; }
        if (ext.oom) goto oom;
    }
    if (gl31_caps.clip_distance && find_word(s, "gl_ClipDistance"))
        sb_str(&ext, gl31_caps.clip_distance == 2 ? "#extension GL_ANGLE_clip_cull_distance : require\n"
                                                  : "#extension GL_EXT_clip_cull_distance : require\n");
    if (es < 320 && (gl31_caps.geometry || gl31_caps.tess) && gl31_caps.io_blocks_ext)
        sb_str(&ext, "#extension GL_EXT_shader_io_blocks : enable\n");

    strip_pervertex(s);
    {
        int did = 0;
        s = demote_doubles(s, &did);
        if (!s) goto oom;
    }
    if (es < 320 && !(s = repl_word(s, "precise", ""))) goto oom;

    /* --- palabras clave ---
     * attribute/varying siguen siendo validos (obsoletos) en GLSL 1.30/1.40 y en el perfil compatibility, y en GLSL ES 3.00
     * son palabras reservadas: cualquier aparicion es el calificador, sea cual sea la version. */
    {
        if (shader_type == GL_VERTEX_SHADER) {
            if (!(s = repl_word(s, "attribute", "in"))) goto oom;
            if (!(s = repl_word(s, "varying", "out"))) goto oom;
        } else {
            if (!(s = repl_word(s, "varying", "in"))) goto oom;
        }
    }
    for (i = 0; k_rename[i][0]; i++)
        if (!(s = repl_word(s, k_rename[i][0], k_rename[i][1]))) goto oom;
    {   /* shadow2D* de GLSL <= 1.20: devuelven vec4; texture() de un sampler2DShadow devuelve float */
        static const char* const sh[] = { "shadow2D", "shadow2DProj", "shadow2DLod", "shadow2DProjLod", NULL };
        char nm[32];
        for (i = 0; sh[i]; i++)
            if (find_word(s, sh[i])) {
                snprintf(nm, sizeof nm, "lorica_%s", sh[i]);
                if (!(s = repl_word(s, sh[i], nm))) goto oom;
                uses_shadow = 1;
            }
    }
    if (!(s = repl_word(s, "noperspective", ""))) goto oom;
    if (uses_color && !(s = repl_word(s, "gl_FragColor", "lorica_FragColor"))) goto oom;
    if (uses_data  && !(s = repl_word(s, "gl_FragData",  "lorica_FragData")))  goto oom;

    /* --- samplers 1D / 1DArray / Rect --- */
    if (!collect_smp_names(s, &names)) {
        set_err(err, errlen, "demasiados samplers 1D/Rect", NULL);
        goto fail;
    }
    if (names.cnt) {
        if (samplers)
            for (i = 0; i < names.cnt && samplers->n < GL31_MAX_SAMPLER_INFO; i++) {
                if (names.n[i].scope >= 0) continue;   /* parametros de funcion: no son uniforms */
                memcpy(samplers->s[samplers->n].name, names.n[i].name, sizeof samplers->s[0].name);
                samplers->s[samplers->n].type = names.n[i].gl;
                samplers->n++;
            }
        s = rewrite_calls(s, &names, used, &bad);
        if (!s) {
            if (bad) { set_err(err, errlen, "funcion sin soporte sobre sampler 1D/Rect", bad); goto fail; }
            goto oom;
        }
        for (i = 0; k_smp[i].from; i++)
            if (!(s = repl_word(s, k_smp[i].from, k_smp[i].to))) goto oom;
    }

    g_saw_index = 0;
    p = filter_layouts(s, shader_type, es);
    free(s);
    s = p;
    if (!s) goto oom;
    /* `layout(depth_any) out float gl_FragDepth;` leaves a bare redeclaration once the layout is gone */
    if (promote_int) lcg_strip_builtin_decl(s, strlen(s));
    if (shader_type == GL_FRAGMENT_SHADER && nbinds > 0) {
        int k2;
        s = inject_frag_locations(s, binds, nbinds);
        if (!s) goto oom;
        for (k2 = 0; k2 < nbinds; k2++) if (binds[k2].index) g_saw_index = 1;
    }
    if (g_saw_index && shader_type == GL_FRAGMENT_SHADER)
        sb_str(&ext, "#extension GL_EXT_blend_func_extended : require\n");

    if (!lu_transform(&s, shader_type, &opts, &lu_hdr, &lu_tail)) goto oom;
    if (lu_hdr.oom || lu_tail.oom) goto oom;

    if (gl31_caps.es31 && find_word(s, "buffer")) {
        s = assign_ssbo_bindings(s, &g_info);
        if (!s) goto oom;
    }

    /* --- salida: cabecera ES + #line 1 para conservar numeracion --- */
    snprintf(num, sizeof num, "#version %d es\n", es);
    sb_str(&o, num);
    if (ext.n) sb_cat(&o, ext.p, ext.n);
    emit_precisions(&o);
    if (g_imix) emit_integer_mix(&o);
    if (uses_color) sb_str(&o, "layout(location = 0) out highp vec4 lorica_FragColor; ");
    if (uses_data)  sb_str(&o, "layout(location = 0) out highp vec4 lorica_FragData[4]; ");
    emit_helpers(&o, used, shader_type == GL_FRAGMENT_SHADER);
    if (uses_shadow) {   /* resultado como con GL_DEPTH_TEXTURE_MODE = GL_LUMINANCE (el valor por defecto) */
        sb_str(&o, "\nhighp vec4 lorica_shadow2D(highp sampler2DShadow s, highp vec3 c) { highp float r = texture(s, c); return vec4(r, r, r, 1.0); }\n"
                   "highp vec4 lorica_shadow2DProj(highp sampler2DShadow s, highp vec4 c) { highp float r = textureProj(s, c); return vec4(r, r, r, 1.0); }\n"
                   "highp vec4 lorica_shadow2DLod(highp sampler2DShadow s, highp vec3 c, highp float l) { highp float r = textureLod(s, c, l); return vec4(r, r, r, 1.0); }\n"
                   "highp vec4 lorica_shadow2DProjLod(highp sampler2DShadow s, highp vec4 c, highp float l) { highp float r = textureProjLod(s, c, l); return vec4(r, r, r, 1.0); }\n");
        if (shader_type == GL_FRAGMENT_SHADER)   /* con bias: solo en el fragment shader */
            sb_str(&o, "highp vec4 lorica_shadow2D(highp sampler2DShadow s, highp vec3 c, highp float b) { highp float r = texture(s, c, b); return vec4(r, r, r, 1.0); }\n"
                       "highp vec4 lorica_shadow2DProj(highp sampler2DShadow s, highp vec4 c, highp float b) { highp float r = textureProj(s, c, b); return vec4(r, r, r, 1.0); }\n");
    }
    if (lu_hdr.n) sb_cat(&o, lu_hdr.p, lu_hdr.n);
    sb_str(&o, "\n#line 1\n");
    sb_str(&o, s);
    if (lu_tail.n) sb_cat(&o, lu_tail.p, lu_tail.n);
    if (o.oom) goto oom;

    free(s); free(ext.p); free(lu_hdr.p); free(lu_tail.p);
    return o.p;

oom:
    set_err(err, errlen, "sin memoria", NULL);
fail:
    free(s); free(ext.p); free(o.p); free(lu_hdr.p); free(lu_tail.p);
    return NULL;
}

char* gl31_glsl_convert(const char* src, GLenum shader_type, char* err, size_t errlen)
{
    return gl31_glsl_convert_ex(src, shader_type, NULL, 0, NULL, err, errlen);
}

/* Conversion para un shader que compila otro llamador (el puente hibrido: GL4ES compila el GLSL ES que sale de aqui).
 * Mismas opciones que usa gl31_glCompileShader para un shader suelto. Devuelve memoria de malloc o NULL con `err`. */
char* gl31_glsl_convert_for_legacy(const char* src, unsigned shader_type, char* err, size_t errlen)
{
    return gl31_glsl_convert_for_legacy_ex(src, shader_type, 0, 0, NULL, err, errlen);
}

/* Igual, con las clip distances emuladas (backend sin EXT_clip_cull_distance): un VS devuelve en *cd_n cuantas
 * escribe en lorica_cd[]; un FS se convierte con fs_cd = las del VS del programa (las descarta por mascara). Es lo
 * que hace gl31_glLinkProgram en sus contextos; en el hibrido linkea GL4ES y lo pide al linkear. */
char* gl31_glsl_convert_for_legacy_ex(const char* src, unsigned shader_type, int staged, int fs_cd, int* cd_n,
                                      char* err, size_t errlen)
{
    gl31_conv_opts_t co;
    char* out;
    memset(&co, 0, sizeof co);
    co.last_vs = (shader_type == GL_VERTEX_SHADER) && !staged;   /* con geometry/teselacion el VS no es la ultima etapa */
    co.fs_cd = (shader_type == GL_FRAGMENT_SHADER) ? fs_cd : 0;
    gl31_glsl_set_opts(&co);
    g_promote_int_next = 1;
    out = gl31_glsl_convert_ex(src, (GLenum)shader_type, NULL, 0, NULL, err, errlen);
    if (cd_n) {
        gl31_conv_info_t ci;
        memset(&ci, 0, sizeof ci);
        if (out) gl31_glsl_get_info(&ci);
        *cd_n = ci.cd_n;
    }
    return out;
}
