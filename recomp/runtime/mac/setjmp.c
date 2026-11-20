/* Guest setjmp/longjmp (Lua's error handling, libpng/libjpeg) over host setjmp.
 *
 * lift.py turns every call to the guest's setjmp into a host _setjmp into a pad that lives in the lifted function's
 * frame, followed by rt_setjmp_arm. Arming saves the guest's callee-saved registers and stack pointer, writes a
 * token into the guest jmp_buf and pushes the pad on this thread's stack of pads; leaving the function disarms its
 * pads. longjmp finds the pad by the token, restores the guest registers, drops the pads armed after it (their
 * frames are being discarded) and jumps back into the lifted function, where setjmp "returns" the value. */
#include "mac.h"
#include <setjmp.h>

#define TOKEN 0x424a4d50u                              /* 'PMJB' */
typedef struct Armed { RtJmpPad *pad; uint32_t serial, esp, ebp, ebx, esi, edi; } Armed;
static __thread Armed armed[256]; static __thread int narmed;
static uint32_t serial_next = 1;

void rt_setjmp_arm(Ctx *c, RtJmpPad *p)
{
    uint32_t gbuf = rt_r32(G_MEM, c->esp + 4);
    c->esp += 4;                                       /* the import's return: setjmp(env) pops its return address */
    for (int k = narmed - 1; k >= 0; k--)              /* re-armed in a loop: move it to the top */
        if (armed[k].pad == p) { memmove(&armed[k], &armed[k + 1], (size_t)(narmed - k - 1) * sizeof *armed); narmed--; break; }
    if (narmed == 256) { memmove(armed, armed + 1, 255 * sizeof *armed); narmed = 255; }
    uint32_t s = __atomic_fetch_add(&serial_next, 1, __ATOMIC_RELAXED);
    p->serial = s; p->val = 0;
    armed[narmed++] = (Armed){p, s, c->esp, c->ebp, c->ebx, c->esi, c->edi};
    rt_w32(G_MEM, gbuf, TOKEN); rt_w32(G_MEM, gbuf + 4, s);
    c->eax = 0;
}
void rt_setjmp_disarm(RtJmpPad *p, int n)
{
    for (int k = narmed - 1; k >= 0; k--)
        if (armed[k].pad >= p && armed[k].pad < p + n) { memmove(&armed[k], &armed[k + 1], (size_t)(narmed - k - 1) * sizeof *armed); narmed--; }
}
static void do_longjmp(Ctx *c, uint32_t gbuf, uint32_t val)
{
    uint32_t s = rt_r32(G_MEM, gbuf + 4);
    if (rt_r32(G_MEM, gbuf) != TOKEN) rt_unhandled(c, rt_r32(G_MEM, c->esp), "longjmp to a jmp_buf no setjmp filled");
    for (int k = narmed - 1; k >= 0; k--)
        if (armed[k].serial == s) {
            Armed a = armed[k]; narmed = k + 1;        /* pads armed after this one belong to discarded frames */
            c->esp = a.esp; c->ebp = a.ebp; c->ebx = a.ebx; c->esi = a.esi; c->edi = a.edi;
            a.pad->val = val ? val : 1;
            _longjmp(a.pad->jb, 1);
        }
    rt_unhandled(c, rt_r32(G_MEM, c->esp), "longjmp to a setjmp whose function has returned");
}
IMP(_longjmp) { do_longjmp(c, ARG(0), ARG(1)); }
IMP(__longjmp) { do_longjmp(c, ARG(0), ARG(1)); }
IMP(_siglongjmp) { do_longjmp(c, ARG(0), ARG(1)); }
/* reached only if setjmp is called through a pointer (not a direct call the lifter could see) */
IMP(_setjmp) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "setjmp called indirectly"); }
IMP(__setjmp) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "setjmp called indirectly"); }
