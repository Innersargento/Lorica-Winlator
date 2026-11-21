/* gl31_glsl_pp.c: preprocesador condicional minimo para el conversor GLSL (ver gl31.h). */
#include "gl31.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static int is_id(unsigned char c) { return isalnum(c) || c == '_'; }

/* ---------- preprocesador condicional minimo ----------
 * Los shaders de fuente unica (DarkPlaces/Xonotic, por ejemplo) mezclan en el mismo texto ramas de
 * varios modos bajo #if/#ifdef. Todas las comprobaciones textuales de este conversor (gl_FragColor vs
 * gl_FragData, main, clip distances, samplers, ...) deben ver solo el codigo activo, como el compilador.
 * Se evaluan #if/#ifdef/#ifndef/#elif/#else/#endif con los #define/#undef del propio texto
 * (semantica de GLSL de escritorio: __VERSION__ = version original, GL_ES sin definir) y las lineas
 * inactivas y las directivas condicionales se sustituyen por espacios (se conservan los \n). Los
 * #define activos se dejan: el compilador del backend los expande. Si alguna expresion no se puede
 * evaluar (sintaxis rara, macro recursiva) se devuelve 0 y el texto queda intacto.
 * Soporta macros con argumentos, el operador ?: y las macros GL_xxx de las extensiones que el
 * driver anuncia (gl31_ext_advertised), como hace el compilador de un driver real. */
#define PP_MAX_MACROS 512
#define PP_MAX_DEPTH  64
typedef struct { char name[64]; char par[128]; char val[256]; int fn, bad; } pp_macro_t;
typedef struct { pp_macro_t m[PP_MAX_MACROS]; int n; } pp_t;
typedef struct { const char* p; pp_t* pp; int depth; int err; } pp_ev_t;

static pp_macro_t* pp_find(pp_t* pp, const char* nm, size_t len)
{
    int i;
    for (i = 0; i < pp->n; i++)
        if (strlen(pp->m[i].name) == len && strncmp(pp->m[i].name, nm, len) == 0) return &pp->m[i];
    return NULL;
}

static int pp_define(pp_t* pp, const char* nm, size_t len, const char* par, size_t plen, const char* val, size_t vlen, int fn)
{
    pp_macro_t* m = pp_find(pp, nm, len);
    if (!m) {
        if (pp->n >= PP_MAX_MACROS || len >= sizeof m->name) return 0;
        m = &pp->m[pp->n++];
        memcpy(m->name, nm, len); m->name[len] = 0;
    }
    m->bad = 0;
    if (plen >= sizeof m->par) { plen = sizeof m->par - 1; m->bad = 1; }
    if (vlen >= sizeof m->val) { vlen = sizeof m->val - 1; m->bad = 1; }   /* truncada: no se puede evaluar */
    memcpy(m->par, par, plen); m->par[plen] = 0;
    memcpy(m->val, val, vlen); m->val[vlen] = 0;
    m->fn = fn;
    return 1;
}

static void pp_undef(pp_t* pp, const char* nm, size_t len)
{
    pp_macro_t* m = pp_find(pp, nm, len);
    if (m) *m = pp->m[--pp->n];
}

static void pp_ws(pp_ev_t* e) { while (*e->p == ' ' || *e->p == '\t' || *e->p == '\r') e->p++; }
/* ¿esta definido? Las macros del texto, y GL_xxx si el driver anuncia esa extension. */
static int pp_is_defined(pp_t* pp, const char* nm, size_t len)
{
    char buf[96];
    if (pp_find(pp, nm, len)) return 1;
    if (len > 3 && len < sizeof buf && nm[0] == 'G' && nm[1] == 'L' && nm[2] == '_') {
        memcpy(buf, nm, len); buf[len] = 0;
        return gl31_ext_advertised(buf);
    }
    return 0;
}

static long long pp_expr(pp_ev_t* e, int minprec);
static long long pp_ternary(pp_ev_t* e);

