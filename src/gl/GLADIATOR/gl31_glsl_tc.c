/* gl31_glsl_tc.c - front end tipado de GLSL para las conversiones implicitas.
 *
 * El GLSL de escritorio (>= 1.20, y todavia mas en 4.x) convierte solo int -> uint -> float en operadores, asignaciones,
 * inicializadores, return, ?: y argumentos de funciones. GLSL ES 3.x no convierte nada. Este modulo lee el shader de
 * escritorio con un parser de verdad (declaraciones, structs, bloques de interfaz, funciones, sentencias y expresiones con su
 * precedencia), calcula el tipo de cada expresion con tabla de simbolos y de funciones, y donde la regla de escritorio
 * convierte inserta la conversion explicita:
 *     literal entero a float/uint   ->  sufijo ".0" / "u"
 *     cualquier otra expresion      ->  float(e), uvec3(e), ...
 * El resultado son inserciones de texto sobre el fuente original (nada se regenera), asi que el resto del shader, sus
 * saltos de linea y sus comentarios quedan intactos.
 *
 * Regla de seguridad: un tipo desconocido (macro sin expandir, identificador no declarado, funcion no reconocida) nunca produce
 * una conversion; si el parser no entiende una declaracion, la salta y sigue con la siguiente. Las inserciones ya hechas siguen
 * siendo validas porque cada una se decide con tipos conocidos. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

char* gl31_glsl_implicit_conv(const char* src);

/* ------------------------------------------------------------------------------------------------------------------ tokens */
enum { TK_EOF, TK_ID, TK_INT, TK_FLT, TK_PUN };
#define TF_UNS 1      /* entero con sufijo u */
#define TF_PLAIN 2    /* entero decimal sin sufijo ni cero inicial: admite el sufijo ".0" / "u" */
#define TF_DBL 4      /* flotante con sufijo lf */

typedef struct { int k; size_t b, e; int flags; } tok_t;

/* ------------------------------------------------------------------------------------------------------------------ tipos */
enum { B_NONE, B_BOOL, B_INT, B_UINT, B_FLOAT };
enum { K_UNK, K_VOID, K_NUM, K_SMP, K_STRUCT };
typedef struct { unsigned char k, b, n, m, arr, sb, shadow, sd; short sid; } ty_t;

static ty_t t_unk(void) { ty_t t; memset(&t, 0, sizeof t); return t; }
static ty_t t_num(int b, int n, int m) { ty_t t = t_unk(); t.k = K_NUM; t.b = (unsigned char)b; t.n = (unsigned char)n; t.m = (unsigned char)m; return t; }
static int is_num(ty_t t) { return t.k == K_NUM && !t.arr; }
static int rank_of(int b) { return b == B_INT ? 1 : b == B_UINT ? 2 : b == B_FLOAT ? 3 : 0; }
static int same_shape(ty_t a, ty_t b) { return a.n == b.n && a.m == b.m; }
/* la conversion implicita de escritorio: solo hacia arriba (int -> uint -> float), misma forma */
static int can_up(ty_t from, ty_t to)
{
    return is_num(from) && is_num(to) && same_shape(from, to) && rank_of(from.b) > 0 && rank_of(from.b) < rank_of(to.b);
}
static int same_type(ty_t a, ty_t b)
{
    if (a.k != b.k || a.arr != b.arr) return 0;
    if (a.k == K_NUM) return a.b == b.b && a.n == b.n && a.m == b.m;
    if (a.k == K_STRUCT) return a.sid == b.sid;
    if (a.k == K_SMP) return a.sb == b.sb && a.shadow == b.shadow && a.sd == b.sd;
    return 1;
}

static void ctor_name(ty_t t, char* out)
{
    static const char* const pre[] = { "", "b", "i", "u", "" };
    const char* base = t.b == B_BOOL ? "bool" : t.b == B_INT ? "int" : t.b == B_UINT ? "uint" : "float";
    if (t.m == 0 && t.n == 1) { strcpy(out, base); return; }
    if (t.m == 0) { sprintf(out, "%svec%d", pre[t.b], t.n); return; }
    if (t.m == t.n) sprintf(out, "mat%d", t.m); else sprintf(out, "mat%dx%d", t.m, t.n);
}

/* ------------------------------------------------------------------------------------------------------------------ estado */
typedef struct { size_t pos; int grp; int seq; char txt[12]; } edit_t;
typedef struct { const char* n; size_t l; ty_t t; int depth; } sym_t;
typedef struct { const char* n; size_t l; ty_t t; } field_t;
typedef struct { const char* n; size_t l; int nf; field_t* f; } struct_t;
#define MAXPAR 12
#define MAXARG 24
typedef struct { const char* n; size_t l; ty_t ret; int np; ty_t pt[MAXPAR]; unsigned char out[MAXPAR]; } func_t;

typedef struct {
    const char* src; size_t len;
    tok_t* tk; int nt, capt;
    int cur;
    edit_t* ed; int ne, cape, seq;
    sym_t* sy; int ns, caps, depth;
    struct_t* st; int nst, capst;
    func_t* fn; int nfn, capfn;
    ty_t cur_ret;
    int oom;
} P;

typedef struct { ty_t t; int tb, te; int lit; } ex_t;     /* tb..te: tokens [tb, te); lit: indice+1 del token si es un literal entero */

static void* grow(void* a, int n, int* cap, size_t sz, int* oom)
{
    int nc;
    void* r;
    if (n < *cap) return a;
    nc = *cap ? *cap * 2 : 64;
    r = realloc(a, (size_t)nc * sz);
    if (!r) { *oom = 1; return a; }
    *cap = nc;
    return r;
}

/* ------------------------------------------------------------------------------------------------------------------ lexer */
static void push_tok(P* p, int k, size_t b, size_t e, int flags)
{
    p->tk = (tok_t*)grow(p->tk, p->nt, &p->capt, sizeof(tok_t), &p->oom);
    if (p->oom) return;
    p->tk[p->nt].k = k; p->tk[p->nt].b = b; p->tk[p->nt].e = e; p->tk[p->nt].flags = flags;
    p->nt++;
}

static void lex(P* p)
{
    const char* s = p->src;
    size_t i = 0, n = p->len;
    int bol = 1;
    while (i < n && !p->oom) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') { bol = 1; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { while (i < n && s[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++; i += 2; continue; }
        if (c == '#' && bol) {                              /* directiva: linea entera (con continuaciones) */
            while (i < n && s[i] != '\n') { if (s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') i++; i++; }
            continue;
        }
        bol = 0;
        if (isalpha(c) || c == '_') {
            size_t b = i;
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
            push_tok(p, TK_ID, b, i, 0);
            continue;
        }
        if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            size_t b = i;
            int isf = 0, hex = 0, flags = 0;
            if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                hex = 1; i += 2;
                while (i < n && isxdigit((unsigned char)s[i])) i++;
            } else {
                while (i < n && isdigit((unsigned char)s[i])) i++;
                if (i < n && s[i] == '.') { isf = 1; i++; while (i < n && isdigit((unsigned char)s[i])) i++; }
                if (i < n && (s[i] == 'e' || s[i] == 'E') &&
                    (isdigit((unsigned char)s[i + 1]) || ((s[i + 1] == '+' || s[i + 1] == '-') && isdigit((unsigned char)s[i + 2])))) {
                    isf = 1; i += 2;
                    while (i < n && isdigit((unsigned char)s[i])) i++;
                }
            }
            {
                size_t sfx = i;
                while (i < n && isalpha((unsigned char)s[i])) {
                    if (s[i] == 'u' || s[i] == 'U') flags |= TF_UNS;
                    if (s[i] == 'l' || s[i] == 'L') flags |= TF_DBL;
                    i++;
                }
                if (isf) { flags &= ~TF_UNS; if (!(flags & TF_DBL)) flags = 0; else flags = TF_DBL; push_tok(p, TK_FLT, b, i, flags); }
                else {
                    int plain = !hex && i == sfx && !(c == '0' && sfx - b > 1);
                    flags &= TF_UNS;
                    if (plain) flags |= TF_PLAIN;
                    push_tok(p, TK_INT, b, i, flags);
                }
            }
            continue;
        }
        {
            static const char* const p3[] = { "<<=", ">>=", NULL };
            static const char* const p2[] = { "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "^^", NULL };
            size_t l = 1;
            int k;
            for (k = 0; p3[k]; k++) if (i + 3 <= n && !memcmp(s + i, p3[k], 3)) l = 3;
            if (l == 1) for (k = 0; p2[k]; k++) if (i + 2 <= n && !memcmp(s + i, p2[k], 2)) { l = 2; break; }
            push_tok(p, TK_PUN, i, i + l, 0);
            i += l;
        }
    }
    push_tok(p, TK_EOF, n, n, 0);
    push_tok(p, TK_EOF, n, n, 0);
}

