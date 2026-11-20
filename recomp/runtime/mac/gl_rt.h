/* OpenGL plumbing shared by the generated wrappers (work/gl_gen.c) and runtime/mac/gl.c. */
#ifndef HW_GL_RT_H
#define HW_GL_RT_H
extern void *glp[];                                    /* the GL implementation, by gen_gl.py index */
extern const char *const gl_names[]; extern const unsigned gl_count;
extern int gl_trace;
static inline void *gl_ptr(uint32_t a) { return a ? (void *)(G_MEM + a) : 0; }
extern int gl_check; void gl_check_error(const char *name);
#define GL_CHECK(name) do { if (gl_check) gl_check_error(name); } while (0)
#define GL_TRACE(name) do { if (gl_trace) fprintf(stderr, "gl: %s\n", name); } while (0)
#endif
