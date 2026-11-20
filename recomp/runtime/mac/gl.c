/* OpenGL for the guest: the entry points gen_gl.py cannot wrap mechanically, the implementation table, and the CGL
 * queries Aspyr's display code makes.
 *
 * The implementation is chosen once (gl_bind): Apple's OpenGL (the reference backend, HW_GL=apple) or the port's own
 * GL on Metal (runtime/mac/mgl, the default once it covers the game). Buffer bindings are tracked so that pointer
 * arguments that are offsets into a bound buffer (vertex arrays, indices, pixel unpack/pack) pass through unchanged
 * while client memory pointers are translated. */
#include "mac.h"
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include "gl_rt.h"
#include <dlfcn.h>
#include <OpenGL/OpenGL.h>

/* every GL call below goes to the chosen implementation (gl_bind): Apple's OpenGL or mgl (Metal) */
void *gl_fn(const char *name);
#define GLF_(name) ({ static void *p_; if (!p_) p_ = gl_fn(#name); (__typeof__(&name))p_; })
#define glGetString GLF_(glGetString)
#define glGetStringi GLF_(glGetStringi)
#define glBindBuffer GLF_(glBindBuffer)
#define glBindVertexArray GLF_(glBindVertexArray)
#define glDeleteBuffers GLF_(glDeleteBuffers)
#define glGetIntegerv GLF_(glGetIntegerv)
#define glMapBufferRange GLF_(glMapBufferRange)
#define glGetBufferParameteriv GLF_(glGetBufferParameteriv)
#define glMapBuffer GLF_(glMapBuffer)
#define glUnmapBuffer GLF_(glUnmapBuffer)
#define glFlushMappedBufferRange GLF_(glFlushMappedBufferRange)
#define glVertexAttribPointer GLF_(glVertexAttribPointer)
#define glVertexAttribIPointer GLF_(glVertexAttribIPointer)
#define glDrawElements GLF_(glDrawElements)
#define glDrawRangeElements GLF_(glDrawRangeElements)
#define glDrawElementsInstanced GLF_(glDrawElementsInstanced)
#define glDrawElementsBaseVertex GLF_(glDrawElementsBaseVertex)
#define glDrawRangeElementsBaseVertex GLF_(glDrawRangeElementsBaseVertex)
#define glDrawElementsInstancedBaseVertex GLF_(glDrawElementsInstancedBaseVertex)
#define glTexImage1D GLF_(glTexImage1D)
#define glTexImage2D GLF_(glTexImage2D)
#define glTexImage3D GLF_(glTexImage3D)
#define glTexSubImage1D GLF_(glTexSubImage1D)
#define glTexSubImage2D GLF_(glTexSubImage2D)
#define glTexSubImage3D GLF_(glTexSubImage3D)
#define glCompressedTexImage2D GLF_(glCompressedTexImage2D)
#define glCompressedTexImage3D GLF_(glCompressedTexImage3D)
#define glCompressedTexSubImage2D GLF_(glCompressedTexSubImage2D)
#define glCompressedTexSubImage3D GLF_(glCompressedTexSubImage3D)
#define glReadPixels GLF_(glReadPixels)
#define glGetTexImage GLF_(glGetTexImage)
#define glGetCompressedTexImage GLF_(glGetCompressedTexImage)
#define glShaderSource GLF_(glShaderSource)
#define glTransformFeedbackVaryings GLF_(glTransformFeedbackVaryings)
#define glGetUniformIndices GLF_(glGetUniformIndices)
#define glFenceSync GLF_(glFenceSync)
#define glClientWaitSync GLF_(glClientWaitSync)
#define glWaitSync GLF_(glWaitSync)
#define glDeleteSync GLF_(glDeleteSync)
#define glIsSync GLF_(glIsSync)
#define glGetSynciv GLF_(glGetSynciv)
#define glGetError GLF_(glGetError)
/* legacy (compatibility profile) entry points the core header leaves out: called through Apple's OpenGL by name */
typedef void *GLhandleARB_; typedef char GLcharARB_;
#define LEGACY(ret, name, args, call) static ret name##_ args { static ret (*f) args; if (!f) f = dlsym(gl_lib(), #name); return f call; }
static void *gl_lib(void);
LEGACY(void, glVertexPointer, (GLint a, GLenum b, GLsizei s, const void *p), (a, b, s, p))
LEGACY(void, glColorPointer, (GLint a, GLenum b, GLsizei s, const void *p), (a, b, s, p))
LEGACY(void, glTexCoordPointer, (GLint a, GLenum b, GLsizei s, const void *p), (a, b, s, p))
LEGACY(void, glSecondaryColorPointer, (GLint a, GLenum b, GLsizei s, const void *p), (a, b, s, p))
LEGACY(void, glNormalPointer, (GLenum b, GLsizei s, const void *p), (b, s, p))
LEGACY(void, glFogCoordPointer, (GLenum b, GLsizei s, const void *p), (b, s, p))
LEGACY(void, glShaderSourceARB, (GLhandleARB_ h, GLsizei n, const GLcharARB_ **s, const GLint *l), (h, n, s, l))
#define glVertexPointer glVertexPointer_
#define glColorPointer glColorPointer_
#define glTexCoordPointer glTexCoordPointer_
#define glSecondaryColorPointer glSecondaryColorPointer_
#define glNormalPointer glNormalPointer_
#define glFogCoordPointer glFogCoordPointer_
#define glShaderSourceARB glShaderSourceARB_
#define GLhandleARB GLhandleARB_
#define GLcharARB GLcharARB_

