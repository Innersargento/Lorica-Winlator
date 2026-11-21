#include "buffers.h"

#include "khash.h"
#include "../glx/hardext.h"
#include "attributes.h"
#include "debug.h"
#include "gl4es.h"
#include "glstate.h"
#include "logs.h"
#include "init.h"
#include "loader.h"

//#define DEBUG
#ifdef DEBUG
#define DBG(a) a
#else
#define DBG(a)
#endif


KHASH_MAP_IMPL_INT(buff, glbuffer_t *);
KHASH_MAP_IMPL_INT(glvao, glvao_t*);

static GLuint lastbuffer = 1;

// lazy upload bookkeeping (glbuffer_t.stale): one flag per 4 KB of the shadow copy
#define STALE_SHIFT 12
static void buffer_mark_stale(glbuffer_t *buff, GLintptr lo, GLintptr hi) {
    if(!buff->data || buff->size <= 0) return;
    if(lo < 0) lo = 0;
    if(hi > (GLintptr)buff->size) hi = (GLintptr)buff->size;
    if(hi <= lo) return;
    GLsizeiptr n = (buff->size + ((1<<STALE_SHIFT)-1)) >> STALE_SHIFT;
    if(buff->stale_n != n) {
        free(buff->stale);
        buff->stale = (unsigned char*)calloc(n, 1);
        buff->stale_n = buff->stale ? n : 0;
        buff->nstale = 0;
        if(!buff->stale) { // out of memory: send now
            LOAD_GLES(glBufferSubData);
            bindBuffer(buff->type, buff->real_buffer);
            gles_glBufferSubData(buff->type, lo, hi - lo, (void*)((uintptr_t)buff->data + lo));
            return;
        }
    }
    for(GLsizeiptr b = lo >> STALE_SHIFT, last = (hi - 1) >> STALE_SHIFT; b <= last; ++b)
        if(!buff->stale[b]) { buff->stale[b] = 1; ++buff->nstale; }
}
// the real storage was (re)created from the shadow copy: nothing is stale
static void buffer_reset_stale(glbuffer_t *buff) {
    free(buff->stale);
    buff->stale = NULL;
    buff->stale_n = 0;
    buff->nstale = 0;
}

// Utility function to bind / unbind a particular buffer

glbuffer_t** BUFF(GLenum target) {
 switch(target) {
     case GL_ARRAY_BUFFER:
        return &glstate->vao->vertex;
        break;
     case GL_ELEMENT_ARRAY_BUFFER:
        return &glstate->vao->elements;
        break;
     case GL_PIXEL_PACK_BUFFER:
        return &glstate->vao->pack;
        break;
     case GL_PIXEL_UNPACK_BUFFER:
        return &glstate->vao->unpack;
        break;
     default:
       LOGD("Warning, unknown buffer target 0x%04X\n", target);
 }
 return (glbuffer_t**)NULL;
}

void unbind_buffer(GLenum target) {
    glbuffer_t **t = BUFF(target);
    if (t)
		*t=(glbuffer_t*)NULL;
}
void bind_buffer(GLenum target, glbuffer_t* buff) {
    glbuffer_t ** t = BUFF(target);
    if (t)
		*t = buff;
}
glbuffer_t* getbuffer_buffer(GLenum target) {
    glbuffer_t ** t = BUFF(target);
    if (t)
		return *t;
    return NULL;
}
glbuffer_t* getbuffer_id(GLuint buffer) {
    if(!buffer)
        return NULL;
    khint_t k;
    khash_t(buff) *list = glstate->buffers;
    k = kh_get(buff, list, buffer);
    if (k == kh_end(list))
        return NULL;
    return kh_value(list, k);
}

int buffer_target(GLenum target) {
	if (target==GL_ARRAY_BUFFER)
		return 1;
	if (target==GL_ELEMENT_ARRAY_BUFFER)
		return 1;
	if (target==GL_PIXEL_PACK_BUFFER)
		return 1;
	if (target==GL_PIXEL_UNPACK_BUFFER)
		return 1;
	return 0;
}

/* ---- Hybrid contexts (compatibility GL >= 3.2): buffer targets that only exist in GLES 3.x ----
 * GL4ES buffers have their own ids and keep the data in client memory, so a GLADIATOR-only function (glBindBufferBase...)
 * would receive an id GLES does not know. For UBO & co. the GL4ES buffer is backed by a real GLES buffer
 * (glbuffer_t.real_buffer) that is bound / filled directly, and gl4es_real_buffer() translates the id. */
#ifndef NOEGL
extern int  gl4es_gl31_hybrid_active(void);                                            /* glx/gl31_bridge.c */
extern void gl4es_gl31_bind_buffer_base(GLenum target, GLuint index, GLuint buffer);
extern void gl4es_gl31_bind_buffer_range(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
#endif

static int es_only_target(GLenum target) {
#ifndef NOEGL
    switch(target) {
        case 0x8A11: /* GL_UNIFORM_BUFFER */
        case 0x8F36: /* GL_COPY_READ_BUFFER */
        case 0x8F37: /* GL_COPY_WRITE_BUFFER */
        case 0x8C8E: /* GL_TRANSFORM_FEEDBACK_BUFFER */
        case 0x90D2: /* GL_SHADER_STORAGE_BUFFER */
        case 0x92C0: /* GL_ATOMIC_COUNTER_BUFFER */
        case 0x8F3F: /* GL_DRAW_INDIRECT_BUFFER */
        case 0x90EE: /* GL_DISPATCH_INDIRECT_BUFFER */
        case 0x8C2A: /* GL_TEXTURE_BUFFER */
            return gl4es_gl31_hybrid_active();
    }
#endif
    return 0;
}

/* ES 3.x buffer entry points (not in GL4ES's own GLES table): fetched from the GLES library */
typedef void* (APIENTRY_GLES *es3_map_t)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef GLboolean (APIENTRY_GLES *es3_unmap_t)(GLenum);
typedef void (APIENTRY_GLES *es3_flush_t)(GLenum, GLintptr, GLsizeiptr);
typedef void (APIENTRY_GLES *es3_copy_t)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);
static void* es3_fn(const char* name) { return gles ? proc_address(gles, name) : NULL; }


/* Which GL4ES buffer id the application has bound to each ES-only target. The real GLES name behind it is a different number that can
 * collide with another buffer's id, so glGetIntegerv(GL_*_BUFFER_BINDING) must answer with the application's id (Wine uses that answer
 * to find its own bookkeeping of the buffer, e.g. to know whether it is already mapped). */