/* ------------------------------------------------------------------------------------------------------------------ utilidades */
static int tx(const P* p, int i, const char* w)
{
    size_t l = strlen(w);
    const tok_t* t = &p->tk[i];
    return t->e - t->b == l && !memcmp(p->src + t->b, w, l);
}
static int is_p(const P* p, const char* w) { return p->tk[p->cur].k == TK_PUN && tx(p, p->cur, w); }
static int is_pn(const P* p, int i, const char* w) { return p->tk[i].k == TK_PUN && tx(p, i, w); }
static int is_kw(const P* p, const char* w) { return p->tk[p->cur].k == TK_ID && tx(p, p->cur, w); }
static int is_kwn(const P* p, int i, const char* w) { return p->tk[i].k == TK_ID && tx(p, i, w); }
static int is_idn(const P* p, int i) { return p->tk[i].k == TK_ID; }
static int expect(P* p, const char* w) { if (!is_p(p, w)) return -1; p->cur++; return 0; }
static int tk_eq(const P* p, int i, const char* s, size_t l)
{
    return p->tk[i].e - p->tk[i].b == l && !memcmp(p->src + p->tk[i].b, s, l);
}

static void add_edit(P* p, size_t pos, int grp, const char* txt)
{
    edit_t* e;
    p->ed = (edit_t*)grow(p->ed, p->ne, &p->cape, sizeof(edit_t), &p->oom);
    if (p->oom) return;
    e = &p->ed[p->ne++];
    e->pos = pos; e->grp = grp; e->seq = p->seq++;
    strncpy(e->txt, txt, sizeof e->txt - 1); e->txt[sizeof e->txt - 1] = 0;
}

/* conversion explicita de x al tipo `to` (que can_up ya aprobo) */
static void conv(P* p, ex_t* x, ty_t to)
{
    char nm[24];
    if (x->lit && to.n == 1 && to.m == 0 && (to.b == B_FLOAT || to.b == B_UINT)) {
        add_edit(p, p->tk[x->lit - 1].e, 0, to.b == B_FLOAT ? ".0" : "u");
    } else {
        ctor_name(to, nm);
        strcat(nm, "(");
        add_edit(p, p->tk[x->tb].b, 1, nm);
        add_edit(p, p->tk[x->te - 1].e, 0, ")");
    }
    x->t = to;
    x->lit = 0;
}
static void conv_if(P* p, ex_t* x, ty_t to) { if (can_up(x->t, to)) conv(p, x, to); }

/* ------------------------------------------------------------------------------------------------------------------ simbolos */
static void declare(P* p, const char* n, size_t l, ty_t t)
{
    p->sy = (sym_t*)grow(p->sy, p->ns, &p->caps, sizeof(sym_t), &p->oom);
    if (p->oom) return;
    p->sy[p->ns].n = n; p->sy[p->ns].l = l; p->sy[p->ns].t = t; p->sy[p->ns].depth = p->depth;
    p->ns++;
}
static void push_scope(P* p) { p->depth++; }
static void pop_scope(P* p)
{
    while (p->ns > 0 && p->sy[p->ns - 1].depth >= p->depth) p->ns--;
    if (p->depth > 0) p->depth--;
}
static void pop_to_global(P* p) { while (p->depth > 0) pop_scope(p); }

static int find_struct(const P* p, const char* n, size_t l)
{
    int i;
    for (i = p->nst - 1; i >= 0; i--) if (p->st[i].n && p->st[i].l == l && !memcmp(p->st[i].n, n, l)) return i;
    return -1;
}

static const struct { const char* n; int b; int nn; int arr; } k_bvars[] = {
    { "gl_VertexID", B_INT, 1, 0 }, { "gl_InstanceID", B_INT, 1, 0 }, { "gl_PrimitiveID", B_INT, 1, 0 }, { "gl_PrimitiveIDIn", B_INT, 1, 0 },
    { "gl_InvocationID", B_INT, 1, 0 }, { "gl_SampleID", B_INT, 1, 0 }, { "gl_Layer", B_INT, 1, 0 }, { "gl_ViewportIndex", B_INT, 1, 0 },
    { "gl_NumSamples", B_INT, 1, 0 }, { "gl_BaseVertex", B_INT, 1, 0 }, { "gl_BaseInstance", B_INT, 1, 0 }, { "gl_DrawID", B_INT, 1, 0 },
    { "gl_LocalInvocationIndex", B_UINT, 1, 0 }, { "gl_LocalInvocationID", B_UINT, 3, 0 }, { "gl_GlobalInvocationID", B_UINT, 3, 0 },
    { "gl_WorkGroupID", B_UINT, 3, 0 }, { "gl_NumWorkGroups", B_UINT, 3, 0 }, { "gl_WorkGroupSize", B_UINT, 3, 0 },
    { "gl_Position", B_FLOAT, 4, 0 }, { "gl_FragCoord", B_FLOAT, 4, 0 }, { "gl_FragColor", B_FLOAT, 4, 0 }, { "gl_FragData", B_FLOAT, 4, 1 },
    { "gl_FragDepth", B_FLOAT, 1, 0 }, { "gl_PointSize", B_FLOAT, 1, 0 }, { "gl_PointCoord", B_FLOAT, 2, 0 }, { "gl_FrontFacing", B_BOOL, 1, 0 },
    { "gl_ClipDistance", B_FLOAT, 1, 1 }, { "gl_CullDistance", B_FLOAT, 1, 1 }, { NULL, 0, 0, 0 }
};

static ty_t lookup_var(const P* p, int tokidx)
{
    int i, k;
    const char* n = p->src + p->tk[tokidx].b;
    size_t l = p->tk[tokidx].e - p->tk[tokidx].b;
    for (i = p->ns - 1; i >= 0; i--) if (p->sy[i].l == l && !memcmp(p->sy[i].n, n, l)) return p->sy[i].t;
    for (k = 0; k_bvars[k].n; k++)
        if (strlen(k_bvars[k].n) == l && !memcmp(k_bvars[k].n, n, l)) { ty_t t = t_num(k_bvars[k].b, k_bvars[k].nn, 0); t.arr = (unsigned char)k_bvars[k].arr; return t; }
    return t_unk();
}