int gl_trace, gl_check;
void gl_check_error(const char *name)             /* HW_GL_ERRORS=1: the first few errors per entry point */
{
    GLenum e = glGetError(); if (!e) return;
    static struct { const char *n; int k; } seen[512]; static int ns; int i = 0;
    for (; i < ns; i++) if (seen[i].n == name) break;
    if (i == ns && ns < 512) seen[ns++].n = name;
    if (i < 512 && seen[i].k++ < 3) hw_log("GL error %04x after %s", e, name);
}
typedef struct GLTrack {
    uint32_t array, element, pack, unpack, vao;
    uint32_t vao_element[4096];                          /* element buffer per vertex array object (small names) */
} GLTrack;
static __thread GLTrack *trk;
static GLTrack *T(void) { if (!trk) trk = calloc(1, sizeof *trk); return trk; }
#define P(name) ((void *)dlsym(gl_lib(), #name))
static void *gl_lib(void) { static void *h; if (!h) h = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW); return h; }

#include "mgl/mgl.h"
int gl_metal;                                        /* the port's own GL on Metal (mgl); HW_GL=apple: Apple's OpenGL */
static void gl_missing(void) { }                     /* entry points mgl does not provide: called, they do nothing */
void *gl_fn(const char *name)
{
    if (gl_metal) { void *f = mgl_lookup(name); if (!f) { static char seen[256][48]; static int n; int k = 0; for (; k < n; k++) if (!strcmp(seen[k], name)) break;
                                                     if (k == n && n < 256) { snprintf(seen[n++], 48, "%s", name); hw_log("mgl: %s is not implemented", name); } return (void *)gl_missing; }
                    return f; }
    return dlsym(gl_lib(), name);
}
void gl_bind(void)                                   /* fill glp[] with the implementation */
{
    static int done; if (done) return; done = 1;
    gl_trace = getenv("HW_GL_TRACE") != 0; gl_check = getenv("HW_GL_ERRORS") != 0;
    const char *b = getenv("HW_GL"); gl_metal = !(b && !strcmp(b, "apple"));   /* Metal unless HW_GL=apple */
    unsigned n = 0;
    if (gl_metal) { for (unsigned k = 0; k < gl_count; k++) { void *f = mgl_lookup(gl_names[k]); glp[k] = f ? f : (void *)gl_missing; if (f) n++; }
                    hw_log("OpenGL: %u of %u entry points on Metal (mgl)", n, gl_count); return; }
    void *h = gl_lib();
    for (unsigned k = 0; k < gl_count; k++) { glp[k] = dlsym(h, gl_names[k]); if (glp[k]) n++; }
    hw_log("OpenGL: %u of %u entry points from Apple's OpenGL (reference backend)", n, gl_count);
}
/* a guest call into an entry point the implementation lacks */
__attribute__((constructor)) static void gl_init(void) { }