/* Expande una macro con argumentos: lee `(a, b)` en e->p y sustituye los parametros en el cuerpo. */
static int pp_expand_fn(pp_ev_t* e, const pp_macro_t* m, char* out, size_t cap)
{
    const char* args[16]; size_t alen[16]; int na = 0, np = 0, depth = 0, i;
    const char* pn[16]; size_t pl[16];
    const char* q;
    size_t n = 0;
    pp_ws(e);
    if (*e->p != '(') return 0;
    e->p++;
    args[0] = e->p;
    for (;; e->p++) {
        if (!*e->p) return 0;
        if (*e->p == '(') depth++;
        else if (*e->p == ')' && depth) depth--;
        else if ((*e->p == ',' || *e->p == ')') && !depth) {
            if (na >= 16) return 0;
            alen[na] = (size_t)(e->p - args[na]); na++;
            if (*e->p == ')') { e->p++; break; }
            args[na] = e->p + 1;
        }
    }
    /* parametros: "a, b" -> nombres */
    q = m->par;
    while (*q) {
        while (*q == ' ' || *q == ',') q++;
        if (!*q) break;
        if (np >= 16) return 0;
        pn[np] = q;
        while (is_id((unsigned char)*q)) q++;
        pl[np] = (size_t)(q - pn[np]);
        if (!pl[np]) return 0;
        np++;
    }
    if (np == 0 && na == 1 && alen[0] == 0) na = 0;     /* F() */
    if (na != np) return 0;
    q = m->val;
    while (*q) {
        if (isalpha((unsigned char)*q) || *q == '_') {
            const char* st = q; size_t k;
            while (is_id((unsigned char)*q)) q++;
            k = (size_t)(q - st);
            for (i = 0; i < np; i++)
                if (pl[i] == k && strncmp(pn[i], st, k) == 0) break;
            if (i < np) {
                if (n + alen[i] + 2 >= cap) return 0;
                out[n++] = '('; memcpy(out + n, args[i], alen[i]); n += alen[i]; out[n++] = ')';
            } else {
                if (n + k + 1 >= cap) return 0;
                memcpy(out + n, st, k); n += k;
            }
        } else {
            if (n + 2 >= cap) return 0;
            out[n++] = *q++;
        }
    }
    out[n] = 0;
    return 1;
}


static long long pp_primary(pp_ev_t* e)
{
    long long v;
    pp_ws(e);
    if (*e->p == '(') {
        e->p++; v = pp_ternary(e); pp_ws(e);
        if (*e->p != ')') { e->err = 1; return 0; }
        e->p++; return v;
    }
    if (*e->p == '!') { e->p++; return !pp_primary(e); }
    if (*e->p == '~') { e->p++; return ~pp_primary(e); }
    if (*e->p == '-') { e->p++; return -pp_primary(e); }
    if (*e->p == '+') { e->p++; return pp_primary(e); }
    if (isdigit((unsigned char)*e->p)) {
        char* end;
        v = strtoll(e->p, &end, 0);
        e->p = end;
        while (*e->p == 'u' || *e->p == 'U' || *e->p == 'l' || *e->p == 'L') e->p++;
        return v;
    }
    if (isalpha((unsigned char)*e->p) || *e->p == '_') {
        const char* st = e->p; size_t n;
        while (is_id((unsigned char)*e->p)) e->p++;
        n = (size_t)(e->p - st);
        if (n == 7 && strncmp(st, "defined", 7) == 0) {
            int paren = 0; const char* id; size_t idn;
            pp_ws(e);
            if (*e->p == '(') { paren = 1; e->p++; pp_ws(e); }
            id = e->p;
            while (is_id((unsigned char)*e->p)) e->p++;
            idn = (size_t)(e->p - id);
            if (!idn) { e->err = 1; return 0; }
            if (paren) { pp_ws(e); if (*e->p != ')') { e->err = 1; return 0; } e->p++; }
            return pp_is_defined(e->pp, id, idn);
        }
        {
            pp_macro_t* m = pp_find(e->pp, st, n);
            pp_ev_t sub;
            char xb[1024];
            if (!m) return pp_is_defined(e->pp, st, n);   /* sin definir vale 0; una extension anunciada, 1 */
            if (m->bad || e->depth > 16) { e->err = 1; return 0; }
            sub.pp = e->pp; sub.depth = e->depth + 1; sub.err = 0;
            if (m->fn) {
                if (!pp_expand_fn(e, m, xb, sizeof xb)) { e->err = 1; return 0; }
                sub.p = xb;
            } else sub.p = m->val;
            pp_ws(&sub);
            if (!*sub.p) { e->err = 1; return 0; }
            v = pp_ternary(&sub);
            pp_ws(&sub);
            if (sub.err || *sub.p) { e->err = 1; return 0; }
            return v;
        }
    }
    e->err = 1;
    return 0;
}