static GLuint es_bound[9];
static int es_slot(GLenum target) {
    switch(target) {
        case 0x8A11: return 0; case 0x8F36: return 1; case 0x8F37: return 2; case 0x8C8E: return 3; case 0x90D2: return 4;
        case 0x92C0: return 5; case 0x8F3F: return 6; case 0x90EE: return 7; case 0x8C2A: return 8;
    }
    return -1;
}

/* glGetIntegerv(pname) for the binding of an ES-only target in hybrid mode: 1 and *out set if handled */
int gl4es_es_only_binding(GLenum pname, GLuint* out) {
    GLenum target;
    switch(pname) {
        case 0x8A28: target = 0x8A11; break;   /* UNIFORM_BUFFER_BINDING */
        case 0x8F36: target = 0x8F36; break;   /* COPY_READ_BUFFER_BINDING */
        case 0x8F37: target = 0x8F37; break;   /* COPY_WRITE_BUFFER_BINDING */
        case 0x8C8F: target = 0x8C8E; break;   /* TRANSFORM_FEEDBACK_BUFFER_BINDING */
        case 0x90D3: target = 0x90D2; break;   /* SHADER_STORAGE_BUFFER_BINDING */
        case 0x92C1: target = 0x92C0; break;   /* ATOMIC_COUNTER_BUFFER_BINDING */
        case 0x8F43: target = 0x8F3F; break;   /* DRAW_INDIRECT_BUFFER_BINDING */
        case 0x90EF: target = 0x90EE; break;   /* DISPATCH_INDIRECT_BUFFER_BINDING */
        case 0x8C2D: target = 0x8C2A; break;   /* TEXTURE_BUFFER_BINDING */
        default: return 0;
    }
    if(!es_only_target(target)) return 0;
    *out = es_bound[es_slot(target)];
    return 1;
}

static glbuffer_t* buffer_get_or_create(GLuint buffer, GLenum target) {
    int ret;
    khash_t(buff) *list = glstate->buffers;
    khint_t k = kh_get(buff, list, buffer);
    glbuffer_t *buff = NULL;
    if (k == kh_end(list)) {
        k = kh_put(buff, list, buffer, &ret);
        buff = kh_value(list, k) = calloc(1, sizeof(glbuffer_t));
        buff->buffer = buffer;
        buff->type = target;
        buff->data = NULL;
        buff->usage = GL_STATIC_DRAW;
        buff->size = 0;
        buff->access = GL_READ_WRITE;
        buff->mapped = 0;
        buff->real_buffer = 0;
    } else
        buff = kh_value(list, k);
    return buff;
}

/* real GLES buffer name behind a GL4ES buffer id (created on demand); 0 for id 0 */
GLuint gl4es_real_buffer(GLuint id) {
    if (!id) return 0;
    glbuffer_t *buff = buffer_get_or_create(id, GL_ARRAY_BUFFER);
    if (!buff->real_buffer) {
        LOAD_GLES(glGenBuffers);
        gles_glGenBuffers(1, &buff->real_buffer);
    }
    gl4es_buffer_resync(buff);  // the name goes to GLADIATOR (UBO, texture buffer...): it reads the real storage, not a draw of ours
    return buff->real_buffer;
}

void APIENTRY_GL4ES gl4es_glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
#ifndef NOEGL
    if (gl4es_gl31_hybrid_active()) {
        gl4es_gl31_bind_buffer_base(target, index, gl4es_real_buffer(buffer));
        return;
    }
#endif
    errorShim(GL_INVALID_OPERATION);
}

void APIENTRY_GL4ES gl4es_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
#ifndef NOEGL
    if (gl4es_gl31_hybrid_active()) {
        gl4es_gl31_bind_buffer_range(target, index, gl4es_real_buffer(buffer), offset, size);
        return;
    }
#endif
    errorShim(GL_INVALID_OPERATION);
}

void rebind_real_buff_arrays(int old_buffer, int new_buffer) {
    for (int j = 0; j < hardext.maxvattrib; j++) {
        if (glstate->vao->vertexattrib[j].real_buffer == old_buffer) {
            glstate->vao->vertexattrib[j].real_buffer = new_buffer;
            /*if(!new_buffer)
                glstate->vao->vertexattrib[j].real_pointer = 0;*/
        }
    }
}

void APIENTRY_GL4ES gl4es_glGenBuffers(GLsizei n, GLuint * buffers) {
    DBG(printf("glGenBuffers(%i, %p)\n", n, buffers);)
	noerrorShim();
    if (n<1) {
		errorShim(GL_INVALID_VALUE);
        return;
    }
    khash_t(buff) *list = glstate->buffers;
    for (int i=0; i<n; i++) {   // create buffer, and check uniqueness...
        int b;
        while(getbuffer_id(b=lastbuffer++));
        buffers[i] = b;
        // create the buffer
        khint_t k;
   	    int ret;
        k = kh_put(buff, list, b, &ret);
        glbuffer_t *buff = kh_value(list, k) = calloc(1, sizeof(glbuffer_t));
        buff->buffer = b;
        buff->type = 0; // no target for now
        buff->data = NULL;
        buff->usage = GL_STATIC_DRAW;
        buff->size = 0;
        buff->access = GL_READ_WRITE;
        buff->mapped = 0;
        buff->real_buffer = 0;
    }
}

void APIENTRY_GL4ES gl4es_glBindBuffer(GLenum target, GLuint buffer) {
    DBG(printf("glBindBuffer(%s, %u)\n", PrintEnum(target), buffer);)
    // this flush is probably not needed as long as real VBO are not used
    FLUSH_BEGINEND;

    if (es_only_target(target)) {
        // UBO & co: bind the real GLES buffer behind this id
        GLuint real = gl4es_real_buffer(buffer);
        LOAD_GLES(glBindBuffer);
        gles_glBindBuffer(target, real);
        es_bound[es_slot(target)] = buffer;
        noerrorShim();
        return;
    }

   	khint_t k;
   	int ret;
	khash_t(buff) *list = glstate->buffers;
	if (!buffer_target(target)) {
		errorShim(GL_INVALID_ENUM);
		return;
	}
    // if buffer = 0 => unbind buffer!
    if (buffer == 0) {
        // unbind buffer
        bindBuffer(target, 0);
        unbind_buffer(target);
    } else {
        // search for an existing buffer
        k = kh_get(buff, list, buffer);
        glbuffer_t *buff = NULL;
        if (k == kh_end(list)){
            k = kh_put(buff, list, buffer, &ret);
            buff = kh_value(list, k) = calloc(1, sizeof(glbuffer_t));
            buff->buffer = buffer;
            buff->type = target;
            buff->data = NULL;
            buff->usage = GL_STATIC_DRAW;
            buff->size = 0;
            buff->access = GL_READ_WRITE;
            buff->mapped = 0;
            buff->real_buffer = 0;
        } else {
            buff = kh_value(list, k);
            buff->type = target;    //TODO: check if old binding?
        }
        bind_buffer(target, buff);
    }
    noerrorShim();
}