/* ---- strings ---- */
static uint32_t gstr_cache(const char *s)
{
    static struct { char *s; uint32_t g; } c[256]; static int n; static pthread_mutex_t l = PTHREAD_MUTEX_INITIALIZER;
    if (!s) return 0;
    pthread_mutex_lock(&l);
    for (int k = 0; k < n; k++) if (!strcmp(c[k].s, s)) { pthread_mutex_unlock(&l); return c[k].g; }
    uint32_t g = g_strdup(s); if (n < 256) { c[n].s = strdup(s); c[n].g = g; n++; }
    pthread_mutex_unlock(&l); return g;
}
IMP_AS(gl_glGetString, "glGetString") { GL_TRACE("glGetString"); gl_bind(); RET(gstr_cache((const char *)glGetString(ARG(0)))); }
ALIAS(gl_glGetString, "_glGetString")
IMP_AS(gl_glGetStringi, "glGetStringi") { GL_TRACE("glGetStringi"); RET(gstr_cache((const char *)glGetStringi(ARG(0), ARG(1)))); }
ALIAS(gl_glGetStringi, "_glGetStringi")

/* ---- buffers ---- */
static void bind_track(uint32_t target, uint32_t buf)
{
    GLTrack *t = T();
    switch (target) {
    case GL_ARRAY_BUFFER: t->array = buf; break;
    case GL_ELEMENT_ARRAY_BUFFER: t->element = buf; if (t->vao < 4096) t->vao_element[t->vao] = buf; break;
    case GL_PIXEL_PACK_BUFFER: t->pack = buf; break;
    case GL_PIXEL_UNPACK_BUFFER: t->unpack = buf; break;
    }
}
IMP_AS(gl_glBindBuffer, "glBindBuffer") { GL_TRACE("glBindBuffer"); bind_track(ARG(0), ARG(1)); glBindBuffer(ARG(0), ARG(1)); GL_CHECK("glBindBuffer"); RETV(); }
ALIAS(gl_glBindBuffer, "_glBindBuffer")
ALIAS(gl_glBindBuffer, "glBindBufferARB")
IMP_AS(gl_glBindVertexArray, "glBindVertexArray") { GL_TRACE("glBindVertexArray"); GLTrack *t = T(); t->vao = ARG(0); t->element = t->vao < 4096 ? t->vao_element[t->vao] : 0; glBindVertexArray(ARG(0)); GL_CHECK("glBindVertexArray"); RETV(); }
ALIAS(gl_glBindVertexArray, "glBindVertexArrayAPPLE")
IMP_AS(gl_glDeleteBuffers, "glDeleteBuffers")
{
    GLTrack *t = T(); uint32_t n = ARG(0), p = ARG(1);
    for (uint32_t k = 0; k < n; k++) { uint32_t b = rt_r32(G_MEM, p + 4 * k);
        if (t->array == b) t->array = 0; if (t->element == b) t->element = 0; if (t->pack == b) t->pack = 0; if (t->unpack == b) t->unpack = 0; }
    glDeleteBuffers((GLsizei)n, gl_ptr(p)); RETV();
}
ALIAS(gl_glDeleteBuffers, "glDeleteBuffersARB")
/* mapped buffers: a guest shadow, copied back when unmapped (or flushed) */
typedef struct Map { uint32_t target, buf, guest, len, offset, access; void *host; } Map;
static Map maps[64]; static int nmaps;
static uint32_t cur_buffer(uint32_t target)
{ GLint b = 0; glGetIntegerv(target == GL_ARRAY_BUFFER ? GL_ARRAY_BUFFER_BINDING : target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING :
      target == GL_PIXEL_UNPACK_BUFFER ? GL_PIXEL_UNPACK_BUFFER_BINDING : target == GL_PIXEL_PACK_BUFFER ? GL_PIXEL_PACK_BUFFER_BINDING : GL_UNIFORM_BUFFER_BINDING, &b); return (uint32_t)b; }