/* ------------------------------------------------------------------------------------------------------------------ nombres de tipo */
/* sampler / image / atomic_uint: devuelve 1 si el nombre es de un tipo opaco (t = sampler conocido o desconocido) */
static int parse_opaque(const char* s, size_t l, ty_t* t)
{
    char b[32];
    const char* q;
    int sb = B_FLOAT, sd = 0, shadow = 0;
    if (l >= sizeof b) return 0;
    memcpy(b, s, l); b[l] = 0;
    if (!strcmp(b, "atomic_uint")) { *t = t_unk(); return 1; }
    q = b;
    if (!strncmp(q, "image", 5) || !strncmp(q, "iimage", 6) || !strncmp(q, "uimage", 6)) { *t = t_unk(); return 1; }
    if (*q == 'i') { sb = B_INT; q++; } else if (*q == 'u') { sb = B_UINT; q++; }
    if (strncmp(q, "sampler", 7)) return 0;
    q += 7;
    {
        size_t ql = strlen(q);
        if (ql >= 6 && !strcmp(q + ql - 6, "Shadow")) { shadow = 1; b[l - 6] = 0; }
    }
    if (!strcmp(q, "1D")) sd = 1; else if (!strcmp(q, "2D")) sd = 2; else if (!strcmp(q, "3D")) sd = 3;
    else if (!strcmp(q, "Cube")) sd = 2; else if (!strcmp(q, "2DRect")) sd = 2; else if (!strcmp(q, "1DArray")) sd = 2;
    else if (!strcmp(q, "2DArray")) sd = 3; else if (!strcmp(q, "CubeArray")) sd = 3; else if (!strcmp(q, "Buffer")) sd = 1;
    else if (!strcmp(q, "2DMS")) sd = 2; else if (!strcmp(q, "2DMSArray")) sd = 3;
    else { *t = t_unk(); return 1; }
    *t = t_unk(); t->k = K_SMP; t->sb = (unsigned char)sb; t->shadow = (unsigned char)shadow; t->sd = (unsigned char)sd;
    return 1;
}

/* nombre de tipo integrado (escalares, vectores, matrices, void, opacos). 1 = es un tipo */
static int builtin_type(const char* s, size_t l, ty_t* t)
{
    char b[32];
    if (l == 0 || l >= sizeof b) return 0;
    memcpy(b, s, l); b[l] = 0;
    if (!strcmp(b, "void")) { *t = t_unk(); t->k = K_VOID; return 1; }
    if (!strcmp(b, "bool")) { *t = t_num(B_BOOL, 1, 0); return 1; }
    if (!strcmp(b, "int")) { *t = t_num(B_INT, 1, 0); return 1; }
    if (!strcmp(b, "uint")) { *t = t_num(B_UINT, 1, 0); return 1; }
    if (!strcmp(b, "float")) { *t = t_num(B_FLOAT, 1, 0); return 1; }
    if (!strcmp(b, "double") || !strncmp(b, "dvec", 4) || !strncmp(b, "dmat", 4)) { *t = t_unk(); return 1; }
    if (l == 5 && !strncmp(b + 1, "vec", 3) && b[4] >= '2' && b[4] <= '4' && (b[0] == 'b' || b[0] == 'i' || b[0] == 'u'))
        { *t = t_num(b[0] == 'b' ? B_BOOL : b[0] == 'i' ? B_INT : B_UINT, b[4] - '0', 0); return 1; }
    if (l == 4 && !strncmp(b, "vec", 3) && b[3] >= '2' && b[3] <= '4') { *t = t_num(B_FLOAT, b[3] - '0', 0); return 1; }
    if (l == 4 && !strncmp(b, "mat", 3) && b[3] >= '2' && b[3] <= '4') { *t = t_num(B_FLOAT, b[3] - '0', b[3] - '0'); return 1; }
    if (l == 6 && !strncmp(b, "mat", 3) && b[3] >= '2' && b[3] <= '4' && b[4] == 'x' && b[5] >= '2' && b[5] <= '4')
        { *t = t_num(B_FLOAT, b[5] - '0', b[3] - '0'); return 1; }                 /* matCxR: C columnas, R filas */
    return parse_opaque(b, l, t);
}

static int type_name_at(const P* p, int i, ty_t* t)
{
    int sid;
    const char* n;
    size_t l;
    if (!is_idn(p, i)) return 0;
    n = p->src + p->tk[i].b; l = p->tk[i].e - p->tk[i].b;
    if (builtin_type(n, l, t)) return 1;
    sid = find_struct(p, n, l);
    if (sid >= 0) { *t = t_unk(); t->k = K_STRUCT; t->sid = (short)sid; return 1; }
    return 0;
}

static int skip_balanced(P* p, const char* open, const char* close)
{
    int d = 0;
    for (;;) {
        if (p->tk[p->cur].k == TK_EOF) return -1;
        if (is_p(p, open)) d++;
        else if (is_p(p, close)) { d--; if (d == 0) { p->cur++; return 0; } }
        p->cur++;
    }
}

/* ------------------------------------------------------------------------------------------------------------------ funciones */
static int is_quals_word(const P* p, int i)
{
    static const char* const q[] = { "const", "in", "out", "inout", "uniform", "buffer", "shared", "attribute", "varying", "centroid", "flat",
                                     "smooth", "noperspective", "patch", "sample", "invariant", "precise", "highp", "mediump", "lowp",
                                     "coherent", "volatile", "restrict", "readonly", "writeonly", NULL };
    int k;
    if (!is_idn(p, i)) return 0;
    for (k = 0; q[k]; k++) if (tx(p, i, q[k])) return 1;
    return 0;
}

/* calificadores; devuelve por *isout 1 = out/inout */
static int parse_quals(P* p, int* isout)
{
    if (isout) *isout = 0;
    while (is_idn(p, p->cur)) {
        if (is_kw(p, "layout")) {
            p->cur++;
            if (!is_p(p, "(") || skip_balanced(p, "(", ")") < 0) return -1;
            continue;
        }
        if (!is_quals_word(p, p->cur)) break;
        if (isout && (is_kw(p, "out") || is_kw(p, "inout"))) *isout = 1;
        p->cur++;
    }
    return 0;
}

static int parse_struct_body(P* p, int sid);

/* especificador de tipo: builtin, struct (nombre o definicion) y sufijos de array. 1 = consumido, 0 = no es un tipo, -1 = error */
static int parse_type_spec(P* p, ty_t* t)
{
    if (!is_idn(p, p->cur)) return 0;
    if (is_kw(p, "struct")) {
        int sid = -1, nm = -1;
        p->cur++;
        if (is_idn(p, p->cur) && !is_pn(p, p->cur, "{")) { nm = p->cur; p->cur++; }
        if (is_p(p, "{")) {
            struct_t* s;
            p->st = (struct_t*)grow(p->st, p->nst, &p->capst, sizeof(struct_t), &p->oom);
            if (p->oom) return -1;
            sid = p->nst++;
            s = &p->st[sid];
            s->n = nm >= 0 ? p->src + p->tk[nm].b : NULL;
            s->l = nm >= 0 ? p->tk[nm].e - p->tk[nm].b : 0;
            s->nf = 0; s->f = NULL;
            if (parse_struct_body(p, sid) < 0) return -1;
        } else if (nm >= 0) {
            sid = find_struct(p, p->src + p->tk[nm].b, p->tk[nm].e - p->tk[nm].b);
        }
        *t = t_unk();
        if (sid >= 0) { t->k = K_STRUCT; t->sid = (short)sid; }
    } else if (!type_name_at(p, p->cur, t)) {
        return 0;
    } else p->cur++;
    while (is_p(p, "[")) {
        if (skip_balanced(p, "[", "]") < 0) return -1;
        t->arr++;
    }
    return 1;
}