void APIENTRY_GL4ES gl4es_glBufferData(GLenum target, GLsizeiptr size, const GLvoid * data, GLenum usage) {
    DBG(printf("glBufferData(%s, %zi, %p, %s)\n", PrintEnum(target), size, data, PrintEnum(usage));)
    if (es_only_target(target)) {
        LOAD_GLES(glBufferData);
        gles_glBufferData(target, size, data, usage);
        errorGL();
        return;
    }
	if (!buffer_target(target)) {
		errorShim(GL_INVALID_ENUM);
		return;
	}
    glbuffer_t *buff = getbuffer_buffer(target);
    if (buff==NULL) {
		errorShim(GL_INVALID_OPERATION);
        LOGE("Warning, null buffer for target=0x%04X for glBufferData\n", target);
        return;
    }
    if(target==GL_ARRAY_BUFFER)
        VaoSharedClear(glstate->vao);
    
    int go_real = 0;
    if(     (target==GL_ARRAY_BUFFER || target==GL_ELEMENT_ARRAY_BUFFER) 
         && (usage==GL_STREAM_DRAW || usage==GL_STATIC_DRAW || usage==GL_DYNAMIC_DRAW) && globals4es.usevbo)
        go_real = 1;
    
    if(buff->real_buffer && !go_real) {
        rebind_real_buff_arrays(buff->real_buffer, 0);
        
        deleteSingleBuffer(buff->real_buffer);
        // what about VA already pointing there?
        buff->real_buffer = 0;
    }
    if(go_real) {
        if(!buff->real_buffer) {
            LOAD_GLES(glGenBuffers);
            gles_glGenBuffers(1, &buff->real_buffer);
        }
        LOAD_GLES(glBufferData);
        LOAD_GLES(glBindBuffer);
        bindBuffer(target, buff->real_buffer);
        gles_glBufferData(target, size, data, usage);
        DBG(printf(" => real VBO %d\n", buff->real_buffer);)
    }
        
    buffer_reset_stale(buff);   // glBufferData above gave the real storage the same contents as the shadow copy
    buff->rs_lo = buff->rs_hi = 0;
    if (buff->data && buff->size<size) {
        free(buff->data);
        buff->data = NULL;
    }
    if(!buff->data)
        buff->data = malloc(size);
    buff->size = size;
    buff->usage = usage;
    DBG(printf("\t buff->data = %p (size=%zd)\n", buff->data, size);)
    buff->access = GL_READ_WRITE;
    if (data)
        memcpy(buff->data, data, size);
    // update binded VA
    for (int i=0; i<hardext.maxvattrib; ++i) {
        vertexattrib_t *v = &glstate->vao->vertexattrib[i];
        if( v->buffer == buff ) {
		    v->real_buffer = v->buffer->real_buffer;
            // do not update real_pointer, as it's the relative start in the buffer
        }
    }
    noerrorShim();
}

void APIENTRY_GL4ES gl4es_glNamedBufferData(GLuint buffer, GLsizeiptr size, const GLvoid * data, GLenum usage) {
    DBG(printf("glNamedBufferData(%u, %zi, %p, %s)\n", buffer, size, data, PrintEnum(usage));)
    glbuffer_t *buff = getbuffer_id(buffer);
    if (buff==NULL) {
        DBG(printf("Named Buffer not found\n");)
		errorShim(GL_INVALID_OPERATION);
        return;
    }
    if (buff->data) {
        free(buff->data);

    }
    int go_real = 0;
    if(     (buff->type==GL_ARRAY_BUFFER || buff->type==GL_ELEMENT_ARRAY_BUFFER) 
         && (usage==GL_STREAM_DRAW || usage==GL_STATIC_DRAW || usage==GL_DYNAMIC_DRAW) && globals4es.usevbo)
        go_real = 1;
    
    if(buff->real_buffer && !go_real) {
        deleteSingleBuffer(buff->real_buffer);
        // what about VA already pointing there?
        buff->real_buffer = 0;
    }
    if(go_real) {
        if(!buff->real_buffer) {
            LOAD_GLES(glGenBuffers);
            gles_glGenBuffers(1, &buff->real_buffer);
        }
        LOAD_GLES(glBufferData);
        LOAD_GLES(glBindBuffer);
        bindBuffer(buff->type, buff->real_buffer);
        gles_glBufferData(buff->type, size, data, usage);
    }

    buffer_reset_stale(buff);
    buff->rs_lo = buff->rs_hi = 0;
    buff->size = size;
    buff->usage = usage;
    buff->data = malloc(size);
    buff->access = GL_READ_WRITE;
    if (data)
        memcpy(buff->data, data, size);
    // update binded VA
    for (int i=0; i<hardext.maxvattrib; ++i) {
        vertexattrib_t *v = &glstate->vao->vertexattrib[i];
        if( v->buffer == buff ) {
		    v->real_buffer = v->buffer->real_buffer;
            // do not update real_pointer, as it's the relative start in the buffer
        }
    }
    noerrorShim();
}

void APIENTRY_GL4ES gl4es_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid * data) {
    DBG(printf("glBufferSubData(%s, %p, %zi, %p)\n", PrintEnum(target), (void*)offset, size, data);)
    if (es_only_target(target)) {
        LOAD_GLES(glBufferSubData);
        gles_glBufferSubData(target, offset, size, data);
        errorGL();
        return;
    }
	if (!buffer_target(target)) {
		errorShim(GL_INVALID_ENUM);
		return;
	}
    glbuffer_t *buff = getbuffer_buffer(target);
    if (buff==NULL) {
		errorShim(GL_INVALID_OPERATION);
        DBG(printf("LIBGL: Warning, null buffer for target=0x%04X for glBufferSubData\n", target);)
        return;
    }

    if(target==GL_ARRAY_BUFFER)
        VaoSharedClear(glstate->vao);

    if (offset<0 || size<0 || offset+size>buff->size) {
        errorShim(GL_INVALID_VALUE);
        return;
    }

    if((target==GL_ARRAY_BUFFER || target==GL_ELEMENT_ARRAY_BUFFER) && buff->real_buffer) {
        LOAD_GLES(glBufferSubData);
        LOAD_GLES(glBindBuffer);
        bindBuffer(target, buff->real_buffer);
        gles_glBufferSubData(target, offset, size, data);
    }
        
    memcpy((char*)buff->data + offset, data, size);
    noerrorShim();
}
void APIENTRY_GL4ES gl4es_glNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, const GLvoid * data) {
    DBG(printf("glNamedBufferSubData(%u, %p, %zi, %p)\n", buffer, (void*)offset, size, data);)
    glbuffer_t *buff = getbuffer_id(buffer);
    if (buff==NULL) {
		errorShim(GL_INVALID_OPERATION);
        return;
    }

    if (offset<0 || size<0 || offset+size>buff->size) {
        errorShim(GL_INVALID_VALUE);
        return;
    }
        
    if((buff->type==GL_ARRAY_BUFFER || buff->type==GL_ELEMENT_ARRAY_BUFFER) && buff->real_buffer) {
        LOAD_GLES(glBufferSubData);
        LOAD_GLES(glBindBuffer);
        bindBuffer(buff->type, buff->real_buffer);
        gles_glBufferSubData(buff->type, offset, size, data);
    }
    memcpy((char*)buff->data + offset, data, size);
    noerrorShim();
}