static uint32_t map_buffer(uint32_t target, uint32_t offset, uint32_t len, uint32_t access, void *host)
{
    if (!host || nmaps == 64) return 0;
    uint32_t g = g_malloc(len ? len : 1);
    if (!(access & GL_MAP_INVALIDATE_RANGE_BIT) && !(access & GL_MAP_INVALIDATE_BUFFER_BIT)) memcpy(GP(g), host, len);
    maps[nmaps++] = (Map){target, cur_buffer(target), g, len, offset, access, host};
    return g;
}
static int unmap_buffer(uint32_t target)
{
    uint32_t b = cur_buffer(target);
    for (int k = nmaps - 1; k >= 0; k--) if (maps[k].target == target && maps[k].buf == b) {
        if (maps[k].access & GL_MAP_WRITE_BIT || !maps[k].access) memcpy(maps[k].host, GP(maps[k].guest), maps[k].len);
        g_free(maps[k].guest); maps[k] = maps[--nmaps];
        return glUnmapBuffer(target);
    }
    return glUnmapBuffer(target);
}
IMP_AS(gl_glMapBufferRange, "glMapBufferRange")
{
    uint32_t target = ARG(0), off = ARG(1), len = ARG(2), acc = ARG(3);
    extern int gl_metal; void *h = glMapBufferRange(target, (GLintptr)off, (GLsizeiptr)len, gl_metal ? acc : acc & ~(GLbitfield)GL_MAP_UNSYNCHRONIZED_BIT);
    RET(map_buffer(target, off, len, acc, h));
}
IMP_AS(gl_glMapBuffer, "glMapBuffer")
{
    uint32_t target = ARG(0), access = ARG(1); GLint size = 0; glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    void *h = glMapBuffer(target, access);
    RET(map_buffer(target, 0, (uint32_t)size, access == GL_READ_ONLY ? GL_MAP_READ_BIT : GL_MAP_WRITE_BIT | GL_MAP_READ_BIT, h));
}
ALIAS(gl_glMapBuffer, "glMapBufferARB")
IMP_AS(gl_glUnmapBuffer, "glUnmapBuffer") { GL_TRACE("glUnmapBuffer"); RET(unmap_buffer(ARG(0))); }
ALIAS(gl_glUnmapBuffer, "glUnmapBufferARB")
IMP_AS(gl_glFlushMappedBufferRange, "glFlushMappedBufferRange")
{
    uint32_t target = ARG(0), off = ARG(1), len = ARG(2), b = cur_buffer(target);
    for (int k = nmaps - 1; k >= 0; k--) if (maps[k].target == target && maps[k].buf == b) {
        memcpy((uint8_t *)maps[k].host + off, GP(maps[k].guest + off), len); break; }
    glFlushMappedBufferRange(target, (GLintptr)off, (GLsizeiptr)len); RETV();
}