static int parse_struct_body(P* p, int sid)
{
    int cap = 0, oom_dummy = 0;
    p->cur++;                                           /* { */
    while (!is_p(p, "}")) {
        ty_t ft;
        int r;
        if (p->tk[p->cur].k == TK_EOF) return -1;
        if (parse_quals(p, NULL) < 0) return -1;
        r = parse_type_spec(p, &ft);
        if (r <= 0) return -1;
        for (;;) {
            int ni;
            ty_t dt = ft;
            field_t* f;
            if (!is_idn(p, p->cur)) return -1;
            ni = p->cur++;
            while (is_p(p, "[")) { if (skip_balanced(p, "[", "]") < 0) return -1; dt.arr++; }
            {
                struct_t* s = &p->st[sid];
                void* nf = realloc(s->f, (size_t)(s->nf + 1) * sizeof(field_t));
                (void)cap; (void)oom_dummy;
                if (!nf) { p->oom = 1; return -1; }
                s->f = (field_t*)nf;
                f = &s->f[s->nf++];
                f->n = p->src + p->tk[ni].b; f->l = p->tk[ni].e - p->tk[ni].b; f->t = dt;
            }
            if (is_p(p, ",")) { p->cur++; continue; }
            if (is_p(p, ";")) { p->cur++; break; }
            return -1;
        }
    }
    p->cur++;                                           /* } */
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ tipo de miembros */
static ty_t member_type(const P* p, ty_t t, int ni)
{
    const char* n = p->src + p->tk[ni].b;
    size_t l = p->tk[ni].e - p->tk[ni].b, i;
    if (t.k == K_STRUCT && !t.arr && t.sid >= 0 && t.sid < p->nst) {
        const struct_t* s = &p->st[t.sid];
        int k;
        for (k = 0; k < s->nf; k++) if (s->f[k].l == l && !memcmp(s->f[k].n, n, l)) return s->f[k].t;
        return t_unk();
    }
    if (is_num(t) && t.m == 0 && t.n > 1 && l >= 1 && l <= 4) {
        static const char* const sets[] = { "xyzw", "rgba", "stpq", NULL };
        int k;
        for (k = 0; sets[k]; k++) {
            int ok = 1;
            for (i = 0; i < l; i++) { const char* c = strchr(sets[k], n[i]); if (!c || !n[i] || (int)(c - sets[k]) >= t.n) ok = 0; }
            if (ok) return t_num(t.b, (int)l, 0);
        }
    }
    return t_unk();
}

static ty_t elem_type(ty_t t)
{
    if (t.arr) { t.arr--; return t; }
    if (is_num(t)) {
        if (t.m > 0) return t_num(t.b, t.n, 0);          /* columna */
        if (t.n > 1) return t_num(t.b, 1, 0);
    }
    return t_unk();
}

/* ------------------------------------------------------------------------------------------------------------------ expresiones */
static int parse_assign(P* p, ex_t* r);
static int parse_expr(P* p, ex_t* r);

static void unify(P* p, ex_t* a, ex_t* b)
{
    if (!is_num(a->t) || !is_num(b->t)) return;
    {
        int ra = rank_of(a->t.b), rb = rank_of(b->t.b);
        if (ra == 0 || rb == 0 || ra == rb) return;
        if (ra < rb) conv(p, a, t_num(b->t.b, a->t.n, a->t.m));
        else conv(p, b, t_num(a->t.b, b->t.n, b->t.m));
    }
}

/* forma del resultado de un operador aritmetico; 0 si no se sabe */
static int arith_shape(ty_t a, ty_t b, char op, int* n, int* m)
{
    if (a.m == 0 && b.m == 0) {
        *m = 0;
        if (a.n == b.n) *n = a.n; else if (a.n == 1) *n = b.n; else if (b.n == 1) *n = a.n; else return 0;
        return 1;
    }
    if (op == '*') {
        if (a.m && b.m) { *n = a.n; *m = b.m; return 1; }
        if (a.m && b.m == 0 && b.n == 1) { *n = a.n; *m = a.m; return 1; }
        if (b.m && a.m == 0 && a.n == 1) { *n = b.n; *m = b.m; return 1; }
        if (a.m && b.m == 0) { *n = a.n; *m = 0; return 1; }          /* mat * vec */
        if (b.m && a.m == 0) { *n = b.m; *m = 0; return 1; }          /* vec * mat */
        return 0;
    }
    if (a.m && b.m == 0 && b.n == 1) { *n = a.n; *m = a.m; return 1; }
    if (b.m && a.m == 0 && a.n == 1) { *n = b.n; *m = b.m; return 1; }
    if (a.m && b.m && a.n == b.n && a.m == b.m) { *n = a.n; *m = a.m; return 1; }
    return 0;
}

static void bin_apply(P* p, const char* op, ex_t* a, ex_t* b, ex_t* r)
{
    r->tb = a->tb; r->te = b->te; r->lit = 0; r->t = t_unk();
    if (!strcmp(op, "||") || !strcmp(op, "&&") || !strcmp(op, "^^")) { r->t = t_num(B_BOOL, 1, 0); return; }
    if (!strcmp(op, "<<") || !strcmp(op, ">>")) { if (is_num(a->t)) r->t = a->t; return; }
    if (!strcmp(op, "==") || !strcmp(op, "!=") || !strcmp(op, "<") || !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=")) {
        unify(p, a, b);
        r->t = t_num(B_BOOL, 1, 0);
        return;
    }
    if (!is_num(a->t) || !is_num(b->t)) return;
    unify(p, a, b);
    if (rank_of(a->t.b) == 0 || rank_of(b->t.b) == 0) return;
    if (op[0] == '&' || op[0] == '|' || op[0] == '^') {
        r->t = t_num(a->t.b, a->t.n >= b->t.n ? a->t.n : b->t.n, 0);
        return;
    }
    {
        int n, m;
        if (!arith_shape(a->t, b->t, op[0], &n, &m)) return;
        r->t = t_num(a->t.b, n, m);
    }
}

static int prec_of(const P* p)
{
    static const struct { const char* o; int pr; } t[] = {
        { "||", 1 }, { "^^", 2 }, { "&&", 3 }, { "|", 4 }, { "^", 5 }, { "&", 6 }, { "==", 7 }, { "!=", 7 },
        { "<", 8 }, { ">", 8 }, { "<=", 8 }, { ">=", 8 }, { "<<", 9 }, { ">>", 9 }, { "+", 10 }, { "-", 10 },
        { "*", 11 }, { "/", 11 }, { "%", 11 }, { NULL, 0 }
    };
    int k;
    if (p->tk[p->cur].k != TK_PUN) return 0;
    for (k = 0; t[k].o; k++) if (tx(p, p->cur, t[k].o)) return t[k].pr;
    return 0;
}

/* ---- funciones integradas ---- */
static int in_list(const char* n, size_t l, const char* const* list)
{
    int k;
    for (k = 0; list[k]; k++) if (strlen(list[k]) == l && !memcmp(list[k], n, l)) return 1;
    return 0;
}

static const char* const k_gen[] = {
    "radians", "degrees", "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "pow", "exp", "log",
    "exp2", "log2", "sqrt", "inversesqrt", "abs", "sign", "floor", "trunc", "round", "roundEven", "ceil", "fract", "mod", "min", "max", "clamp",
    "mix", "step", "smoothstep", "normalize", "faceforward", "reflect", "refract", "fma", "dFdx", "dFdy", "fwidth", "dFdxCoarse", "dFdyCoarse",
    "dFdxFine", "dFdyFine", "length", "distance", "dot", "cross", NULL };

/* funciones de textura: bit i = el argumento i es float (coordenada, lod, bias, derivadas) */
static const struct { const char* n; unsigned fmask; int kind; } k_tex[] = {
    { "texture", 0x6, 0 }, { "textureProj", 0x6, 0 }, { "textureLod", 0x6, 0 }, { "textureOffset", 0xA, 0 }, { "textureProjOffset", 0xA, 0 },
    { "textureLodOffset", 0x6, 0 }, { "textureProjLod", 0x6, 0 }, { "textureProjLodOffset", 0x6, 0 }, { "textureGrad", 0xE, 0 },
    { "textureGradOffset", 0xE, 0 }, { "textureProjGrad", 0xE, 0 }, { "textureProjGradOffset", 0xE, 0 },
    { "textureGather", 0x2, 1 }, { "textureGatherOffset", 0x2, 1 }, { "textureGatherOffsets", 0x2, 1 }, { "textureQueryLod", 0x2, 2 },
    { "texelFetch", 0, 3 }, { "texelFetchOffset", 0, 3 }, { "textureSize", 0, 4 }, { "textureQueryLevels", 0, 5 }, { "textureSamples", 0, 5 },
    { "texture1D", 0x6, 6 }, { "texture2D", 0x6, 6 }, { "texture3D", 0x6, 6 }, { "textureCube", 0x6, 6 },
    { "texture1DProj", 0x6, 6 }, { "texture2DProj", 0x6, 6 }, { "texture3DProj", 0x6, 6 },
    { "texture1DLod", 0x6, 6 }, { "texture2DLod", 0x6, 6 }, { "texture3DLod", 0x6, 6 }, { "textureCubeLod", 0x6, 6 },
    { "texture1DProjLod", 0x6, 6 }, { "texture2DProjLod", 0x6, 6 }, { "texture3DProjLod", 0x6, 6 },
    { "shadow1D", 0x6, 6 }, { "shadow2D", 0x6, 6 }, { "shadow1DProj", 0x6, 6 }, { "shadow2DProj", 0x6, 6 },
    { "shadow1DLod", 0x6, 6 }, { "shadow2DLod", 0x6, 6 }, { "shadow1DProjLod", 0x6, 6 }, { "shadow2DProjLod", 0x6, 6 },
    { NULL, 0, 0 } };

static int builtin_call(P* p, const char* n, size_t l, ex_t* a, int na, ty_t* ret)
{
    int i, k;
    if (in_list(n, l, k_gen)) {
        int anyf = 0, anyu = 0, allnum = 1, mn = 1, mm = 0, base = B_NONE;
        for (i = 0; i < na; i++) {
            if (a[i].t.k != K_NUM || a[i].t.arr) { allnum = 0; continue; }
            if (a[i].t.b == B_FLOAT) anyf = 1;
            if (a[i].t.b == B_UINT) anyu = 1;
        }
        for (i = 0; i < na; i++) {
            if (!is_num(a[i].t) || rank_of(a[i].t.b) == 0) continue;
            if (anyf && a[i].t.b != B_FLOAT) conv(p, &a[i], t_num(B_FLOAT, a[i].t.n, a[i].t.m));
            else if (anyu && a[i].t.b == B_INT) conv(p, &a[i], t_num(B_UINT, a[i].t.n, a[i].t.m));
        }
        *ret = t_unk();
        if (!allnum || na == 0) return 1;
        for (i = 0; i < na; i++) {
            if (a[i].t.n > mn) mn = a[i].t.n;
            if (a[i].t.m > mm) mm = a[i].t.m;
            if (base == B_NONE && rank_of(a[i].t.b)) base = a[i].t.b;
        }
        if (base == B_NONE) return 1;
        if (anyf) base = B_FLOAT; else if (anyu) base = B_UINT;
        if ((l == 6 && !memcmp(n, "length", 6)) || (l == 8 && !memcmp(n, "distance", 8)) || (l == 3 && !memcmp(n, "dot", 3))) *ret = t_num(B_FLOAT, 1, 0);
        else if (l == 5 && !memcmp(n, "cross", 5)) *ret = t_num(B_FLOAT, 3, 0);
        else *ret = t_num(base, mn, mm);
        return 1;
    }
    for (k = 0; k_tex[k].n; k++) {
        if (strlen(k_tex[k].n) != l || memcmp(k_tex[k].n, n, l)) continue;
        for (i = 0; i < na && i < 8; i++)
            if ((k_tex[k].fmask >> i) & 1u)
                if (is_num(a[i].t) && rank_of(a[i].t.b) && a[i].t.b != B_FLOAT) conv(p, &a[i], t_num(B_FLOAT, a[i].t.n, a[i].t.m));
        *ret = t_unk();
        if (na >= 1 && a[0].t.k == K_SMP && !a[0].t.arr) {
            ty_t s = a[0].t;
            switch (k_tex[k].kind) {
                case 0: *ret = s.shadow ? t_num(B_FLOAT, 1, 0) : t_num(s.sb, 4, 0); break;
                case 1: *ret = t_num(s.sb, 4, 0); break;
                case 2: *ret = t_num(B_FLOAT, 2, 0); break;
                case 3: *ret = t_num(s.sb, 4, 0); break;
                case 4: *ret = s.sd ? t_num(B_INT, s.sd, 0) : t_unk(); break;
                case 5: *ret = t_num(B_INT, 1, 0); break;
                default: *ret = t_num(s.sb, 4, 0); break;                  /* nombres de la API vieja */
            }
        }
        return 1;
    }
    /* solo tipo de retorno (los argumentos no se convierten) */
    {
        int n0 = (na >= 1 && is_num(a[0].t)) ? a[0].t.n : 0;
#define NAME(s) (l == sizeof(s) - 1 && !memcmp(n, s, l))
        *ret = t_unk();
        if (NAME("floatBitsToInt") || NAME("bitCount") || NAME("findLSB") || NAME("findMSB")) { if (n0) *ret = t_num(B_INT, n0, 0); return 1; }
        if (NAME("floatBitsToUint")) { if (n0) *ret = t_num(B_UINT, n0, 0); return 1; }
        if (NAME("intBitsToFloat") || NAME("uintBitsToFloat")) { if (n0) *ret = t_num(B_FLOAT, n0, 0); return 1; }
        if (NAME("packHalf2x16") || NAME("packUnorm2x16") || NAME("packSnorm2x16") || NAME("packUnorm4x8") || NAME("packSnorm4x8")) { *ret = t_num(B_UINT, 1, 0); return 1; }
        if (NAME("unpackHalf2x16") || NAME("unpackUnorm2x16") || NAME("unpackSnorm2x16")) { *ret = t_num(B_FLOAT, 2, 0); return 1; }
        if (NAME("unpackUnorm4x8") || NAME("unpackSnorm4x8")) { *ret = t_num(B_FLOAT, 4, 0); return 1; }
        if (NAME("isnan") || NAME("isinf") || NAME("not") || NAME("lessThan") || NAME("lessThanEqual") || NAME("greaterThan") ||
            NAME("greaterThanEqual") || NAME("equal") || NAME("notEqual")) { if (n0) *ret = t_num(B_BOOL, n0, 0); return 1; }
        if (NAME("any") || NAME("all")) { *ret = t_num(B_BOOL, 1, 0); return 1; }
        if (NAME("determinant")) { *ret = t_num(B_FLOAT, 1, 0); return 1; }
        if (NAME("transpose") && na >= 1 && is_num(a[0].t) && a[0].t.m) { *ret = t_num(B_FLOAT, a[0].t.m, a[0].t.n); return 1; }
        if ((NAME("inverse") || NAME("matrixCompMult")) && na >= 1 && is_num(a[0].t)) { *ret = a[0].t; return 1; }
        if (NAME("outerProduct") && na >= 2 && is_num(a[0].t) && is_num(a[1].t)) { *ret = t_num(B_FLOAT, a[0].t.n, a[1].t.n); return 1; }
#undef NAME
    }
    return 0;
}

/* funcion del usuario: elige la sobrecarga de menor coste y convierte los argumentos. 1 = resuelta */
static int user_call(P* p, const char* n, size_t l, ex_t* a, int na, ty_t* ret)
{
    int best = -1, bestc = 1 << 20, f, i;
    for (f = 0; f < p->nfn; f++) {
        const func_t* fn = &p->fn[f];
        int cost = 0, ok = 1;
        if (fn->l != l || memcmp(fn->n, n, l) || fn->np != na) continue;
        for (i = 0; i < na && ok; i++) {
            ty_t at = a[i].t, pt = fn->pt[i];
            if (at.k == K_UNK || pt.k == K_UNK) continue;
            if (same_type(at, pt)) continue;
            if (!fn->out[i] && can_up(at, pt)) { cost++; continue; }
            ok = 0;
        }
        if (ok && cost < bestc) { best = f; bestc = cost; }
    }
    if (best < 0) {
        /* ninguna sobrecarga viable: si hay una con ese nombre y numero de argumentos, la usamos para el tipo de retorno */
        int cands = 0, last = -1;
        for (f = 0; f < p->nfn; f++) if (p->fn[f].l == l && !memcmp(p->fn[f].n, n, l)) { if (p->fn[f].np == na) { cands++; last = f; } }
        if (cands == 1) { *ret = p->fn[last].ret; return 1; }
        return 0;
    }
    for (i = 0; i < na; i++) if (!p->fn[best].out[i]) conv_if(p, &a[i], p->fn[best].pt[i]);
    *ret = p->fn[best].ret;
    return 1;
}

static int parse_args(P* p, ex_t* a, int* na)
{
    p->cur++;                                           /* ( */
    *na = 0;
    if (is_p(p, ")")) { p->cur++; return 0; }
    if (is_kw(p, "void") && is_pn(p, p->cur + 1, ")")) { p->cur += 2; return 0; }
    for (;;) {
        if (*na >= MAXARG) return -1;
        if (parse_assign(p, &a[*na]) < 0) return -1;
        (*na)++;
        if (is_p(p, ",")) { p->cur++; continue; }
        if (is_p(p, ")")) { p->cur++; return 0; }
        return -1;
    }
}

static int parse_primary(P* p, ex_t* r)
{
    const tok_t* t = &p->tk[p->cur];
    int start = p->cur;
    memset(r, 0, sizeof *r);
    r->tb = start;
    if (t->k == TK_INT) {
        r->t = t_num((t->flags & TF_UNS) ? B_UINT : B_INT, 1, 0);
        if ((t->flags & (TF_PLAIN | TF_UNS)) == TF_PLAIN) r->lit = start + 1;
        p->cur++; r->te = p->cur;
        return 0;
    }
    if (t->k == TK_FLT) {
        r->t = (t->flags & TF_DBL) ? t_unk() : t_num(B_FLOAT, 1, 0);
        p->cur++; r->te = p->cur;
        return 0;
    }
    if (is_p(p, "(")) {
        ex_t in;
        p->cur++;
        if (parse_expr(p, &in) < 0 || expect(p, ")") < 0) return -1;
        r->t = in.t; r->te = p->cur;
        return 0;
    }
    if (t->k == TK_ID) {
        ty_t ct;
        if (is_kw(p, "true") || is_kw(p, "false")) { r->t = t_num(B_BOOL, 1, 0); p->cur++; r->te = p->cur; return 0; }
        if (type_name_at(p, p->cur, &ct)) {                                    /* constructor */
            ex_t a[MAXARG];
            int na, k;
            if (parse_type_spec(p, &ct) <= 0 || !is_p(p, "(")) return -1;
            if (parse_args(p, a, &na) < 0) return -1;
            if (ct.arr) {
                ty_t el = ct; el.arr--;
                for (k = 0; k < na; k++) conv_if(p, &a[k], el);
            } else if (ct.k == K_STRUCT && ct.sid >= 0 && ct.sid < p->nst && p->st[ct.sid].nf == na) {
                for (k = 0; k < na; k++) conv_if(p, &a[k], p->st[ct.sid].f[k].t);
            }
            r->t = ct; r->te = p->cur;
            return 0;
        }
        if (is_pn(p, p->cur + 1, "(")) {                                       /* llamada */
            ex_t a[MAXARG];
            int na, nameidx = p->cur;
            const char* nm = p->src + t->b;
            size_t nl = t->e - t->b;
            ty_t ret = t_unk();
            p->cur++;
            if (parse_args(p, a, &na) < 0) return -1;
            if (!user_call(p, nm, nl, a, na, &ret)) { if (!builtin_call(p, nm, nl, a, na, &ret)) ret = t_unk(); }
            (void)nameidx;
            r->t = ret; r->te = p->cur;
            return 0;
        }
        r->t = lookup_var(p, start);
        p->cur++; r->te = p->cur;
        return 0;
    }
    return -1;
}

static int parse_postfix(P* p, ex_t* r)
{
    if (parse_primary(p, r) < 0) return -1;
    for (;;) {
        if (is_p(p, "[")) {
            ex_t ix;
            p->cur++;
            if (parse_expr(p, &ix) < 0 || expect(p, "]") < 0) return -1;
            r->t = elem_type(r->t); r->te = p->cur; r->lit = 0;
        } else if (is_p(p, ".")) {
            int ni;
            p->cur++;
            if (!is_idn(p, p->cur)) return -1;
            ni = p->cur++;
            if (tk_eq(p, ni, "length", 6) && is_p(p, "(")) {
                p->cur++;
                if (expect(p, ")") < 0) return -1;
                r->t = t_num(B_INT, 1, 0);
            } else r->t = member_type(p, r->t, ni);
            r->te = p->cur; r->lit = 0;
        } else if (is_p(p, "++") || is_p(p, "--")) {
            p->cur++; r->te = p->cur; r->lit = 0;
        } else break;
    }
    return 0;
}

static int parse_unary(P* p, ex_t* r)
{
    if (is_p(p, "+") || is_p(p, "-") || is_p(p, "!") || is_p(p, "~") || is_p(p, "++") || is_p(p, "--")) {
        char op = (char)p->src[p->tk[p->cur].b];
        int start = p->cur, dbl = p->tk[p->cur].e - p->tk[p->cur].b == 2;
        ex_t x;
        p->cur++;
        if (parse_unary(p, &x) < 0) return -1;
        r->tb = start; r->te = x.te;
        r->t = (op == '!') ? t_num(B_BOOL, 1, 0) : x.t;
        r->lit = (!dbl && (op == '+' || op == '-')) ? x.lit : 0;
        return 0;
    }
    return parse_postfix(p, r);
}

static int parse_binary(P* p, int minp, ex_t* r)
{
    if (parse_unary(p, r) < 0) return -1;
    for (;;) {
        int pr = prec_of(p);
        char op[4];
        ex_t b, res;
        size_t l;
        if (pr == 0 || pr < minp) break;
        l = p->tk[p->cur].e - p->tk[p->cur].b;
        memcpy(op, p->src + p->tk[p->cur].b, l); op[l] = 0;
        p->cur++;
        if (parse_binary(p, pr + 1, &b) < 0) return -1;
        bin_apply(p, op, r, &b, &res);
        *r = res;
    }
    return 0;
}

static int parse_cond(P* p, ex_t* r)
{
    if (parse_binary(p, 1, r) < 0) return -1;
    if (is_p(p, "?")) {
        ex_t a, b;
        int tb = r->tb;
        p->cur++;
        if (parse_expr(p, &a) < 0 || expect(p, ":") < 0 || parse_assign(p, &b) < 0) return -1;
        unify(p, &a, &b);
        r->t = (same_type(a.t, b.t)) ? a.t : (a.t.k == K_UNK ? b.t : (b.t.k == K_UNK ? a.t : t_unk()));
        r->tb = tb; r->te = b.te; r->lit = 0;
    }
    return 0;
}

static int is_assign_op(const P* p)
{
    static const char* const ops[] = { "=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "|=", "^=", NULL };
    int k;
    if (p->tk[p->cur].k != TK_PUN) return 0;
    for (k = 0; ops[k]; k++) if (tx(p, p->cur, ops[k])) return 1;
    return 0;
}

static int parse_assign(P* p, ex_t* r)
{
    if (parse_cond(p, r) < 0) return -1;
    if (is_assign_op(p)) {
        ex_t b;
        int simple = p->tk[p->cur].e - p->tk[p->cur].b == 1;
        int shift_or_bit = tx(p, p->cur, "<<=") || tx(p, p->cur, ">>=") || tx(p, p->cur, "&=") || tx(p, p->cur, "|=") || tx(p, p->cur, "^=");
        p->cur++;
        if (parse_assign(p, &b) < 0) return -1;
        if (is_num(r->t) && is_num(b.t)) {
            if (simple) conv_if(p, &b, r->t);
            else if (!shift_or_bit && rank_of(b.t.b) && rank_of(b.t.b) < rank_of(r->t.b)) conv(p, &b, t_num(r->t.b, b.t.n, b.t.m));
        }
        r->te = b.te; r->lit = 0;
    }
    return 0;
}

static int parse_expr(P* p, ex_t* r)
{
    if (parse_assign(p, r) < 0) return -1;
    while (is_p(p, ",")) {
        ex_t b;
        int tb = r->tb;
        p->cur++;
        if (parse_assign(p, &b) < 0) return -1;
        *r = b; r->tb = tb; r->lit = 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------ declaraciones y sentencias */
static int parse_block(P* p);
static int parse_stmt(P* p);

static void register_func(P* p, const func_t* f)
{
    p->fn = (func_t*)grow(p->fn, p->nfn, &p->capfn, sizeof(func_t), &p->oom);
    if (p->oom) return;
    p->fn[p->nfn++] = *f;
}

/* cur en el '(' de los parametros */
static int parse_func(P* p, ty_t ret, int nameidx)
{
    func_t f;
    int pn[MAXPAR];
    int np = 0;
    memset(&f, 0, sizeof f);
    f.n = p->src + p->tk[nameidx].b; f.l = p->tk[nameidx].e - p->tk[nameidx].b; f.ret = ret;
    p->cur++;                                           /* ( */
    if (!is_p(p, ")")) {
        if (is_kw(p, "void") && is_pn(p, p->cur + 1, ")")) p->cur++;
        else for (;;) {
            ty_t pt;
            int isout, r;
            if (np >= MAXPAR) return -1;
            if (parse_quals(p, &isout) < 0) return -1;
            r = parse_type_spec(p, &pt);
            if (r <= 0) return -1;
            pn[np] = -1;
            if (is_idn(p, p->cur) && !is_quals_word(p, p->cur)) pn[np] = p->cur++;
            while (is_p(p, "[")) { if (skip_balanced(p, "[", "]") < 0) return -1; pt.arr++; }
            f.pt[np] = pt; f.out[np] = (unsigned char)isout;
            np++;
            if (is_p(p, ",")) { p->cur++; continue; }
            break;
        }
    }
    if (expect(p, ")") < 0) return -1;
    f.np = np;
    register_func(p, &f);
    if (is_p(p, ";")) { p->cur++; return 0; }
    if (!is_p(p, "{")) return -1;
    {
        int k, r;
        ty_t save = p->cur_ret;
        push_scope(p);
        for (k = 0; k < np; k++) if (pn[k] >= 0) declare(p, p->src + p->tk[pn[k]].b, p->tk[pn[k]].e - p->tk[pn[k]].b, f.pt[k]);
        p->cur_ret = ret;
        r = parse_block(p);
        p->cur_ret = save;
        pop_scope(p);
        return r;
    }
}

/* 0 = declaracion consumida, 1 = no es una declaracion (cur sin tocar), -1 = error */
static int parse_decl(P* p, int global)
{
    int save = p->cur, quals_seen, r, unk_named = 0;
    ty_t t;
    if (parse_quals(p, NULL) < 0) return -1;
    quals_seen = p->cur > save;
    r = parse_type_spec(p, &t);
    if (r < 0) return -1;
    if (r == 0) {
        if (quals_seen && is_idn(p, p->cur) && is_pn(p, p->cur + 1, "{")) {          /* bloque de interfaz */
            int sid, bn = p->cur;
            ty_t bt = t_unk();
            struct_t* s;
            p->st = (struct_t*)grow(p->st, p->nst, &p->capst, sizeof(struct_t), &p->oom);
            if (p->oom) return -1;
            sid = p->nst++;
            s = &p->st[sid];
            s->n = NULL; s->l = 0; s->nf = 0; s->f = NULL;
            p->cur++;
            if (parse_struct_body(p, sid) < 0) return -1;
            (void)bn;
            bt.k = K_STRUCT; bt.sid = (short)sid;
            if (is_p(p, ";")) {                                                      /* sin instancia: los miembros son globales */
                int k;
                for (k = 0; k < p->st[sid].nf; k++) declare(p, p->st[sid].f[k].n, p->st[sid].f[k].l, p->st[sid].f[k].t);
                p->cur++;
                return 0;
            }
            if (!is_idn(p, p->cur)) return -1;
            {
                int ni = p->cur++;
                while (is_p(p, "[")) { if (skip_balanced(p, "[", "]") < 0) return -1; bt.arr++; }
                declare(p, p->src + p->tk[ni].b, p->tk[ni].e - p->tk[ni].b, bt);
            }
            return expect(p, ";");
        }
        /* tipo desconocido (macro sin expandir): `calificadores TIPO nombre` o `TIPO nombre` */
        if (is_idn(p, p->cur) && !is_quals_word(p, p->cur) && is_idn(p, p->cur + 1) && !is_kwn(p, p->cur, "return") &&
            !is_kwn(p, p->cur, "else") && !is_kwn(p, p->cur, "case") && !is_kwn(p, p->cur, "discard") && !is_kwn(p, p->cur, "do")) {
            t = t_unk(); p->cur++; unk_named = 1;
            while (is_p(p, "[")) { if (skip_balanced(p, "[", "]") < 0) return -1; t.arr++; }
        } else { p->cur = save; return 1; }
    }
    (void)unk_named;
    if (is_p(p, ";")) { p->cur++; return 0; }
    if (!is_idn(p, p->cur) || is_quals_word(p, p->cur)) {
        if (quals_seen || r == 0) return -1;
        p->cur = save;                                  /* `vec3(1.0);` y similares: es una expresion */
        return 1;
    }
    if (global && is_pn(p, p->cur + 1, "(")) { int ni = p->cur++; return parse_func(p, t, ni); }
    for (;;) {
        ty_t dt = t;
        int ni;
        if (!is_idn(p, p->cur)) return -1;
        ni = p->cur++;
        while (is_p(p, "[")) { if (skip_balanced(p, "[", "]") < 0) return -1; dt.arr++; }
        if (is_p(p, "=")) {
            p->cur++;
            if (is_p(p, "{")) { if (skip_balanced(p, "{", "}") < 0) return -1; }
            else {
                ex_t ini;
                if (parse_assign(p, &ini) < 0) return -1;
                conv_if(p, &ini, dt);
            }
        }
        declare(p, p->src + p->tk[ni].b, p->tk[ni].e - p->tk[ni].b, dt);
        if (is_p(p, ",")) { p->cur++; continue; }
        return expect(p, ";");
    }
}

static int parse_block(P* p)
{
    if (expect(p, "{") < 0) return -1;
    push_scope(p);
    while (!is_p(p, "}")) {
        if (p->tk[p->cur].k == TK_EOF) return -1;
        if (parse_stmt(p) < 0) return -1;
    }
    p->cur++;
    pop_scope(p);
    return 0;
}

static int skip_to_semi(P* p)
{
    while (!is_p(p, ";")) { if (p->tk[p->cur].k == TK_EOF) return -1; p->cur++; }
    p->cur++;
    return 0;
}

static int parse_paren_expr(P* p)
{
    ex_t x;
    if (expect(p, "(") < 0 || parse_expr(p, &x) < 0 || expect(p, ")") < 0) return -1;
    return 0;
}

static int parse_stmt(P* p)
{
    if (p->tk[p->cur].k == TK_EOF) return -1;
    if (is_p(p, ";")) { p->cur++; return 0; }
    if (is_p(p, "{")) return parse_block(p);
    if (is_idn(p, p->cur)) {
        if (is_kw(p, "if")) {
            p->cur++;
            if (parse_paren_expr(p) < 0 || parse_stmt(p) < 0) return -1;
            if (is_kw(p, "else")) { p->cur++; return parse_stmt(p); }
            return 0;
        }
        if (is_kw(p, "while")) { p->cur++; if (parse_paren_expr(p) < 0) return -1; return parse_stmt(p); }
        if (is_kw(p, "do")) {
            p->cur++;
            if (parse_stmt(p) < 0 || !is_kw(p, "while")) return -1;
            p->cur++;
            if (parse_paren_expr(p) < 0) return -1;
            return expect(p, ";");
        }
        if (is_kw(p, "switch")) { p->cur++; if (parse_paren_expr(p) < 0) return -1; return parse_stmt(p); }
        if (is_kw(p, "case")) { ex_t x; p->cur++; if (parse_expr(p, &x) < 0) return -1; return expect(p, ":"); }
        if (is_kw(p, "default") && is_pn(p, p->cur + 1, ":")) { p->cur += 2; return 0; }
        if (is_kw(p, "return")) {
            p->cur++;
            if (!is_p(p, ";")) {
                ex_t x;
                if (parse_expr(p, &x) < 0) return -1;
                conv_if(p, &x, p->cur_ret);
            }
            return expect(p, ";");
        }
        if (is_kw(p, "break") || is_kw(p, "continue") || is_kw(p, "discard")) { p->cur++; return expect(p, ";"); }
        if (is_kw(p, "precision")) return skip_to_semi(p);
        if (is_kw(p, "for")) {
            int r;
            p->cur++;
            if (expect(p, "(") < 0) return -1;
            push_scope(p);
            if (is_p(p, ";")) p->cur++;
            else {
                int d = parse_decl(p, 0);
                if (d < 0) return -1;
                if (d == 1) { ex_t x; if (parse_expr(p, &x) < 0 || expect(p, ";") < 0) return -1; }
            }
            if (!is_p(p, ";")) { ex_t x; if (parse_expr(p, &x) < 0) return -1; }
            if (expect(p, ";") < 0) return -1;
            if (!is_p(p, ")")) { ex_t x; if (parse_expr(p, &x) < 0) return -1; }
            if (expect(p, ")") < 0) return -1;
            r = parse_stmt(p);
            pop_scope(p);
            return r;
        }
    }
    {
        int d = parse_decl(p, 0);
        ex_t x;
        if (d < 0) return -1;
        if (d == 0) return 0;
        if (parse_expr(p, &x) < 0) return -1;
        return expect(p, ";");
    }
}

/* tras un error: salta el elemento de nivel superior que empezaba en `start` (hasta su ';' o su '}' de cierre) */
static void resync(P* p, int start)
{
    int d = 0, i = start;
    pop_to_global(p);
    for (;; i++) {
        if (p->tk[i].k == TK_EOF) break;
        if (is_pn(p, i, "{")) d++;
        else if (is_pn(p, i, "}")) { d--; if (d <= 0) { i++; if (is_pn(p, i, ";")) i++; break; } }
        else if (is_pn(p, i, ";") && d == 0) { i++; break; }
    }
    p->cur = i > start ? i : start + 1;
}

/* ------------------------------------------------------------------------------------------------------------------ salida */
static int edit_cmp(const void* x, const void* y)
{
    const edit_t* a = (const edit_t*)x;
    const edit_t* b = (const edit_t*)y;
    if (a->pos != b->pos) return a->pos < b->pos ? -1 : 1;
    if (a->grp != b->grp) return a->grp - b->grp;                       /* cierres y sufijos antes que aperturas */
    if (a->grp == 0) return a->seq - b->seq;                            /* cierres: el mas interno (creado antes) primero */
    return b->seq - a->seq;                                             /* aperturas: la mas externa (creada despues) primero */
}

static void free_all(P* p)
{
    int i;
    free(p->tk); free(p->ed); free(p->sy); free(p->fn);
    for (i = 0; i < p->nst; i++) free(p->st[i].f);
    free(p->st);
}

/* Devuelve el fuente con las conversiones implicitas hechas explicitas (memoria de malloc), o NULL si falta memoria. */
char* gl31_glsl_implicit_conv(const char* src)
{
    P p;
    char* out;
    size_t n, o = 0, tot;
    int i;
    memset(&p, 0, sizeof p);
    p.src = src; p.len = strlen(src);
    lex(&p);
    while (!p.oom && p.tk[p.cur].k != TK_EOF) {
        int start = p.cur, r;
        if (is_kw(&p, "precision")) { if (skip_to_semi(&p) < 0) break; continue; }
        if (is_p(&p, ";")) { p.cur++; continue; }
        r = parse_decl(&p, 1);
        if (r != 0) resync(&p, start);
        if (p.cur <= start) p.cur = start + 1;
    }
    if (p.oom) { free_all(&p); return NULL; }
    if (p.ne > 1) qsort(p.ed, (size_t)p.ne, sizeof(edit_t), edit_cmp);
    tot = p.len + 1;
    for (i = 0; i < p.ne; i++) tot += strlen(p.ed[i].txt);
    out = (char*)malloc(tot);
    if (!out) { free_all(&p); return NULL; }
    n = 0;
    for (i = 0; i <= p.ne; i++) {
        size_t upto = i < p.ne ? p.ed[i].pos : p.len;
        if (upto > p.len) upto = p.len;
        if (upto > n) { memcpy(out + o, src + n, upto - n); o += upto - n; n = upto; }
        if (i < p.ne) { size_t l = strlen(p.ed[i].txt); memcpy(out + o, p.ed[i].txt, l); o += l; }
    }
    out[o] = 0;
    free_all(&p);
    return out;
}