void APIENTRY_GL4ES gl4es_glDeleteBuffers(GLsizei n, const GLuint * buffers) {
    DBG(printf("glDeleteBuffers(%i, %p)\n", n, buffers);)
    if(!glstate) return;
    FLUSH_BEGINEND;
    
    VaoSharedClear(glstate->vao);
	khash_t(buff) *list = glstate->buffers;
    if (list) {
        khint_t k;
        glbuffer_t *buff;
        for (int i = 0; i < n; i++) {
            GLuint t = buffers[i];
            DBG(printf("\t deleting %d\n", t);)
            if (t) {    // don't allow to remove default one
                k = kh_get(buff, list, t);
                if (k != kh_end(list)) {
                    buff = kh_value(list, k);
                    if(buff->real_buffer) {
                        rebind_real_buff_arrays(buff->real_buffer, 0);  // unbind
                        LOAD_GLES(glDeleteBuffers);
                        deleteSingleBuffer(buff->real_buffer);
                    }
                    if (glstate->vao->vertex == buff)
                        glstate->vao->vertex = NULL;
                    if (glstate->vao->elements == buff)
                        glstate->vao->elements = NULL;
                    if (glstate->vao->pack == buff)
                        glstate->vao->pack = NULL;
                    if (glstate->vao->unpack == buff)
                        glstate->vao->unpack = NULL;
                    for (int j = 0; j < hardext.maxvattrib; j++)
                        if (glstate->vao->vertexattrib[j].vbuf == buff) glstate->vao->vertexattrib[j].vbuf = NULL;
                    for (int j = 0; j < hardext.maxvattrib; j++)
                        if (glstate->vao->vertexattrib[j].buffer == buff) {
                            glstate->vao->vertexattrib[j].buffer = NULL;
                            glstate->vao->vertexattrib[j].real_buffer = 0;
                            glstate->vao->vertexattrib[j].real_pointer = 0;
                        }
                    DBG(printf("\t buff->data = %p\n", buff->data);)
                    if (buff->data) free(buff->data);
                    buffer_reset_stale(buff);
                    kh_del(buff, list, k);
                    free(buff);
                }
            }
        }
    }
    DBG(printf("\t done\n");)
    noerrorShim();
}

GLboolean APIENTRY_GL4ES gl4es_glIsBuffer(GLuint buffer) {
    DBG(printf("glIsBuffer(%u)\n", buffer);)
	khash_t(buff) *list = glstate->buffers;
	khint_t k;
	noerrorShim();
    if (list) {
		k = kh_get(buff, list, buffer);
		if (k != kh_end(list)) {
			return GL_TRUE;
		}
	}
	return GL_FALSE;
}


static void bufferGetParameteriv(glbuffer_t* buff, GLenum value, GLint * data) {
	noerrorShim();
	switch (value) {
		case GL_BUFFER_ACCESS:
			data[0] = buff->access;
			break;
		case GL_BUFFER_ACCESS_FLAGS:
			data[0] = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
			break;
		case GL_BUFFER_MAPPED:
			data[0]=(buff->mapped)?GL_TRUE:GL_FALSE;
			break;
		case GL_BUFFER_MAP_LENGTH:
			data[0]=(buff->mapped)?buff->size:0;
			break;
		case GL_BUFFER_MAP_OFFSET:
			data[0]=0;
			break;
		case GL_BUFFER_SIZE:
			data[0] = buff->size;
			break;
		case GL_BUFFER_USAGE:
			data[0] = buff->usage;
			break;
		default:
			errorShim(GL_INVALID_ENUM);
		/* TODO Error if something else */
	}
}

void APIENTRY_GL4ES gl4es_glGetBufferParameteriv(GLenum target, GLenum value, GLint * data) {
    if (es_only_target(target)) {
        LOAD_GLES(glGetBufferParameteriv);
        gles_glGetBufferParameteriv(target, value, data);
        errorGL();
        return;
    }
    DBG(printf("glGetBufferParameteriv(%s, %s, %p)\n", PrintEnum(target), PrintEnum(value), data);)
	if (!buffer_target(target)) {
		errorShim(GL_INVALID_ENUM);
		return;
	}
	glbuffer_t* buff = getbuffer_buffer(target);
	if (buff==NULL) {
		errorShim(GL_INVALID_OPERATION);
		return;		// Should generate an error!
	}
    bufferGetParameteriv(buff, value, data);
}
void APIENTRY_GL4ES gl4es_glGetNamedBufferParameteriv(GLuint buffer, GLenum value, GLint * data) {
    DBG(printf("glGetNamedBufferParameteriv(%u, %s, %p)\n", buffer, PrintEnum(value), data);)
	glbuffer_t* buff = getbuffer_id(buffer);
	if (buff==NULL) {
		errorShim(GL_INVALID_OPERATION);
		return;		// Should generate an error!
	}
    bufferGetParameteriv(buff, value, data);
}

void* APIENTRY_GL4ES gl4es_glMapBuffer(GLenum target, GLenum access) {
    DBG(printf("glMapBuffer(%s, %s)\n", PrintEnum(target), PrintEnum(access));)
	if (!buffer_target(target)) {
		errorShim(GL_INVALID_ENUM);
		return (void*)NULL;
	}

    if(target==GL_ARRAY_BUFFER)
        VaoSharedClear(glstate->vao);

    glbuffer_t *buff = getbuffer_buffer(target);
    if (buff==NULL) {
        errorShim(GL_INVALID_VALUE);
		return NULL;
    }
    if(buff->mapped) {
        errorShim(GL_INVALID_OPERATION);
        return NULL;
    }
	buff->access = access;	// not used
	buff->mapped = 1;
    buff->ranged = 0;
	noerrorShim();
	return buff->data;		// Not nice, should do some copy or something probably
}
void* APIENTRY_GL4ES gl4es_glMapNamedBuffer(GLuint buffer, GLenum access) {
    DBG(printf("glMapNamedBuffer(%u, %s)\n", buffer, PrintEnum(access));)

	glbuffer_t *buff = getbuffer_id(buffer);
	if (buff==NULL) {
        errorShim(GL_INVALID_VALUE);
		return NULL;
    }
    if(buff->mapped) {
        errorShim(GL_INVALID_OPERATION);
        return NULL;
    }
	buff->access = access;	// not used
	buff->mapped = 1;
    buff->ranged = 0;
	noerrorShim();
	return buff->data;		// Not nice, should do some copy or something probably
}