/* ---- vertex arrays: an offset when a buffer is bound, else client memory ---- */
static const void *vptr(uint32_t p) { return T()->array ? (const void *)(uintptr_t)p : gl_ptr(p); }
static const void *iptr(uint32_t p) { return T()->element ? (const void *)(uintptr_t)p : gl_ptr(p); }
static const void *uptr(uint32_t p) { return T()->unpack ? (const void *)(uintptr_t)p : gl_ptr(p); }
static void *pptr(uint32_t p) { return T()->pack ? (void *)(uintptr_t)p : gl_ptr(p); }
IMP_AS(gl_glVertexAttribPointer, "glVertexAttribPointer") { GL_TRACE("glVertexAttribPointer"); glVertexAttribPointer(ARG(0), (GLint)ARG(1), ARG(2), (GLboolean)ARG(3), (GLsizei)ARG(4), vptr(ARG(5))); GL_CHECK("glVertexAttribPointer"); RETV(); }
ALIAS(gl_glVertexAttribPointer, "glVertexAttribPointerARB")
IMP_AS(gl_glVertexAttribIPointer, "glVertexAttribIPointer") { GL_TRACE("glVertexAttribIPointer"); glVertexAttribIPointer(ARG(0), (GLint)ARG(1), ARG(2), (GLsizei)ARG(3), vptr(ARG(4))); GL_CHECK("glVertexAttribIPointer"); RETV(); }
IMP_AS(gl_glVertexPointer, "glVertexPointer") { GL_TRACE("glVertexPointer"); glVertexPointer((GLint)ARG(0), ARG(1), (GLsizei)ARG(2), vptr(ARG(3))); GL_CHECK("glVertexPointer"); RETV(); }
IMP_AS(gl_glColorPointer, "glColorPointer") { GL_TRACE("glColorPointer"); glColorPointer((GLint)ARG(0), ARG(1), (GLsizei)ARG(2), vptr(ARG(3))); GL_CHECK("glColorPointer"); RETV(); }
IMP_AS(gl_glTexCoordPointer, "glTexCoordPointer") { GL_TRACE("glTexCoordPointer"); glTexCoordPointer((GLint)ARG(0), ARG(1), (GLsizei)ARG(2), vptr(ARG(3))); GL_CHECK("glTexCoordPointer"); RETV(); }
IMP_AS(gl_glNormalPointer, "glNormalPointer") { GL_TRACE("glNormalPointer"); glNormalPointer(ARG(0), (GLsizei)ARG(1), vptr(ARG(2))); GL_CHECK("glNormalPointer"); RETV(); }
IMP_AS(gl_glSecondaryColorPointer, "glSecondaryColorPointer") { GL_TRACE("glSecondaryColorPointer"); glSecondaryColorPointer((GLint)ARG(0), ARG(1), (GLsizei)ARG(2), vptr(ARG(3))); GL_CHECK("glSecondaryColorPointer"); RETV(); }
IMP_AS(gl_glFogCoordPointer, "glFogCoordPointer") { GL_TRACE("glFogCoordPointer"); glFogCoordPointer(ARG(0), (GLsizei)ARG(1), vptr(ARG(2))); GL_CHECK("glFogCoordPointer"); RETV(); }
IMP_AS(gl_glDrawElements, "glDrawElements") { GL_TRACE("glDrawElements"); glDrawElements(ARG(0), (GLsizei)ARG(1), ARG(2), iptr(ARG(3))); GL_CHECK("glDrawElements"); RETV(); }
ALIAS(gl_glDrawElements, "_glDrawElements")
IMP_AS(gl_glDrawRangeElements, "glDrawRangeElements") { GL_TRACE("glDrawRangeElements"); glDrawRangeElements(ARG(0), ARG(1), ARG(2), (GLsizei)ARG(3), ARG(4), iptr(ARG(5))); GL_CHECK("glDrawRangeElements"); RETV(); }
IMP_AS(gl_glDrawElementsInstanced, "glDrawElementsInstanced") { GL_TRACE("glDrawElementsInstanced"); glDrawElementsInstanced(ARG(0), (GLsizei)ARG(1), ARG(2), iptr(ARG(3)), (GLsizei)ARG(4)); GL_CHECK("glDrawElementsInstanced"); RETV(); }
ALIAS(gl_glDrawElementsInstanced, "glDrawElementsInstancedARB")
IMP_AS(gl_glDrawElementsBaseVertex, "glDrawElementsBaseVertex") { GL_TRACE("glDrawElementsBaseVertex"); glDrawElementsBaseVertex(ARG(0), (GLsizei)ARG(1), ARG(2), iptr(ARG(3)), (GLint)ARG(4)); GL_CHECK("glDrawElementsBaseVertex"); RETV(); }
IMP_AS(gl_glDrawRangeElementsBaseVertex, "glDrawRangeElementsBaseVertex") { GL_TRACE("glDrawRangeElementsBaseVertex"); glDrawRangeElementsBaseVertex(ARG(0), ARG(1), ARG(2), (GLsizei)ARG(3), ARG(4), iptr(ARG(5)), (GLint)ARG(6)); GL_CHECK("glDrawRangeElementsBaseVertex"); RETV(); }
IMP_AS(gl_glDrawElementsInstancedBaseVertex, "glDrawElementsInstancedBaseVertex") { GL_TRACE("glDrawElementsInstancedBaseVertex"); glDrawElementsInstancedBaseVertex(ARG(0), (GLsizei)ARG(1), ARG(2), iptr(ARG(3)), (GLsizei)ARG(4), (GLint)ARG(5)); GL_CHECK("glDrawElementsInstancedBaseVertex"); RETV(); }

