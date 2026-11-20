/* Native runtime for recompiled i386 Mach-O code (Aspyr's Homeworld Remastered): shared definitions.
 *
 * Guest ABI (Darwin i386): cdecl; the caller pops the arguments; eax (and edx for 64-bit) or st(0) for float/double
 * results; a struct is returned through a hidden first argument that the CALLEE pops (`ret $4`). The stack is 16-byte
 * aligned at every call site, so esp % 16 == 12 at a function's first instruction; code keeps SSE values in aligned
 * stack slots (movaps), so everything here that calls guest code keeps that alignment.
 *
 * An import implementation is `IMP(symbol)` (symbol = the Mach-O name, with its leading underscore) or
 * `IMP_AS(cname, "symbol$VARIANT")` for names that are not C identifiers. tools/gen_mac_imptab.py collects them. */
#ifndef HW_MAC_H
#define HW_MAC_H
#include "rt.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define IMP(sym)            void imp_##sym(Ctx *c)
#define IMP_AS(cname, str)  void imp_##cname(Ctx *c)
#define ALIAS(cname, name)                              /* registers a second name for imp_##cname (gen_mac_imptab.py) */
#define OVR(sym)            void ovr_##sym(Ctx *c)      /* replaces the image's own function `sym` at its entry */
#define ARG(n)      rt_r32(G_MEM, c->esp + 4 + 4 * (n))
#define ARGF(n)     rt_rf32(G_MEM, c->esp + 4 + 4 * (n))
#define ARGD(n)     rt_rf64(G_MEM, c->esp + 4 + 4 * (n))        /* a double occupies two argument slots */
#define ARG64(n)    rt_r64(G_MEM, c->esp + 4 + 4 * (n))
#define GP(a)       ((void *)(G_MEM + (uint32_t)(a)))
#define GS(a)       ((const char *)(G_MEM + (uint32_t)(a)))       /* guest C string (NULL guest pointer: "") */
#define RET(v)      do { c->eax = (uint32_t)(v); c->esp += 4; return; } while (0)
#define RET64(v)    do { uint64_t _v = (uint64_t)(v); c->eax = (uint32_t)_v; c->edx = (uint32_t)(_v >> 32); c->esp += 4; return; } while (0)
#define RETD(v)     do { double _d = (double)(v); c->top = (c->top - 1) & 7; c->st[c->top] = _d; c->esp += 4; return; } while (0)
#define RET_SRET()  do { c->eax = ARG(0); c->esp += 8; return; } while (0)   /* struct result: callee pops the pointer */
#define RETV()      do { c->esp += 4; return; } while (0)

/* guest memory */
uint32_t g_malloc(uint32_t n);
uint32_t g_calloc(uint32_t n);
void     g_free(uint32_t p);
uint32_t g_realloc(uint32_t p, uint32_t n);
uint32_t g_size(uint32_t p);
uint32_t g_strdup(const char *s);
uint32_t g_data(const char *name, uint32_t size);   /* a runtime-owned guest data object (zeroed), stable per name */
uint32_t g_thunk(const char *symbol);               /* guest address that dispatches to the import `symbol` */

/* calling guest code from the host (callbacks, thread start, static constructors) */
uint32_t g_call(Ctx *c, uint32_t fn, int nargs, const uint32_t *args);
Ctx *mac_ctx(void);                                 /* this thread's guest context (created on first use) */

/* runtime switches */
extern int hw_trace;                                /* HW_TRACE=1: log every import call */
extern int hw_stub;                                 /* HW_STUB=1: unimplemented imports return 0 instead of aborting */
void hw_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* guest image */
extern uint32_t hw_image_lo, hw_image_hi;           /* mapped Mach-O segments */
extern char hw_game_dir[1024];                      /* .../HomeworldRemastered.app/Contents (data root) */
/* host objects behind guest pthread mutexes / condition variables (threads.c) */
pthread_mutex_t *gmutex(uint32_t guest_mutex);
pthread_cond_t *gcond(uint32_t guest_cond);
void gsync_drop(uint32_t guest_addr);
#endif