GLboolean APIENTRY_GL4ES gl4es_glUnmapBuffer(GLenum target) {
    if (es_only_target(target)) {
        es3_unmap_t f = (es3_unmap_t)es3_fn("glUnmapBuffer");
        GLboolean r = f ? f(target) : GL_FALSE;
        errorGL();
        return r;
    }
    DBG(printf("glUnmapBuffer(%s)\n", PrintEnum(target));)
    if(glstate->list.compiling) {errorShim(GL_INVALID_OPERATION); return GL_FALSE;}
    FLUSH_BEGINEND;
        
	if (!buffer_target(target)) {
		errorShim(GL_INVALID_ENUM);
		return GL_FALSE;
	}

    if(target==GL_ARRAY_BUFFER)
        VaoSharedClear(glstate->vao);
        
    glbuffer_t *buff = getbuffer_buffer(target);
    if (buff==NULL) {
        errorShim(GL_INVALID_VALUE);
		return GL_FALSE;
    }
	noerrorShim();
    // The app wrote into the shadow copy. Do not send it now: the blocks are only marked and the draw that reads them sends them
    // (gl4es_buffer_resync_range), like Mesa's valid_buffer_range.
    if(buff->real_buffer && (buff->type==GL_ARRAY_BUFFER || buff->type==GL_ELEMENT_ARRAY_BUFFER) && buff->mapped && !buff->ranged && (buff->access==GL_WRITE_ONLY || buff->access==GL_READ_WRITE))
        buffer_mark_stale(buff, 0, buff->size);
    if(buff->real_buffer && (buff->type==GL_ARRAY_BUFFER || buff->type==GL_ELEMENT_ARRAY_BUFFER) && buff->mapped && buff->ranged && (buff->access&GL_MAP_WRITE_BIT_EXT) && !(buff->access&GL_MAP_FLUSH_EXPLICIT_BIT_EXT))
        buffer_mark_stale(buff, buff->offset, buff->offset+buff->length);
    if (buff->mapped) {
		buff->mapped = 0;
        buff->ranged = 0;
		return GL_TRUE;
	}
	return GL_FALSE;
}
GLboolean APIENTRY_GL4ES gl4es_glUnmapNamedBuffer(GLuint buffer) {
    DBG(printf("glUnmapNamedBuffer(%u)\n", buffer);)
    if(glstate->list.compiling) {errorShim(GL_INVALID_OPERATION); return GL_FALSE;}
    FLUSH_BEGINEND;
        
	glbuffer_t *buff = getbuffer_id(buffer);
	if (buff==NULL)
		return GL_FALSE;		// Should generate an error!
	noerrorShim();
    if(buff->real_buffer && (buff->type==GL_ARRAY_BUFFER || buff->type==GL_ELEMENT_ARRAY_BUFFER) && buff->mapped && (buff->access==GL_WRITE_ONLY || buff->access==GL_READ_WRITE))
        buffer_mark_stale(buff, 0, buff->size);     // lazy upload, see glUnmapBuffer
    if(buff->real_buffer && (buff->type==GL_ARRAY_BUFFER || buff->type==GL_ELEMENT_ARRAY_BUFFER) && buff->mapped && buff->ranged && (buff->access&GL_MAP_WRITE_BIT_EXT) && !(buff->access&GL_MAP_FLUSH_EXPLICIT_BIT_EXT))
        buffer_mark_stale(buff, buff->offset, buff->offset+buff->length);
	if (buff->mapped) {
		buff->mapped = 0;
        buff->ranged = 0;
		return GL_TRUE;
	}
	return GL_FALSE;
}

void APIENTRY_GL4ES gl4es_glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, GLvoid * data) {
    if (es_only_target(target)) {
        // ES has no glGetBufferSubData: read it through a mapping
        es3_map_t m = (es3_map_t)es3_fn("glMapBufferRange");
        es3_unmap_t u = (es3_unmap_t)es3_fn("glUnmapBuffer");
        if (m && u) {
            void* p = m(target, offset, size, 0x0001 /* GL_MAP_READ_BIT */);
            if (p) { memcpy(data, p, (size_t)size); u(target); }
        }
        errorGL();
        return;
    }
    DBG(printf("glGetBufferSubData(%s, %p, %zi, %p)\n", PrintEnum(target), (void*)offset, size, data);)
	if (!buffer_target(target)) {
		errorShim(GL_INVALID_ENUM);
		return;
	}
	glbuffer_t *buff = getbuffer_buffer(target);

	if (buff==NULL)
		return;		// Should generate an error!
	// TODO, check parameter consistancie
    memcpy(data, (char*)buff->data+offset, size);
	noerrorShim();
}
void APIENTRY_GL4ES gl4es_glGetNamedBufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, GLvoid * data) {
    DBG(printf("glGetNamedBufferSubData(%u, %p, %zi, %p)\n", buffer, (void*)offset, size, data);)
	glbuffer_t *buff = getbuffer_id(buffer);

	if (buff==NULL)
		return;		// Should generate an error!
	// TODO, check parameter consistancie
    memcpy(data, (char*)buff->data+offset, size);
	noerrorShim();
}

void APIENTRY_GL4ES gl4es_glGetBufferPointerv(GLenum target, GLenum pname, GLvoid ** params) {
    DBG(printf("glGetBufferPointerv(%s, %s, %p)\n", PrintEnum(target), PrintEnum(pname), params);)
	if (!buffer_target(target)) {
		errorShim(GL_INVALID_ENUM);
		return;
	}
	glbuffer_t *buff = getbuffer_buffer(target);
	if (buff==NULL)
		return;		// Should generate an error!
	if (pname != GL_BUFFER_MAP_POINTER) {
		errorShim(GL_INVALID_ENUM);
		return;
	}
	if (!buff->mapped) {
		params[0] = NULL;
	} else {
		params[0] = buff->data;
	}
}
void APIENTRY_GL4ES gl4es_glGetNamedBufferPointerv(GLuint buffer, GLenum pname, GLvoid ** params) {
    DBG(printf("glGetNamedBufferPointerv(%u, %s, %p)\n", buffer, PrintEnum(pname), params);)
	glbuffer_t *buff = getbuffer_id(buffer);
	if (buff==NULL)
		return;		// Should generate an error!
	if (pname != GL_BUFFER_MAP_POINTER) {
		errorShim(GL_INVALID_ENUM);
		return;
	}
	if (!buff->mapped) {
		params[0] = NULL;
	} else {
		params[0] = buff->data;
	}
}