/* ---- textures and pixels: offsets into a bound pixel buffer, or client memory ---- */
IMP_AS(gl_glTexImage1D, "glTexImage1D") { GL_TRACE("glTexImage1D"); glTexImage1D(ARG(0), (GLint)ARG(1), (GLint)ARG(2), (GLsizei)ARG(3), (GLint)ARG(4), ARG(5), ARG(6), uptr(ARG(7))); GL_CHECK("glTexImage1D"); RETV(); }
ALIAS(gl_glTexImage1D, "_glTexImage1D")
IMP_AS(gl_glTexImage2D, "glTexImage2D") { GL_TRACE("glTexImage2D"); glTexImage2D(ARG(0), (GLint)ARG(1), (GLint)ARG(2), (GLsizei)ARG(3), (GLsizei)ARG(4), (GLint)ARG(5), ARG(6), ARG(7), uptr(ARG(8))); GL_CHECK("glTexImage2D"); RETV(); }
ALIAS(gl_glTexImage2D, "_glTexImage2D")
IMP_AS(gl_glTexImage3D, "glTexImage3D") { GL_TRACE("glTexImage3D"); glTexImage3D(ARG(0), (GLint)ARG(1), (GLint)ARG(2), (GLsizei)ARG(3), (GLsizei)ARG(4), (GLsizei)ARG(5), (GLint)ARG(6), ARG(7), ARG(8), uptr(ARG(9))); GL_CHECK("glTexImage3D"); RETV(); }
IMP_AS(gl_glTexSubImage1D, "glTexSubImage1D") { GL_TRACE("glTexSubImage1D"); glTexSubImage1D(ARG(0), (GLint)ARG(1), (GLint)ARG(2), (GLsizei)ARG(3), ARG(4), ARG(5), uptr(ARG(6))); GL_CHECK("glTexSubImage1D"); RETV(); }
ALIAS(gl_glTexSubImage1D, "_glTexSubImage1D")
IMP_AS(gl_glTexSubImage2D, "glTexSubImage2D") { GL_TRACE("glTexSubImage2D"); glTexSubImage2D(ARG(0), (GLint)ARG(1), (GLint)ARG(2), (GLint)ARG(3), (GLsizei)ARG(4), (GLsizei)ARG(5), ARG(6), ARG(7), uptr(ARG(8))); GL_CHECK("glTexSubImage2D"); RETV(); }
ALIAS(gl_glTexSubImage2D, "_glTexSubImage2D")
IMP_AS(gl_glTexSubImage3D, "glTexSubImage3D") { GL_TRACE("glTexSubImage3D"); glTexSubImage3D(ARG(0), (GLint)ARG(1), (GLint)ARG(2), (GLint)ARG(3), (GLint)ARG(4), (GLsizei)ARG(5), (GLsizei)ARG(6), (GLsizei)ARG(7), ARG(8), ARG(9), uptr(ARG(10))); GL_CHECK("glTexSubImage3D"); RETV(); }
IMP_AS(gl_glCompressedTexImage2D, "glCompressedTexImage2D") { GL_TRACE("glCompressedTexImage2D"); glCompressedTexImage2D(ARG(0), (GLint)ARG(1), ARG(2), (GLsizei)ARG(3), (GLsizei)ARG(4), (GLint)ARG(5), (GLsizei)ARG(6), uptr(ARG(7))); GL_CHECK("glCompressedTexImage2D"); RETV(); }
IMP_AS(gl_glCompressedTexImage3D, "glCompressedTexImage3D") { GL_TRACE("glCompressedTexImage3D"); glCompressedTexImage3D(ARG(0), (GLint)ARG(1), ARG(2), (GLsizei)ARG(3), (GLsizei)ARG(4), (GLsizei)ARG(5), (GLint)ARG(6), (GLsizei)ARG(7), uptr(ARG(8))); GL_CHECK("glCompressedTexImage3D"); RETV(); }
IMP_AS(gl_glCompressedTexSubImage2D, "glCompressedTexSubImage2D") { GL_TRACE("glCompressedTexSubImage2D"); glCompressedTexSubImage2D(ARG(0), (GLint)ARG(1), (GLint)ARG(2), (GLint)ARG(3), (GLsizei)ARG(4), (GLsizei)ARG(5), ARG(6), (GLsizei)ARG(7), uptr(ARG(8))); GL_CHECK("glCompressedTexSubImage2D"); RETV(); }
IMP_AS(gl_glCompressedTexSubImage3D, "glCompressedTexSubImage3D") { GL_TRACE("glCompressedTexSubImage3D"); glCompressedTexSubImage3D(ARG(0), (GLint)ARG(1), (GLint)ARG(2), (GLint)ARG(3), (GLint)ARG(4), (GLsizei)ARG(5), (GLsizei)ARG(6), (GLsizei)ARG(7), ARG(8), (GLsizei)ARG(9), uptr(ARG(10))); GL_CHECK("glCompressedTexSubImage3D"); RETV(); }
IMP_AS(gl_glReadPixels, "glReadPixels") { GL_TRACE("glReadPixels"); glReadPixels((GLint)ARG(0), (GLint)ARG(1), (GLsizei)ARG(2), (GLsizei)ARG(3), ARG(4), ARG(5), pptr(ARG(6))); GL_CHECK("glReadPixels"); RETV(); }
ALIAS(gl_glReadPixels, "_glReadPixels")
IMP_AS(gl_glGetTexImage, "glGetTexImage") { GL_TRACE("glGetTexImage"); glGetTexImage(ARG(0), (GLint)ARG(1), ARG(2), ARG(3), pptr(ARG(4))); GL_CHECK("glGetTexImage"); RETV(); }
ALIAS(gl_glGetTexImage, "_glGetTexImage")
IMP_AS(gl_glGetCompressedTexImage, "glGetCompressedTexImage") { GL_TRACE("glGetCompressedTexImage"); glGetCompressedTexImage(ARG(0), (GLint)ARG(1), pptr(ARG(2))); GL_CHECK("glGetCompressedTexImage"); RETV(); }

