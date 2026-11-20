/* mgl: the OpenGL 4.1 core subset Homeworld Remastered uses, implemented on Metal.
 *
 * Conventions: everything stays in OpenGL's memory layout (row 0 = the bottom of an image). Shaders are compiled with
 * Y flipped and depth mapped to [0, 1], so the GL viewport and scissor numbers apply to Metal unchanged, texture
 * uploads need no flipping, render targets sample as in GL, and glReadPixels copies rows as they are. The default
 * framebuffer is an offscreen texture; presenting draws it flipped into the window's drawable. */
#ifndef MGL_H
#define MGL_H
#include <stdint.h>
#include <SDL2/SDL.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct mgl_entry { const char *name; void *fn; } mgl_entry;
extern const mgl_entry mgl_entries[]; extern const unsigned mgl_nentries;
void *mgl_lookup(const char *name);                    /* GL entry point by name, or 0 */
void *mgl_create_context(SDL_Window *w);               /* SDL_GL_CreateContext */
void mgl_window_created(SDL_Window *w);                /* on the main thread, as each window is made: its Metal view */
int mgl_make_current(SDL_Window *w, void *ctx);
void mgl_delete_context(void *ctx);
void mgl_swap(SDL_Window *w);
void mgl_set_swap_interval(int interval);
void mgl_drawable_size(SDL_Window *w, int *dw, int *dh);
int mgl_read_back(int x, int y, int w, int h, void *rgba);   /* default framebuffer, GL row order */
uint32_t mgl_renderer_id(void);                        /* the renderer ID to report through CGL */
#ifdef __cplusplus
}
#endif
#endif