void* APIENTRY_GL4ES gl4es_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
    if (es_only_target(target)) {
        es3_map_t f = (es3_map_t)es3_fn("glMapBufferRange");
        void* r = f ? f(target, offset, length, access) : NULL;
        errorGL();
        return r;
    }
    DBG(printf("glMapBufferRange(%s, %p, %zd, 0x%x)\n", PrintEnum(target), (void*)offset, length, access);)
	if (!buffer_target(target)) {
		errorShim(GL_INVALID_ENUM);
		return NULL;
	}

	glbuffer_t *buff = getbuffer_buffer(target);
	if (buff==NULL) {
        errorShim(GL_INVALID_VALUE);
		return NULL;		// Should generate an error!
    }
    if(buff->mapped) {
        errorShim(GL_INVALID_OPERATION);
        return NULL;
    }
	buff->access = access;
	buff->mapped = 1;
    buff->ranged = 1;
    buff->offset = offset;
    buff->length = length;
	noerrorShim();
    uintptr_t ret = (uintptr_t)buff->data;
    ret += offset;
	return (void*)ret;
}
void APIENTRY_GL4ES gl4es_glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length)
{
    if (es_only_target(target)) {
        es3_flush_t f = (es3_flush_t)es3_fn("glFlushMappedBufferRange");
        if (f) f(target, offset, length);
        errorGL();
        return;
    }
    DBG(printf("glFlushMappedBufferRange(%s, %p, %zd)\n", PrintEnum(target), (void*)offset, length);)
	if (!buffer_target(target)) {
		errorShim(GL_INVALID_ENUM);
		return;
	}

    if(target==GL_ARRAY_BUFFER)
        VaoSharedClear(glstate->vao);

    glbuffer_t *buff = getbuffer_buffer(target);
    if(!buff) {
        errorShim(GL_INVALID_VALUE);
        return;
    }
    if(!buff->mapped || !buff->ranged || !(buff->access&GL_MAP_FLUSH_EXPLICIT_BIT_EXT)) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }

    if(buff->real_buffer && (buff->type==GL_ARRAY_BUFFER || buff->type==GL_ELEMENT_ARRAY_BUFFER) && (buff->access&GL_MAP_WRITE_BIT_EXT)) {
        LOAD_GLES(glBufferSubData);
        bindBuffer(buff->type, buff->real_buffer);
        gles_glBufferSubData(buff->type, buff->offset+offset, length, (void*)((uintptr_t)buff->data+buff->offset+offset));
        // the application may keep writing into the flushed range after the flush (a mapped buffer is coherent memory on desktop
        // drivers: the data is seen by the draw anyway). Remember the range so the draw re-sends it (gl4es_buffer_resync).
        GLintptr lo = buff->offset+offset, hi = lo + length;
        if(buff->rs_hi <= buff->rs_lo) { buff->rs_lo = lo; buff->rs_hi = hi; }
        else { if(lo < buff->rs_lo) buff->rs_lo = lo; if(hi > buff->rs_hi) buff->rs_hi = hi; }
    }
}

// Re-send, from the shadow copy, the ranges the application flushed (glFlushMappedBufferRange) since the last draw. The application
// can write into a range after flushing it and before the draw that reads it; the GLES buffer then still holds the previous frame's
// vertices (sprites drawn at their neighbour's position). Cheap: only the flushed union is re-sent, once.
static void buffer_resync_flushed(glbuffer_t *buff) {
    if(buff->rs_hi <= buff->rs_lo) return;
    GLintptr lo = buff->rs_lo, hi = buff->rs_hi;
    buff->rs_lo = buff->rs_hi = 0;
    if(hi > (GLintptr)buff->size) hi = (GLintptr)buff->size;
    if(hi <= lo) return;
    LOAD_GLES(glBufferSubData);
    bindBuffer(buff->type, buff->real_buffer);
    gles_glBufferSubData(buff->type, lo, hi - lo, (void*)((uintptr_t)buff->data + lo));
}

// Lazy upload of the bytes the app wrote through a mapping (glUnmapBuffer marks them, buffer_mark_stale). A mapping is a pointer to
// the shadow copy; the real VBO only has to hold what a draw reads, so an app that maps a large buffer every frame and writes a
// small part does not pay a full copy per frame.
void gl4es_buffer_resync_range(glbuffer_t *buff, GLintptr lo, GLintptr hi) {
    if(!buff || !buff->real_buffer || !buff->data) return;
    buffer_resync_flushed(buff);
    if(!buff->nstale || !buff->stale) return;
    if(lo < 0) lo = 0;
    if(hi > (GLintptr)buff->size) hi = (GLintptr)buff->size;
    if(hi <= lo) return;
    GLsizeiptr first = lo >> STALE_SHIFT, last = (hi - 1) >> STALE_SHIFT;
    if(last >= buff->stale_n) last = buff->stale_n - 1;
    LOAD_GLES(glBufferSubData);
    int bound = 0;
    for(GLsizeiptr b = first; b <= last && buff->nstale; ) {
        if(!buff->stale[b]) { ++b; continue; }
        GLsizeiptr e = b;
        while(e <= last && buff->stale[e]) { buff->stale[e] = 0; --buff->nstale; ++e; }   // one upload per run of stale blocks
        GLintptr from = (GLintptr)b << STALE_SHIFT, to = (GLintptr)e << STALE_SHIFT;
        if(to > (GLintptr)buff->size) to = (GLintptr)buff->size;
        if(!bound) { bindBuffer(buff->type, buff->real_buffer); bound = 1; }
        gles_glBufferSubData(buff->type, from, to - from, (void*)((uintptr_t)buff->data + from));
        b = e;
    }
}

void gl4es_buffer_resync(glbuffer_t *buff) {
    if(!buff) return;
    gl4es_buffer_resync_range(buff, 0, (GLintptr)buff->size);
}