/* ---- shaders: arrays of guest string pointers ---- */
static const GLchar **strings(uint32_t arr, uint32_t n)
{
    const GLchar **s = calloc(n + 1, sizeof *s);
    for (uint32_t k = 0; k < n; k++) s[k] = gl_ptr(rt_r32(G_MEM, arr + 4 * k));
    return s;
}
IMP_AS(gl_glShaderSource, "glShaderSource")
{
    const GLchar **s = strings(ARG(2), ARG(1));
    const char *dump = getenv("HW_DUMP_SHADERS");             /* a folder: every shader source as the game sends it */
    if (dump) {
        static int n; char p[1024]; snprintf(p, sizeof p, "%s/shader_%04d.glsl", dump, n++);
        FILE *f = fopen(p, "w");
        if (f) { for (uint32_t k = 0; k < ARG(1); k++) { int32_t len = ARG(3) ? (int32_t)rt_r32(G_MEM, ARG(3) + 4 * k) : -1;
                    if (len < 0) fputs(s[k], f); else fwrite(s[k], 1, (size_t)len, f); } fclose(f); }
    }
    glShaderSource(ARG(0), (GLsizei)ARG(1), s, gl_ptr(ARG(3))); free(s); RETV();
}
IMP_AS(gl_glShaderSourceARB, "glShaderSourceARB")
{
    const GLcharARB **s = (const GLcharARB **)strings(ARG(2), ARG(1));
    glShaderSourceARB((GLhandleARB)(uintptr_t)ARG(0), (GLsizei)ARG(1), s, gl_ptr(ARG(3))); free(s); RETV();
}
IMP_AS(gl_glTransformFeedbackVaryings, "glTransformFeedbackVaryings")
{ const GLchar **s = strings(ARG(2), ARG(1)); glTransformFeedbackVaryings(ARG(0), (GLsizei)ARG(1), s, ARG(3)); free(s); RETV(); }
IMP_AS(gl_glGetUniformIndices, "glGetUniformIndices")
{ const GLchar **s = strings(ARG(2), ARG(1)); glGetUniformIndices(ARG(0), (GLsizei)ARG(1), s, gl_ptr(ARG(3))); free(s); RETV(); }

/* ---- sync objects: small handles ---- */
static GLsync syncs[1024];
IMP_AS(gl_glFenceSync, "glFenceSync")
{
    GLsync s = glFenceSync(ARG(0), ARG(1));
    for (uint32_t k = 1; k < 1024; k++) if (!syncs[k]) { syncs[k] = s; RET(k); }
    glDeleteSync(s); RET(0);
}
static GLsync S_(uint32_t h) { return h && h < 1024 ? syncs[h] : 0; }
IMP_AS(gl_glClientWaitSync, "glClientWaitSync") { GL_TRACE("glClientWaitSync"); RET(glClientWaitSync(S_(ARG(0)), ARG(1), (GLuint64)ARG64(2))); }
IMP_AS(gl_glWaitSync, "glWaitSync") { GL_TRACE("glWaitSync"); glWaitSync(S_(ARG(0)), ARG(1), (GLuint64)ARG64(2)); GL_CHECK("glWaitSync"); RETV(); }
IMP_AS(gl_glDeleteSync, "glDeleteSync") { GL_TRACE("glDeleteSync"); uint32_t h = ARG(0); if (S_(h)) { glDeleteSync(syncs[h]); syncs[h] = 0; } GL_CHECK("glDeleteSync"); RETV(); }
IMP_AS(gl_glIsSync, "glIsSync") { GL_TRACE("glIsSync"); RET(S_(ARG(0)) && glIsSync(S_(ARG(0)))); }
IMP_AS(gl_glGetSynciv, "glGetSynciv") { GL_TRACE("glGetSynciv"); glGetSynciv(S_(ARG(0)), ARG(1), (GLsizei)ARG(2), gl_ptr(ARG(3)), gl_ptr(ARG(4))); GL_CHECK("glGetSynciv"); RETV(); }

