/* gl31_glsl_pp.c: preprocesador GLSL de una pasada para el conversor (ver gl31.h).
 *
 * Los shaders de fuente unica (DarkPlaces/Xonotic, por ejemplo) mezclan en el mismo texto ramas de varios modos bajo #if/#ifdef, y
 * esconden expresiones tras #define. Todas las comprobaciones textuales del conversor (gl_FragColor vs gl_FragData, main, clip
 * distances, samplers, ...) y el conversor tipado (gl31_glsl_tc.c) deben ver el codigo activo y las expresiones reales, como el
 * compilador. Una sola pasada, como el glcpp de Mesa:
 *   - #if / #ifdef / #ifndef / #elif / #else / #endif se evaluan (semantica de GLSL de escritorio: __VERSION__ = version original,
 *     GL_ES sin definir, GL_xxx definido solo para las extensiones que el driver anuncia). La condicion se evalua como en C: primero
 *     `defined`, luego se expanden las macros (de objeto y de funcion), y lo que queda se calcula; un identificador sin definir vale 0.
 *   - las lineas inactivas y las directivas condicionales se sustituyen por espacios (se conservan los \n);
 *   - el codigo activo se expande: macros de objeto y de funcion, argumentos expandidos antes de sustituir, `##`, nuevo recorrido del
 *     resultado sin volver a expandir la macro en curso, __LINE__ y __VERSION__. Los saltos de linea se conservan (una llamada que
 *     ocupa varias lineas deja sus \n al final);
 *   - las demas directivas (#define incluido, #extension, #pragma, #line...) pasan intactas.
 * No hay tablas de tamano fijo: macros, argumentos y pila crecen con el fuente. El unico tope es el tamano de la salida (una macro
 * recursiva en exceso); si algo falla se devuelve NULL con el motivo y el conversor sigue con el texto original. */
#include "gl31.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static int is_id(unsigned char c) { return isalnum(c) || c == '_'; }

/* ------------------------------------------------------------------------------------------------ buffers y tabla de macros */
typedef struct { char* name; char** par; int np; int fn; int skip; char* body; } xm_t;
typedef struct { xm_t* m; int n, cap; } xtab_t;
typedef struct { char* p; size_t n, cap; int oom; size_t limit; } xsb_t;
typedef struct { xtab_t* tab; const xm_t** stack; int scap; int depth; int line; xsb_t* out; } xctx_t;

static void xsb_put(xsb_t* b, const char* s, size_t n)
{
    if (b->oom || !n) return;
    if (b->n + n + 1 > b->limit) { b->oom = 1; return; }
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        char* t;
        while (nc < b->n + n + 1) nc *= 2;
        t = (char*)realloc(b->p, nc);
        if (!t) { b->oom = 1; return; }
        b->p = t; b->cap = nc;
    }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void xsb_c(xsb_t* b, char c) { xsb_put(b, &c, 1); }

static char* xdup(const char* s, size_t n) { char* r = (char*)malloc(n + 1); if (r) { memcpy(r, s, n); r[n] = 0; } return r; }

static void xm_free(xm_t* m)
{
    int i;
    for (i = 0; i < m->np; i++) free(m->par[i]);
    free(m->par); free(m->name); free(m->body);
}

static xm_t* xm_find(xtab_t* t, const char* nm, size_t len)
{
    int i;
    for (i = 0; i < t->n; i++)
        if (strlen(t->m[i].name) == len && !strncmp(t->m[i].name, nm, len)) return &t->m[i];
    return NULL;
}

static void xm_undef(xtab_t* t, const char* nm, size_t len)
{
    xm_t* m = xm_find(t, nm, len);
    if (!m) return;
    xm_free(m);
    *m = t->m[--t->n];
}