void APIENTRY_GL4ES gl4es_glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size)
{
    if (es_only_target(readTarget) && es_only_target(writeTarget)) {
        es3_copy_t f = (es3_copy_t)es3_fn("glCopyBufferSubData");
        if (f) f(readTarget, writeTarget, readOffset, writeOffset, size);
        errorGL();
        return;
    }
    DBG(printf("glCopyBufferSubData(%s, %s, %p, %p, %zd)\n", PrintEnum(readTarget), PrintEnum(writeTarget), (void*)readOffset, (void*)writeOffset, size);)
    //TODO: Add GL_COPY_READ_BUFFER and GL_COPY_WRITE_BUFFER (and GL_QUERY_BUFFER?)
	glbuffer_t *readbuff = getbuffer_buffer(readTarget);
	glbuffer_t *writebuff = getbuffer_buffer(writeTarget);
    if(!readbuff || !writebuff) {
        errorShim(GL_INVALID_VALUE);
        return;
    }
    if(writebuff->ranged && !(writebuff->access&GL_MAP_PERSISTENT_BIT)) {
        errorShim(GL_INVALID_OPERATION);
        return;
    }
    // TODO: check memory overlap and overread/overwrite
    memcpy((char*)writebuff->data+writeOffset, (char*)readbuff->data+readOffset, size);
    if(writebuff->real_buffer && (writebuff->type==GL_ARRAY_BUFFER || writebuff->type==GL_ELEMENT_ARRAY_BUFFER) && writebuff->mapped && (writebuff->access==GL_WRITE_ONLY || writebuff->access==GL_READ_WRITE)) {
        LOAD_GLES(glBufferSubData);
        bindBuffer(writebuff->type, writebuff->real_buffer);
        gles_glBufferSubData(writebuff->type, writeOffset, size, (char*)writebuff->data+writeOffset);
    }
    noerrorShim();
}

void bindBuffer(GLenum target, GLuint buffer)
{
    LOAD_GLES(glBindBuffer);
    if(target==GL_ARRAY_BUFFER) {
        if(glstate->bind_buffer.array == buffer)
            return;
        DBG(printf("Bind buffer %d to GL_ARRAY_BUFFER\n", buffer);)
        glstate->bind_buffer.array = buffer;
        gles_glBindBuffer(target, buffer);
        
    } else if (target==GL_ELEMENT_ARRAY_BUFFER) {
        glstate->bind_buffer.want_index = buffer;
        if(glstate->bind_buffer.index == buffer)
            return;
        glstate->bind_buffer.index = buffer;
        DBG(printf("Bind buffer %d to GL_ELEMENT_ARRAY_BUFFER\n", buffer);)
        gles_glBindBuffer(target, buffer);
    } else {
        LOGE("Warning, unhandled Buffer type %s in bindBuffer\n", PrintEnum(target));
        return;
    }
    glstate->bind_buffer.used = (glstate->bind_buffer.index || glstate->bind_buffer.array)?1:0;   // any real buffer bound: unboundBuffers() must clear it
}

GLuint wantBufferIndex(GLuint buffer)
{
    GLuint ret = glstate->bind_buffer.want_index;
    glstate->bind_buffer.want_index = buffer;
    return ret;
}

void realize_bufferIndex()
{
    LOAD_GLES(glBindBuffer);
    if(glstate->bind_buffer.index != glstate->bind_buffer.want_index) {
        glstate->bind_buffer.index = glstate->bind_buffer.want_index;
        gles_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, glstate->bind_buffer.index);
        DBG(printf("Bind buffer %d to GL_ELEMENT_ARRAY_BUFFER\n", glstate->bind_buffer.index);)
        glstate->bind_buffer.used = (glstate->bind_buffer.index || glstate->bind_buffer.array)?1:0;   // any real buffer bound: unboundBuffers() must clear it
    }
}

void deleteSingleBuffer(GLuint buffer) {
   LOAD_GLES(glDeleteBuffers);
   if(glstate->bind_buffer.index == buffer) glstate->bind_buffer.index = 0;
   else if(glstate->bind_buffer.want_index == buffer) glstate->bind_buffer.want_index = 0;
   else if(glstate->bind_buffer.array == buffer) glstate->bind_buffer.array = 0;
   gles_glDeleteBuffers(1, &buffer);
}

// Before a draw from client memory (blit quad, lists...): no real GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER may stay bound, or GLES
// reads the client pointers as offsets into that buffer. `used` is set whenever either of them is bound.
void unboundBuffers()
{
    if(!glstate->bind_buffer.used)
        return;
    LOAD_GLES(glBindBuffer);
    if(glstate->bind_buffer.array) {
        glstate->bind_buffer.array = 0;
        gles_glBindBuffer(GL_ARRAY_BUFFER, 0);
        DBG(printf("Bind buffer %d to GL_ARRAY_BUFFER\n", 0);)
    }
    if(glstate->bind_buffer.index) {
        glstate->bind_buffer.index = 0;
        glstate->bind_buffer.want_index = 0;
        gles_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        DBG(printf("Bind buffer %d to GL_ELEMENT_ARRAY_BUFFER\n", 0);)
    }
    glstate->bind_buffer.used = 0;
}


//Direct wrapper
AliasExport(void,glGenBuffers,,(GLsizei n, GLuint * buffers));
AliasExport(void,glBindBuffer,,(GLenum target, GLuint buffer));
AliasExport(void,glBufferData,,(GLenum target, GLsizeiptr size, const GLvoid * data, GLenum usage));
AliasExport(void,glBufferSubData,,(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid * data));
AliasExport(void,glDeleteBuffers,,(GLsizei n, const GLuint * buffers));
AliasExport(GLboolean,glIsBuffer,,(GLuint buffer));
AliasExport(void,glGetBufferParameteriv,,(GLenum target, GLenum value, GLint * data));
AliasExport(void*,glMapBuffer,,(GLenum target, GLenum access));
AliasExport(GLboolean,glUnmapBuffer,,(GLenum target));
AliasExport(void,glGetBufferSubData,,(GLenum target, GLintptr offset, GLsizeiptr size, GLvoid * data));
AliasExport(void,glGetBufferPointerv,,(GLenum target, GLenum pname, GLvoid ** params));

AliasExport(void*,glMapBufferRange,,(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access));
AliasExport(void,glFlushMappedBufferRange,,(GLenum target, GLintptr offset, GLsizeiptr length));

AliasExport(void,glCopyBufferSubData,,(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size));
//ARB wrapper
#ifndef AMIGAOS4
AliasExport(void,glGenBuffers,ARB,(GLsizei n, GLuint * buffers));
AliasExport(void,glBindBuffer,ARB,(GLenum target, GLuint buffer));
AliasExport(void,glBufferData,ARB,(GLenum target, GLsizeiptr size, const GLvoid * data, GLenum usage));
AliasExport(void,glBufferSubData,ARB,(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid * data));
AliasExport(void,glDeleteBuffers,ARB,(GLsizei n, const GLuint * buffers));
AliasExport(GLboolean,glIsBuffer,ARB,(GLuint buffer));
AliasExport(void,glGetBufferParameteriv,ARB,(GLenum target, GLenum value, GLint * data));
AliasExport(void*,glMapBuffer,ARB,(GLenum target, GLenum access));
AliasExport(GLboolean,glUnmapBuffer,ARB,(GLenum target));
AliasExport(void,glGetBufferSubData,ARB,(GLenum target, GLintptr offset, GLsizeiptr size, GLvoid * data));
AliasExport(void,glGetBufferPointerv,ARB,(GLenum target, GLenum pname, GLvoid ** params));
#endif