/* ---- not supported: they stop with their name ---- */
IMP_AS(gl_glGetPointerv, "glGetPointerv") { GL_TRACE("glGetPointerv"); rt_unhandled(c, rt_r32(G_MEM, c->esp), "glGetPointerv"); }
IMP_AS(gl_glDebugMessageCallback, "glDebugMessageCallback") { GL_TRACE("glDebugMessageCallback"); RETV(); }      /* Apple's OpenGL has no debug output */
ALIAS(gl_glDebugMessageCallback, "glDebugMessageCallbackARB")

/* ---- CGL (renderer queries in Aspyr's display code) ---- */
static CGLRendererInfoObj rinfo[16];
IMP(_CGLQueryRendererInfo)       /* (GLuint display_mask, CGLRendererInfoObj *rend, GLint *nrend) */
{
    CGLRendererInfoObj r; GLint n = 0;
    CGLError e = CGLQueryRendererInfo(ARG(0), &r, &n);
    if (e) RET(e);
    uint32_t k = 1; while (k < 16 && rinfo[k]) k++;
    if (k == 16) { CGLDestroyRendererInfo(r); RET(kCGLBadAlloc); }
    rinfo[k] = r; rt_w32(G_MEM, ARG(1), k); rt_w32(G_MEM, ARG(2), (uint32_t)n); RET(0);
}
IMP(_CGLDescribeRenderer)        /* (rend, GLint rend_num, CGLRendererProperty prop, GLint *value) */
{
    uint32_t k = ARG(0); if (!k || k >= 16 || !rinfo[k]) RET(kCGLBadRendererInfo);
    GLint v = 0; CGLError e = CGLDescribeRenderer(rinfo[k], (GLint)ARG(1), ARG(2), &v);
    if (ARG(2) == kCGLRPVideoMemoryMegabytes || ARG(2) == kCGLRPTextureMemoryMegabytes) { if (v > 2048) v = 2048; }
    if (ARG(2) == kCGLRPVideoMemory || ARG(2) == kCGLRPTextureMemory) { if ((unsigned)v > 0x7fffffffu || v < 0) v = 0x7fffffff; }
    rt_w32(G_MEM, ARG(3), (uint32_t)v); RET(e);
}
IMP(_CGLDestroyRendererInfo) { uint32_t k = ARG(0); if (k && k < 16 && rinfo[k]) { CGLDestroyRendererInfo(rinfo[k]); rinfo[k] = 0; } RET(0); }
/* CGL contexts: small handles for the host contexts the guest has seen */
static CGLContextObj cgl[16];
static uint32_t cgl_handle(CGLContextObj x)
{
    if (!x) return 0;
    for (uint32_t k = 1; k < 16; k++) if (cgl[k] == x) return k;
    for (uint32_t k = 1; k < 16; k++) if (!cgl[k]) { cgl[k] = x; return k; }
    return 0;
}
static CGLContextObj CGL_(uint32_t h) { return h && h < 16 ? cgl[h] : 0; }
IMP(_CGLGetCurrentContext) { if (gl_metal) RET(15); RET(cgl_handle(CGLGetCurrentContext())); }
IMP(_CGLSetParameter) { if (gl_metal) RET(0); CGLContextObj x = CGL_(ARG(0)); RET(x ? CGLSetParameter(x, ARG(1), GP(ARG(2))) : kCGLBadContext); }
IMP(_CGLGetParameter) { if (gl_metal && ARG(0) == 15) { if (ARG(2)) rt_w32(G_MEM, ARG(2), ARG(1) == kCGLCPCurrentRendererID ? mgl_renderer_id() : 0); RET(0); } CGLContextObj x = CGL_(ARG(0)); RET(x ? CGLGetParameter(x, ARG(1), GP(ARG(2))) : kCGLBadContext); }
IMP(_CGLEnable) { RET(0); }
IMP(_CGLDisable) { RET(0); }
/* ASL::SDL::GetCGLContext(void *sdl_gl_context): the CGL context behind it (the current one) */
OVR(_ZN3ASL3SDL13GetCGLContextEPv) { if (gl_metal) RET(15); RET(cgl_handle(CGLGetCurrentContext())); }