/* Registra un #define (toma posesion de par). Una redefinicion sustituye a la anterior. Devuelve 0 sin memoria. */
static int xm_define(xtab_t* t, const char* nm, size_t len, char** par, int np, int fn, int skip, const char* body, size_t blen)
{
    xm_t* m;
    xm_undef(t, nm, len);
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 32;
        xm_t* nt = (xm_t*)realloc(t->m, (size_t)nc * sizeof *nt);
        if (!nt) return 0;
        t->m = nt; t->cap = nc;
    }
    m = &t->m[t->n];
    memset(m, 0, sizeof *m);
    m->name = xdup(nm, len); m->body = xdup(body, blen);
    if (!m->name || !m->body) { free(m->name); free(m->body); return 0; }
    m->par = par; m->np = np; m->fn = fn; m->skip = skip;
    t->n++;
    return 1;
}

static int x_idstart(unsigned char ch) { return isalpha(ch) || ch == '_'; }
static int x_blank(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

/* Un token numerico entero (1, 1.0, .5, 1e-3, 0x1F, 1.0f, 1u): su sufijo no es un identificador */
static size_t x_number(const char* in, size_t i, size_t len)
{
    size_t j = i;
    int hex = (in[j] == '0' && j + 1 < len && (in[j + 1] == 'x' || in[j + 1] == 'X'));
    while (j < len) {
        unsigned char ch = (unsigned char)in[j];
        if (isalnum(ch) || ch == '_' || ch == '.') { j++; continue; }
        if ((ch == '+' || ch == '-') && !hex && j > i && (in[j - 1] == 'e' || in[j - 1] == 'E')) { j++; continue; }
        break;
    }
    return j;
}

static int x_on_stack(const xctx_t* c, const xm_t* m)
{
    int i;
    for (i = 0; i < c->depth; i++) if (c->stack[i] == m) return 1;
    return 0;
}

static int x_param(const xm_t* m, const char* id, size_t n)
{
    int a;
    for (a = 0; a < m->np; a++)
        if (strlen(m->par[a]) == n && !strncmp(m->par[a], id, n)) return a;
    return -1;
}

/* Sustituye los parametros del cuerpo por los argumentos (expandidos). Los operandos de `##` van sin expandir y se pegan. */
static void x_substitute(const xm_t* m, char** raw, char** exp, xsb_t* o)
{
    const char* q = m->body;
    size_t bl = strlen(q), i = 0;
    while (i < bl) {
        unsigned char ch = (unsigned char)q[i];
        if (ch == '#' && i + 1 < bl && q[i + 1] == '#') {
            while (o->n && (o->p[o->n - 1] == ' ' || o->p[o->n - 1] == '\t')) o->p[--o->n] = 0;
            i += 2;
            while (i < bl && (q[i] == ' ' || q[i] == '\t')) i++;
            if (i < bl && x_idstart((unsigned char)q[i])) {         /* operando derecho: crudo */
                size_t s = i;
                int a;
                while (i < bl && (isalnum((unsigned char)q[i]) || q[i] == '_')) i++;
                a = x_param(m, q + s, i - s);
                if (a >= 0) xsb_put(o, raw[a], strlen(raw[a]));
                else xsb_put(o, q + s, i - s);
            }
            continue;
        }
        if (isdigit(ch) || (ch == '.' && i + 1 < bl && isdigit((unsigned char)q[i + 1]))) {
            size_t j = x_number(q, i, bl);
            xsb_put(o, q + i, j - i); i = j; continue;
        }
        if (x_idstart(ch)) {
            size_t s = i, b2;
            int a, pasted;
            while (i < bl && (isalnum((unsigned char)q[i]) || q[i] == '_')) i++;
            a = x_param(m, q + s, i - s);
            if (a < 0) { xsb_put(o, q + s, i - s); continue; }
            b2 = i; while (b2 < bl && (q[b2] == ' ' || q[b2] == '\t')) b2++;
            pasted = (b2 + 1 < bl && q[b2] == '#' && q[b2 + 1] == '#');    /* operando izquierdo de un `##` */
            xsb_put(o, pasted ? raw[a] : exp[a], strlen(pasted ? raw[a] : exp[a]));
            continue;
        }
        xsb_c(o, (char)ch); i++;
    }
}

/* Argumentos de `( a, b )` desde in[j] == '('. Devuelve la posicion tras el ')' o 0 si no cierra. Cuenta en *na; si as/ae no son
 * NULL, guarda el inicio y el fin de cada uno (deben tener *na huecos: primero se llama sin ellos para contar). */
static size_t x_args(const char* in, size_t j, size_t len, size_t* as, size_t* ae, int* na)
{
    int depth = 0, n = 0;
    size_t i;
    if (as) as[0] = j + 1;
    for (i = j + 1; i < len; i++) {
        char ch = in[i];
        if (ch == '(') depth++;
        else if (ch == ')') {
            if (!depth) { if (ae) ae[n] = i; *na = n + 1; return i + 1; }
            depth--;
        } else if (ch == ',' && !depth) {
            if (ae) { ae[n] = i; as[n + 1] = i + 1; }
            n++;
        }
    }
    return 0;
}

static void xexpand(xctx_t* c, const char* in, size_t len);

/* Llamada a una macro de funcion: in[j] es el '('. Devuelve la posicion tras la llamada, o 0 si no es una llamada valida (el
 * nombre se deja tal cual). */
static size_t x_call(xctx_t* c, const xm_t* m, const char* in, size_t len, size_t j)
{
    xsb_t* o = c->out;
    size_t *as, *ae, end, k;
    int na = 0, a, ok = 1, nl = 0, nargs;
    char **raw, **exp;
    xsb_t sub = {0, 0, 0, 0, 0};
    end = x_args(in, j, len, NULL, NULL, &na);
    if (!end) return 0;
    if (m->np == 0) {                               /* F(): un unico argumento, vacio */
        if (na != 1) return 0;
        for (k = j + 1; k + 1 < end; k++) if (!x_blank(in[k])) return 0;
        nargs = 0;
    } else if (na != m->np) return 0;
    else nargs = na;
    as = (size_t*)malloc(sizeof *as * (size_t)(na + 1)); ae = (size_t*)malloc(sizeof *ae * (size_t)(na + 1));
    raw = (char**)calloc((size_t)(na + 1), sizeof *raw); exp = (char**)calloc((size_t)(na + 1), sizeof *exp);
    if (!as || !ae || !raw || !exp) { free(as); free(ae); free(raw); free(exp); o->oom = 1; return 0; }
    x_args(in, j, len, as, ae, &na);
    for (a = 0; a < nargs && ok; a++) {
        xsb_t ax = {0, 0, 0, 0, 0};
        xctx_t sc = *c;
        size_t s0 = as[a], l = ae[a] - as[a];
        while (l && x_blank(in[s0])) { s0++; l--; }
        while (l && x_blank(in[s0 + l - 1])) l--;
        raw[a] = xdup(in + s0, l);
        if (raw[a]) for (k = 0; k < l; k++) if (raw[a][k] == '\n' || raw[a][k] == '\r') raw[a][k] = ' ';   /* los \n se reponen al final */
        ax.limit = o->limit;
        sc.out = &ax;
        xexpand(&sc, raw[a] ? raw[a] : "", raw[a] ? l : 0);
        exp[a] = ax.p ? ax.p : xdup("", 0);
        if (!raw[a] || !exp[a] || ax.oom) ok = 0;
    }
    if (ok) {
        sub.limit = o->limit;
        x_substitute(m, raw, exp, &sub);
        if (sub.oom) ok = 0;
    }
    if (ok) {
        if (c->depth < c->scap) {
            c->stack[c->depth++] = m;
            xexpand(c, sub.p ? sub.p : "", sub.n);
            c->depth--;
        } else xsb_put(o, sub.p ? sub.p : "", sub.n);          /* no ocurre: la pila cabe en tantas macros como hay */
    } else o->oom = 1;
    for (a = 0; a < nargs; a++) { free(raw[a]); free(exp[a]); }
    free(raw); free(exp); free(as); free(ae); free(sub.p);
    for (k = j; k < end; k++) if (in[k] == '\n') nl++;
    while (nl--) { xsb_c(o, '\n'); if (!c->depth) c->line++; }
    return end;
}

static void xexpand(xctx_t* c, const char* in, size_t len)
{
    xsb_t* o = c->out;
    size_t i = 0;
    while (i < len && !o->oom) {
        unsigned char ch = (unsigned char)in[i];
        if (ch == '\n') { if (!c->depth) c->line++; xsb_c(o, '\n'); i++; continue; }
        if (isdigit(ch) || (ch == '.' && i + 1 < len && isdigit((unsigned char)in[i + 1]))) {
            size_t j = x_number(in, i, len);
            xsb_put(o, in + i, j - i); i = j; continue;
        }
        if (x_idstart(ch)) {
            size_t s = i, j, e;
            xm_t* m;
            while (i < len && (isalnum((unsigned char)in[i]) || in[i] == '_')) i++;
            if (i - s == 8 && !strncmp(in + s, "__LINE__", 8) && !xm_find(c->tab, "__LINE__", 8)) {
                char b[24]; snprintf(b, sizeof b, "%d", c->line); xsb_put(o, b, strlen(b)); continue;
            }
            if (i - s == 8 && !strncmp(in + s, "__FILE__", 8) && !xm_find(c->tab, "__FILE__", 8)) { xsb_c(o, '0'); continue; }
            m = xm_find(c->tab, in + s, i - s);
            if (!m || m->skip || x_on_stack(c, m) || c->depth >= c->scap) { xsb_put(o, in + s, i - s); continue; }
            if (!m->fn) {
                c->stack[c->depth++] = m;
                xexpand(c, m->body, strlen(m->body));
                c->depth--;
                continue;
            }
            j = i;
            while (j < len && x_blank(in[j])) j++;
            if (j >= len || in[j] != '(') { xsb_put(o, in + s, i - s); continue; }   /* sin '(' no es una llamada */
            e = x_call(c, m, in, len, j);
            if (!e) { xsb_put(o, in + s, i - s); continue; }
            i = e;
            continue;
        }
        xsb_c(o, (char)ch); i++;
    }
}

/* Fin de la linea logica que empieza en `ls` (una directiva continua con \ al final). Apunta al \n o al \0. */
static const char* x_line_end(const char* ls)
{
    const char* d = ls;
    const char* le = ls;
    while (*d == ' ' || *d == '\t') d++;
    for (;;) {
        while (*le && *le != '\n') le++;
        if (*d == '#' && le > ls && *le && (le[-1] == '\\' || (le[-1] == '\r' && le > ls + 1 && le[-2] == '\\'))) { le++; continue; }
        break;
    }
    return le;
}

static void x_free_tab(xtab_t* t)
{
    int i;
    for (i = 0; i < t->n; i++) xm_free(&t->m[i]);
    free(t->m);
}

/* `#define nombre[(params)] cuerpo` sobre la linea logica [a, le), con `a` tras la palabra define. 0 solo sin memoria. */
static int x_parse_define(xtab_t* t, const char* a, const char* le)
{
    size_t tl = (size_t)(le - a), k, n, bl;
    char* tmp = (char*)malloc(tl + 1);
    const char* id; const char* v;
    char** par = NULL; int np = 0, fn = 0, skip = 0, ok = 1, i;
    if (!tmp) return 0;
    for (k = 0; k < tl; k++) {
        if (a[k] == '\\' && k + 1 < tl && a[k + 1] == '\n') { tmp[k] = ' '; tmp[k + 1] = ' '; k++; }
        else if (a[k] == '\\' && k + 2 < tl && a[k + 1] == '\r' && a[k + 2] == '\n') { tmp[k] = tmp[k + 1] = tmp[k + 2] = ' '; k += 2; }
        else tmp[k] = (a[k] == '\n' || a[k] == '\r') ? ' ' : a[k];
    }
    tmp[tl] = 0;
    id = tmp;
    while (*id == ' ' || *id == '\t') id++;
    n = 0; while (is_id((unsigned char)id[n])) n++;
    if (!n || isdigit((unsigned char)id[0])) { free(tmp); return 1; }       /* no es un nombre valido: no se registra */
    v = id + n;
    if (*v == '(') {
        fn = 1; v++;
        for (;;) {
            size_t pl;
            while (*v == ' ' || *v == '\t') v++;
            if (*v == ')') { v++; break; }
            pl = 0; while (is_id((unsigned char)v[pl])) pl++;
            if (!pl) { skip = 1; while (*v && *v != ')') v++; if (*v) v++; break; }      /* `...` u otra cosa: no se expande */
            {
                char** np2 = (char**)realloc(par, (size_t)(np + 1) * sizeof *par);
                if (!np2) { ok = 0; break; }
                par = np2;
                par[np] = xdup(v, pl);
                if (!par[np]) { ok = 0; break; }
                np++;
            }
            v += pl;
            while (*v == ' ' || *v == '\t') v++;
            if (*v == ',') v++;
            else if (*v == ')') { v++; break; }
            else { skip = 1; break; }
        }
    }
    while (*v == ' ' || *v == '\t') v++;
    bl = strlen(v);
    while (bl && (v[bl - 1] == ' ' || v[bl - 1] == '\t')) bl--;
    if (ok) ok = xm_define(t, id, n, par, np, fn, skip, v, bl);
    if (!ok) { for (i = 0; i < np; i++) free(par[i]); free(par); }
    free(tmp);
    return ok;
}

/* ------------------------------------------------------------------------------------------------ condiciones (#if / #elif)
 * Se evalua sobre el texto ya expandido: lo que queda son numeros, operadores e identificadores sin definir (= 0). Un identificador
 * GL_xxx vale 1 si el driver anuncia esa extension (como el compilador de un driver real). */
typedef struct { const char* p; int err; } pp_ev_t;

static void pp_ws(pp_ev_t* e) { while (*e->p == ' ' || *e->p == '\t' || *e->p == '\r' || *e->p == '\n') e->p++; }

static int pp_ext_defined(const char* nm, size_t len)
{
    char buf[96];
    if (len > 3 && len < sizeof buf && nm[0] == 'G' && nm[1] == 'L' && nm[2] == '_') {
        memcpy(buf, nm, len); buf[len] = 0;
        return gl31_ext_advertised(buf);
    }
    return 0;
}

static long long pp_ternary(pp_ev_t* e);
static long long pp_expr(pp_ev_t* e, int minprec);

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
        const char* st = e->p;
        while (is_id((unsigned char)*e->p)) e->p++;
        return pp_ext_defined(st, (size_t)(e->p - st));         /* sin definir vale 0; una extension anunciada, 1 */
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

static int x_defined(xtab_t* t, const char* nm, size_t len) { return xm_find(t, nm, len) != NULL || pp_ext_defined(nm, len); }

/* Evalua el cuerpo de un #if / #elif: `defined` primero (sin expandir su operando), luego la expansion de macros, luego el calculo.
 * *ok = 0 si no se pudo (sintaxis rota, falta de memoria). */
static int x_cond(xctx_t* c, const char* body, int* ok)
{
    xsb_t d = {0, 0, 0, 0, 0}, x = {0, 0, 0, 0, 0};
    xctx_t sc = *c;
    pp_ev_t e;
    const char* p = body;
    long long v = 0;
    d.limit = x.limit = c->out->limit;
    while (*p) {
        if (x_idstart((unsigned char)*p)) {
            const char* s = p;
            while (is_id((unsigned char)*p)) p++;
            if (p - s == 7 && !strncmp(s, "defined", 7)) {
                const char* q = p;
                int paren = 0;
                const char* id;
                size_t n;
                while (*q == ' ' || *q == '\t') q++;
                if (*q == '(') { paren = 1; q++; while (*q == ' ' || *q == '\t') q++; }
                id = q;
                while (is_id((unsigned char)*q)) q++;
                n = (size_t)(q - id);
                if (!n) { *ok = 0; free(d.p); free(x.p); return 0; }
                if (paren) {
                    while (*q == ' ' || *q == '\t') q++;
                    if (*q != ')') { *ok = 0; free(d.p); free(x.p); return 0; }
                    q++;
                }
                xsb_c(&d, x_defined(c->tab, id, n) ? '1' : '0');
                p = q;
            } else xsb_put(&d, s, (size_t)(p - s));
        } else if (isdigit((unsigned char)*p)) { size_t j = x_number(p, 0, strlen(p)); xsb_put(&d, p, j); p += j; }
        else { xsb_c(&d, *p); p++; }
    }
    sc.out = &x; sc.depth = 0; sc.line = 0;
    xexpand(&sc, d.p ? d.p : "", d.n);
    if (d.oom || x.oom) { *ok = 0; free(d.p); free(x.p); return 0; }
    e.p = x.p ? x.p : ""; e.err = 0;
    v = pp_ternary(&e);
    pp_ws(&e);
    *ok = !e.err && !*e.p;
    free(d.p); free(x.p);
    return v != 0;
}

/* ------------------------------------------------------------------------------------------------ pasada unica */
typedef struct { int parent, taken, active; } pp_frame_t;

static void blank_range(xsb_t* out, const char* a, const char* b)       /* espacios, conservando los \n */
{
    for (; a < b; a++) xsb_c(out, *a == '\n' ? '\n' : ' ');
}

char* gl31_glsl_preprocess(const char* s, long ver, const char** why)
{
    xtab_t tab = {0, 0, 0};
    xsb_t out = {0, 0, 0, 0, 0};
    xctx_t c;
    pp_frame_t* st = NULL;
    int sp = 0, scap = 0, active = 1, ok = 1, line = 1;
    const char* p = s;
    const char* reason = NULL;
    char vbuf[24];
    size_t k, hashes = 0;
    memset(&c, 0, sizeof c);
    for (k = 0; s[k]; k++) if (s[k] == '#') hashes++;
    out.limit = strlen(s) * 8 + ((size_t)1 << 20);
    c.scap = (int)(hashes + 3);                    /* una macro no puede estar dos veces en la pila: cabe una por #define */
    c.stack = (const xm_t**)malloc(sizeof *c.stack * (size_t)c.scap);
    if (!c.stack) { if (why) *why = "out of memory"; return NULL; }
    snprintf(vbuf, sizeof vbuf, "%ld", ver);
    if (!xm_define(&tab, "__VERSION__", 11, NULL, 0, 0, 0, vbuf, strlen(vbuf))) { free(c.stack); x_free_tab(&tab); if (why) *why = "out of memory"; return NULL; }
    c.tab = &tab; c.out = &out;

    while (*p && ok) {
        const char* d = p;
        const char* le;
        while (*d == ' ' || *d == '\t') d++;
        if (*d == '#') {
            const char* a = d + 1;
            char dir[16]; size_t dn = 0;
            char* body; size_t bl = 0;
            const char* b;
            le = x_line_end(p);
            while (*a == ' ' || *a == '\t') a++;
            while (is_id((unsigned char)*a) && dn < sizeof dir - 1) dir[dn++] = *a++;
            dir[dn] = 0;
            body = (char*)malloc((size_t)(le - a) + 1);
            if (!body) { ok = 0; reason = "out of memory"; break; }
            for (b = a; b < le; b++)               /* cuerpo sin las continuaciones `\` + salto */
                body[bl++] = (*b == '\\' && (b[1] == '\n' || (b[1] == '\r' && b[2] == '\n'))) ? ' ' : (*b == '\n' || *b == '\r') ? ' ' : *b;
            body[bl] = 0;
            if (!strcmp(dir, "if") || !strcmp(dir, "ifdef") || !strcmp(dir, "ifndef")) {
                int cond = 0;
                if (sp == scap) {
                    pp_frame_t* nf = (pp_frame_t*)realloc(st, sizeof *st * (size_t)(scap ? scap * 2 : 16));
                    if (!nf) { ok = 0; reason = "out of memory"; free(body); break; }
                    st = nf; scap = scap ? scap * 2 : 16;
                }
                st[sp].parent = active;
                if (active) {
                    if (dir[2] == 0) { cond = x_cond(&c, body, &ok); if (!ok) reason = "cannot evaluate an #if expression"; }
                    else {
                        const char* id = body; size_t n = 0;
                        while (*id == ' ' || *id == '\t') id++;
                        while (is_id((unsigned char)id[n])) n++;
                        if (!n) { ok = 0; reason = "#ifdef without a name"; }
                        cond = x_defined(&tab, id, n);
                        if (dir[2] == 'n') cond = !cond;
                    }
                }
                st[sp].taken = active && cond;
                active = active && cond;
                st[sp].active = active;
                sp++;
                blank_range(&out, p, le);
            } else if (!strcmp(dir, "elif")) {
                if (!sp) { ok = 0; reason = "#elif without #if"; }
                else {
                    pp_frame_t* f = &st[sp - 1];
                    if (!f->parent || f->taken) active = 0;
                    else { int cond = x_cond(&c, body, &ok); if (!ok) reason = "cannot evaluate an #elif expression"; active = cond; f->taken = cond; }
                }
                blank_range(&out, p, le);
            } else if (!strcmp(dir, "else")) {
                if (!sp) { ok = 0; reason = "#else without #if"; }
                else { pp_frame_t* f = &st[sp - 1]; active = f->parent && !f->taken; f->taken = 1; }
                blank_range(&out, p, le);
            } else if (!strcmp(dir, "endif")) {
                if (!sp) { ok = 0; reason = "#endif without #if"; }
                else active = st[--sp].parent;
                blank_range(&out, p, le);
            } else if (!active) {
                blank_range(&out, p, le);
            } else {
                if (!strcmp(dir, "define")) { if (!x_parse_define(&tab, a, le)) { ok = 0; reason = "out of memory"; } }
                else if (!strcmp(dir, "undef")) {
                    const char* id = body; size_t n = 0;
                    while (*id == ' ' || *id == '\t') id++;
                    while (is_id((unsigned char)id[n])) n++;
                    xm_undef(&tab, id, n);
                }
                xsb_put(&out, p, (size_t)(le - p));   /* las demas directivas pasan intactas */
            }
            free(body);
            for (k = 0; k < (size_t)(le - p); k++) if (p[k] == '\n') line++;
            if (*le == '\n') { xsb_c(&out, '\n'); line++; le++; }
            p = le;
        } else {
            /* tramo de codigo hasta la siguiente directiva (una llamada a macro puede ocupar varias lineas) */
            const char* e = p;
            for (;;) {
                const char* q;
                while (*e && *e != '\n') e++;
                if (*e == '\n') e++;
                q = e; while (*q == ' ' || *q == '\t') q++;
                if (!*e || *q == '#') break;
            }
            if (active) {
                c.line = line; c.depth = 0;
                xexpand(&c, p, (size_t)(e - p));
                line = c.line;
            } else {
                blank_range(&out, p, e);
                for (k = 0; k < (size_t)(e - p); k++) if (p[k] == '\n') line++;
            }
            p = e;
        }
        if (out.oom && ok) { ok = 0; reason = "output too large (runaway macro expansion?)"; }
    }
    if (ok && sp) { ok = 0; reason = "#if without #endif"; }
    free(st); free(c.stack);
    x_free_tab(&tab);
    if (!ok) { free(out.p); if (why) *why = reason ? reason : "preprocessing failed"; return NULL; }
    if (!out.p) out.p = xdup("", 0);
    return out.p;
}