//Direct Access
AliasExport(void,glNamedBufferData,,(GLuint buffer, GLsizeiptr size, const GLvoid * data, GLenum usage));
AliasExport(void,glNamedBufferSubData,,(GLuint buffer, GLintptr offset, GLsizeiptr size, const GLvoid * data));
AliasExport(void,glGetNamedBufferParameteriv,,(GLuint buffer, GLenum value, GLint * data));
AliasExport(void*,glMapNamedBuffer,,(GLuint buffer, GLenum access));
AliasExport(GLboolean,glUnmapNamedBuffer,,(GLuint buffer));
AliasExport(void,glGetNamedBufferSubData,,(GLuint buffer, GLintptr offset, GLsizeiptr size, GLvoid * data));
AliasExport(void,glGetNamedBufferPointerv,,(GLuint buffer, GLenum pname, GLvoid ** params));

AliasExport(void,glNamedBufferData,EXT,(GLuint buffer, GLsizeiptr size, const GLvoid * data, GLenum usage));
AliasExport(void,glNamedBufferSubData,EXT,(GLuint buffer, GLintptr offset, GLsizeiptr size, const GLvoid * data));
AliasExport(void,glGetNamedBufferParameteriv,EXT,(GLuint buffer, GLenum value, GLint * data));
AliasExport(void*,glMapNamedBuffer,EXT,(GLuint buffer, GLenum access));
AliasExport(GLboolean,glUnmapNamedBuffer,EXT,(GLuint buffer));
AliasExport(void,glGetNamedBufferSubData,EXT,(GLuint buffer, GLintptr offset, GLsizeiptr size, GLvoid * data));
AliasExport(void,glGetNamedBufferPointerv,EXT,(GLuint buffer, GLenum pname, GLvoid ** params));


// VAO ****************
static GLuint lastvao = 1;

void APIENTRY_GL4ES gl4es_glGenVertexArrays(GLsizei n, GLuint *arrays) {
    DBG(printf("glGenVertexArrays(%i, %p)\n", n, arrays);)
	noerrorShim();
    if (n<1) {
		errorShim(GL_INVALID_VALUE);
        return;
    }
    for (int i=0; i<n; i++) {   // TODO: create VAO here and check unicity
        arrays[i] = lastvao++;
    }
}
void APIENTRY_GL4ES gl4es_glBindVertexArray(GLuint array) {
    DBG(printf("glBindVertexArray(%u)\n", array);)
    FLUSH_BEGINEND;

   	khint_t k;
   	int ret;
	khash_t(glvao) *list = glstate->vaos;
    // if array = 0 => unbind buffer!
    if (array == 0) {
        // unbind buffer
        glstate->vao = glstate->defaultvao;
    } else {
        // search for an existing buffer
        k = kh_get(glvao, list, array);
        glvao_t *glvao = NULL;
        if (k == kh_end(list)){
            k = kh_put(glvao, list, array, &ret);
            glvao = kh_value(list, k) = malloc(sizeof(glvao_t));
            // new vao is binded to nothing
            VaoInit(glvao);
            // Copy current status to new VAO
            glvao->vertex = glstate->vao->vertex;
            glvao->elements = glstate->vao->elements;
            glvao->pack = glstate->vao->pack;
            glvao->unpack = glstate->vao->unpack;
            glvao->maxtex = glstate->vao->maxtex;

            // just put is number
            glvao->array = array;
        } else {
            glvao = kh_value(list, k);
        }
        glstate->vao = glvao;
    }

    noerrorShim();
}
void APIENTRY_GL4ES gl4es_glDeleteVertexArrays(GLsizei n, const GLuint *arrays) {
    DBG(printf("glDeleteVertexArrays(%i, %p)\n", n, arrays);)
    if(!glstate) return;
    FLUSH_BEGINEND;

	khash_t(glvao) *list = glstate->vaos;
    if (list) {
        khint_t k;
        glvao_t *glvao;
        for (int i = 0; i < n; i++) {
            GLuint t = arrays[i];
            if (t) {    // don't allow to remove the default one
                k = kh_get(glvao, list, t);
                if (k != kh_end(list)) {
                    glvao = kh_value(list, k);
                    VaoSharedClear(glvao);
                    kh_del(glvao, list, k);
                    //free(glvao);  //let the use delete those
                }
            }
        }
    }
    noerrorShim();
}
GLboolean APIENTRY_GL4ES gl4es_glIsVertexArray(GLuint array) {
    DBG(printf("glIsVertexArray(%u)\n", array);)
    if(!glstate)
        return GL_FALSE;
	khash_t(glvao) *list = glstate->vaos;
	khint_t k;
	noerrorShim();
    if (list) {
		k = kh_get(glvao, list, array);
		if (k != kh_end(list)) {
			return GL_TRUE;
		}
	}
	return GL_FALSE;
}

void VaoSharedClear(glvao_t *vao) {
    if(vao==NULL || vao->shared_arrays==NULL)
        return;
    if(!(--(*vao->shared_arrays))) {
        free(vao->vert.ptr);
        free(vao->color.ptr);
        free(vao->secondary.ptr);
        free(vao->normal.ptr);
        for (int i=0; i<hardext.maxtex; i++)
            free(vao->tex[i].ptr);
        free(vao->shared_arrays);
    }
    vao->vert.ptr = NULL;
    vao->color.ptr = NULL;
    vao->secondary.ptr = NULL;
    vao->normal.ptr = NULL;
    for (int i=0; i<hardext.maxtex; i++)
        vao->tex[i].ptr = NULL;
    vao->shared_arrays = NULL;
}

void VaoInit(glvao_t *vao) {
    memset(vao, 0, sizeof(glvao_t));
    for (int i=0; i<hardext.maxvattrib; i++) {
        vao->vertexattrib[i].size = 4;
        vao->vertexattrib[i].type = GL_FLOAT;
    }
}

//Direct wrapper
AliasExport(void,glGenVertexArrays,,(GLsizei n, GLuint *arrays));
AliasExport(void,glBindVertexArray,,(GLuint array));
AliasExport(void,glDeleteVertexArrays,,(GLsizei n, const GLuint *arrays));
AliasExport(GLboolean,glIsVertexArray,,(GLuint array));