/* precedencia de los operadores binarios; 0 si *p no es uno. len = longitud del operador */
static int pp_binop(const char* p, int* len)
{
    *len = 2;
    if (p[0] == '|' && p[1] == '|') return 1;
    if (p[0] == '&' && p[1] == '&') return 2;
    if (p[0] == '=' && p[1] == '=') return 6;
    if (p[0] == '!' && p[1] == '=') return 6;
    if (p[0] == '<' && p[1] == '=') return 7;
    if (p[0] == '>' && p[1] == '=') return 7;
    if (p[0] == '<' && p[1] == '<') return 8;
    if (p[0] == '>' && p[1] == '>') return 8;
    *len = 1;
    if (p[0] == '|') return 3;
    if (p[0] == '^') return 4;
    if (p[0] == '&') return 5;
    if (p[0] == '<' || p[0] == '>') return 7;
    if (p[0] == '+' || p[0] == '-') return 9;
    if (p[0] == '*' || p[0] == '/' || p[0] == '%') return 10;
    return 0;
}

static long long pp_expr(pp_ev_t* e, int minprec)
{
    long long l = pp_primary(e);
    for (;;) {
        int len, prec;
        const char* op;
        long long r;
        pp_ws(e);
        if (e->err) return 0;
        op = e->p;
        prec = pp_binop(op, &len);
        if (!prec || prec <= minprec) return l;
        e->p += len;
        r = pp_expr(e, prec);
        if (e->err) return 0;
        switch (len == 2 ? op[0] * 256 + op[1] : op[0]) {
            case '|' * 256 + '|': l = l || r; break;
            case '&' * 256 + '&': l = l && r; break;
            case '=' * 256 + '=': l = l == r; break;
            case '!' * 256 + '=': l = l != r; break;
            case '<' * 256 + '=': l = l <= r; break;
            case '>' * 256 + '=': l = l >= r; break;
            case '<' * 256 + '<': l = r < 0 || r > 62 ? 0 : l << r; break;
            case '>' * 256 + '>': l = r < 0 || r > 62 ? 0 : l >> r; break;
            case '|': l = l | r; break;
            case '^': l = l ^ r; break;
            case '&': l = l & r; break;
            case '<': l = l < r; break;
            case '>': l = l > r; break;
            case '+': l = l + r; break;
            case '-': l = l - r; break;
            case '*': l = l * r; break;
            case '/': if (!r) { e->err = 1; return 0; } l = l / r; break;
            case '%': if (!r) { e->err = 1; return 0; } l = l % r; break;
            default: e->err = 1; return 0;
        }
    }
}

/* cond ? a : b (la rama no elegida se evalua igual: sus errores solo importan si hay sintaxis rota) */
static long long pp_ternary(pp_ev_t* e)
{
    long long c = pp_expr(e, 0), a, b;
    pp_ws(e);
    if (e->err || *e->p != '?') return c;
    e->p++;
    a = pp_ternary(e);
    pp_ws(e);
    if (e->err || *e->p != ':') { e->err = 1; return 0; }
    e->p++;
    b = pp_ternary(e);
    return c ? a : b;
}

/* Evalua una condicion #if / #elif. *ok = 0 si no se pudo. */
static int pp_eval(pp_t* pp, const char* text, int* ok)
{
    pp_ev_t e;
    long long v;
    e.p = text; e.pp = pp; e.depth = 0; e.err = 0;
    v = pp_ternary(&e);
    pp_ws(&e);
    *ok = !e.err && !*e.p;
    return v != 0;
}

typedef struct { int parent, taken, active; } pp_frame_t;

/* Devuelve 1 si el texto se proceso (s modificado en sitio), 0 si se dejo intacto. */
int gl31_glsl_pp_resolve(char* s, long ver)
{
    pp_t* pp = (pp_t*)calloc(1, sizeof *pp);
    char* t = (char*)malloc(strlen(s) + 1);
    pp_frame_t st[PP_MAX_DEPTH];
    int sp = 0, active = 1, ok = 1;
    char* p;
    char vbuf[24];
    if (!pp || !t) { free(pp); free(t); return 0; }
    strcpy(t, s);
    snprintf(vbuf, sizeof vbuf, "%ld", ver);
    pp_define(pp, "__VERSION__", 11, "", 0, vbuf, strlen(vbuf), 0);

    p = t;
    while (*p && ok) {
        char* ls = p;            /* inicio de la linea logica */
        char* le;                /* fin (sin el \n) */
        char* d = p;
        while (*d == ' ' || *d == '\t') d++;
        /* fin de linea logica: una directiva continua con \ al final de linea */
        le = p;
        for (;;) {
            while (*le && *le != '\n') le++;
            if (*d == '#' && le > ls && le[-1] == '\\' && *le) { le++; continue; }
            if (*d == '#' && le > ls + 1 && le[-1] == '\r' && le[-2] == '\\' && *le) { le++; continue; }
            break;
        }
        if (*d == '#') {
            char dir[16]; size_t dn = 0; char* a = d + 1;
            char bc[1024]; char* body = bc; size_t bl = 0;
            int blank = 0;
            while (*a == ' ' || *a == '\t') a++;
            while (is_id((unsigned char)*a) && dn < sizeof dir - 1) dir[dn++] = *a++;
            dir[dn] = 0;
            /* copia del cuerpo (hasta el fin de la linea logica) sin las continuaciones `\` + salto */
            for (; a < le && bl < sizeof bc - 1; a++)
                bc[bl++] = (*a == '\\' && (a[1] == '\n' || (a[1] == '\r' && a[2] == '\n'))) ? ' '
                         : (*a == '\n' || *a == '\r') ? ' ' : *a;
            bc[bl] = 0;
            if (a < le) ok = 0;                         /* directiva demasiado larga */
            if (!strcmp(dir, "if") || !strcmp(dir, "ifdef") || !strcmp(dir, "ifndef")) {
                int cond = 0;
                blank = 1;
                if (sp >= PP_MAX_DEPTH) { ok = 0; }
                else {
                    st[sp].parent = active;
                    if (active) {
                        if (dir[2] == 0) cond = pp_eval(pp, body, &ok);
                        else {
                            const char* id = body; size_t n;
                            while (*id == ' ' || *id == '\t') id++;
                            n = 0; while (is_id((unsigned char)id[n])) n++;
                            if (!n) ok = 0;
                            cond = pp_is_defined(pp, id, n);
                            if (dir[2] == 'n') cond = !cond;
                        }
                    }
                    st[sp].taken = active && cond;
                    active = active && cond;
                    st[sp].active = active;
                    sp++;
                }
            } else if (!strcmp(dir, "elif")) {
                blank = 1;
                if (!sp) ok = 0;
                else {
                    pp_frame_t* f = &st[sp - 1];
                    if (!f->parent || f->taken) active = 0;
                    else {
                        int cond = pp_eval(pp, body, &ok);
                        active = cond; f->taken = cond;
                    }
                }
            } else if (!strcmp(dir, "else")) {
                blank = 1;
                if (!sp) ok = 0;
                else {
                    pp_frame_t* f = &st[sp - 1];
                    active = f->parent && !f->taken;
                    f->taken = 1;
                }
            } else if (!strcmp(dir, "endif")) {
                blank = 1;
                if (!sp) ok = 0;
                else active = st[--sp].parent;
            } else if (!active) {
                blank = 1;
            } else if (!strcmp(dir, "define")) {
                const char* id = body; size_t n;
                while (*id == ' ' || *id == '\t') id++;
                n = 0; while (is_id((unsigned char)id[n])) n++;
                if (!n) ok = 0;
                else {
                    const char* v = id + n; int fn = (*v == '(');
                    const char* par = ""; size_t pl = 0;
                    if (fn) {                              /* #define F(a, b) cuerpo */
                        par = v + 1;
                        while (*v && *v != ')') v++;
                        if (!*v) ok = 0;
                        else { pl = (size_t)(v - par); v++; }
                    }
                    while (*v == ' ' || *v == '\t') v++;
                    if (ok && !pp_define(pp, id, n, par, pl, v, strlen(v), fn)) ok = 0;
                }
            } else if (!strcmp(dir, "undef")) {
                const char* id = body; size_t n;
                while (*id == ' ' || *id == '\t') id++;
                n = 0; while (is_id((unsigned char)id[n])) n++;
                pp_undef(pp, id, n);
            }
            if (blank) { char* b; for (b = ls; b < le; b++) if (*b != '\n') *b = ' '; }
        } else if (!active) {
            char* b; for (b = ls; b < le; b++) *b = ' ';
        }
        p = *le ? le + 1 : le;
    }
    if (sp) ok = 0;                                   /* #if sin cerrar */
    if (ok) strcpy(s, t);
    free(pp); free(t);
    return ok;
}

