/* C++ exceptions through the recompiled code (Itanium ABI, as Apple's i386 toolchain emits it).
 *
 * The guest stack is real (calls push guest return addresses), so a throw walks guest frames with the image's compact
 * unwind table (every function with unwind info is an EBP frame) and reads each function's LSDA (__gcc_except_tab)
 * for its call sites, landing pads and catch types. Two phases, as libunwind does: search for a matching handler
 * first (none: std::terminate), then walk again entering every cleanup landing pad on the way, and finally the
 * handler. lift.py registered each function's landing pads at its entry (RtSehPad), so rt_seh_resume can continue in
 * a frame that is still on the host stack: the registers are set as the unwinder computed them, eax = the exception
 * and edx = the selector, as the landing pad expects. A cleanup ends in _Unwind_Resume, which continues phase 2 from
 * that frame.
 *
 * Exception object: __cxa_allocate_exception reserves a 128-byte header before the object: [0] type_info, [4]
 * destructor, [8] handler count, [12] next caught exception, [16] rethrown flag, [20] handler frame (guest esp),
 * [24] handler landing pad, [28] handler selector; the _Unwind_Exception the landing pads see is at object - 32. */
#include "mac.h"
#include "macho_tab.h"

#define HDR 128
#define UEX 32                                         /* _Unwind_Exception lies this far before the object */
uint32_t cxx_ti_exception(void);
int cxx_is_base(uint32_t thrown, uint32_t catch_ti, uint32_t obj, uint32_t *adjusted);

typedef struct Frame { uint32_t pc, esp, ebp, ebx, esi, edi; } Frame;
static const struct mo_unwind *find_unwind(uint32_t pc)
{
    unsigned lo = 0, hi = mo_unwind_n;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (mo_unwind[m].func <= pc) lo = m + 1; else hi = m; }
    return lo ? &mo_unwind[lo - 1] : 0;
}
/* step to the caller: EBP frames (UNWIND_X86_MODE_EBP_FRAME); saved registers below ebp */
static int step(Frame *f, uint32_t enc)
{
    uint32_t mode = (enc >> 24) & 0xf;
    if (mode != 1 && mode != 0) return 0;              /* frameless and DWARF encodings do not occur in this image */
    if (mode == 1) {
        uint32_t regs = enc & 0x7fff, off = (enc >> 16) & 0xff, loc = f->ebp - 4 * off;
        for (int k = 0; k < 5; k++, loc += 4) {
            switch ((regs >> (3 * k)) & 7) {
            case 1: f->ebx = rt_r32(G_MEM, loc); break;
            case 4: f->edi = rt_r32(G_MEM, loc); break;
            case 5: f->esi = rt_r32(G_MEM, loc); break;
            }
        }
    }
    if (!f->ebp) return 0;
    f->esp = f->ebp + 8; f->pc = rt_r32(G_MEM, f->ebp + 4); f->ebp = rt_r32(G_MEM, f->ebp);
    return f->pc != 0;
}
static uint32_t uleb_(uint32_t *p) { uint32_t v = 0, s = 0; uint8_t b; do { b = G_MEM[(*p)++]; v |= (uint32_t)(b & 0x7f) << s; s += 7; } while (b & 0x80); return v; }
static int32_t sleb_(uint32_t *p) { int32_t v = 0; uint32_t s = 0; uint8_t b; do { b = G_MEM[(*p)++]; v |= (int32_t)(b & 0x7f) << s; s += 7; } while (b & 0x80); if (s < 32 && (b & 0x40)) v |= -(1 << s); return v; }
static uint32_t enc_size(uint8_t e) { switch (e & 0x0f) { case 2: case 0x0a: return 2; case 0: case 3: case 0x0b: return 4; case 4: case 0x0c: return 8; } return 4; }
static uint32_t read_ptr(uint32_t *p, uint8_t e)
{
    if (e == 0xff) return 0;
    uint32_t at = *p, v;
    switch (e & 0x0f) {
    case 0x01: v = uleb_(p); break;
    case 0x09: v = (uint32_t)sleb_(p); break;
    case 0x02: v = rt_r16(G_MEM, *p); *p += 2; break;
    case 0x0a: v = (uint32_t)(int32_t)(int16_t)rt_r16(G_MEM, *p); *p += 2; break;
    default: v = rt_r32(G_MEM, *p); *p += (e & 0x0f) == 4 || (e & 0x0f) == 0x0c ? 8 : 4; break;
    }
    if ((e & 0x70) == 0x10) v += at;                   /* pcrel */
    if (e & 0x80) v = rt_r32(G_MEM, v);                 /* indirect */
    return v;
}
/* Look up pc's call site. Returns 0: no call site (terminate), 1: found (lp may be 0); *sel: 0 = cleanup only or no
 * match; a positive type filter when a catch clause matches (phase 1 sets *adj to the adjusted object). */
static int call_site(const struct mo_unwind *u, uint32_t pc, uint32_t obj, uint32_t ti, uint32_t *lp, int32_t *sel, int *cleanup, uint32_t *adj)
{
    *lp = 0; *sel = 0; *cleanup = 0;
    uint32_t p = u->lsda, func = u->func;
    uint8_t lpenc = G_MEM[p++]; uint32_t lpstart = lpenc == 0xff ? func : read_ptr(&p, lpenc);
    uint8_t tenc = G_MEM[p++]; uint32_t classinfo = 0;
    if (tenc != 0xff) { uint32_t off = uleb_(&p); classinfo = p + off; }
    uint8_t csenc = G_MEM[p++]; uint32_t cslen = uleb_(&p), cs = p, end = p + cslen, actions = end;
    while (p < end) {
        uint32_t st = read_ptr(&p, csenc & 0x0f), ln = read_ptr(&p, csenc & 0x0f), pad = read_ptr(&p, csenc & 0x0f), act = uleb_(&p);
        if (pc < func + st) break;                     /* the table is sorted */
        if (pc >= func + st + ln) continue;
        if (!pad) return 1;
        *lp = lpstart + pad;
        if (!act) { *cleanup = 1; return 1; }
        uint32_t a = actions + act - 1;
        for (;;) {
            uint32_t here = a; int32_t filter = sleb_(&a); uint32_t ap = a; int32_t next = sleb_(&a);
            if (filter == 0) *cleanup = 1;
            else if (filter > 0 && !*sel) {
                uint32_t tp = classinfo - (uint32_t)filter * enc_size(tenc), cti = read_ptr(&tp, tenc);
                if (!cti) { *sel = filter; *adj = obj; }                  /* catch (...) */
                else if (ti && cxx_is_base(ti, cti, obj, adj)) *sel = filter;
            }
            (void)here;
            if (!next) break;
            a = ap + (uint32_t)next;
        }
        return 1;
    }
    (void)cs;
    return 0;
}

static __thread uint32_t caught;                        /* stack of caught exceptions (header addresses) */
static void terminate(Ctx *c, uint32_t obj, uint32_t ti, const char *why)
{
    static __thread char m[300];
    snprintf(m, sizeof m, "C++ exception %s not handled (%s)", ti ? GS(rt_r32(G_MEM, ti + 4)) : "?", why);
    (void)obj; rt_unhandled(c, rt_r32(G_MEM, c->esp), m);
}
/* phase 2 from frame f: enter the next cleanup landing pad, or the handler at handler_esp */
static void phase2(Ctx *c, uint32_t obj, Frame f, int first)
{
    uint32_t h = obj - HDR, ti = rt_r32(G_MEM, h), hesp = rt_r32(G_MEM, h + 20), hlp = rt_r32(G_MEM, h + 24);
    int32_t hsel = (int32_t)rt_r32(G_MEM, h + 28);
    for (int depth = 0; depth < 4096; depth++) {
        if (!first || depth) {
            const struct mo_unwind *u = find_unwind(f.pc - 1);
            if (f.esp == hesp) {                         /* the handler's frame */
                c->esp = f.esp; c->ebp = f.ebp; c->ebx = f.ebx; c->esi = f.esi; c->edi = f.edi;
                c->eax = obj - UEX; c->edx = (uint32_t)hsel;
                rt_seh_resume(c, hlp);
            }
            if (u && u->lsda) {
                uint32_t lp, adj; int32_t sel; int cl;
                if (call_site(u, f.pc - 1, obj, ti, &lp, &sel, &cl, &adj) && lp && cl) {
                    c->esp = f.esp; c->ebp = f.ebp; c->ebx = f.ebx; c->esi = f.esi; c->edi = f.edi;
                    c->eax = obj - UEX; c->edx = 0;
                    rt_seh_resume(c, lp);
                }
            }
        }
        const struct mo_unwind *u = find_unwind(f.pc - 1);
        if (!step(&f, u ? u->encoding : 0x01000000u)) break;
    }
    terminate(c, obj, ti, "lost the handler's frame");
}
static void raise_(Ctx *c, uint32_t obj, Frame f0)
{
    uint32_t h = obj - HDR, ti = rt_r32(G_MEM, h); Frame f = f0;
    for (int depth = 0; depth < 4096; depth++) {           /* phase 1: find the handler */
        const struct mo_unwind *u = find_unwind(f.pc - 1);
        if (u && u->lsda) {
            uint32_t lp, adj = obj; int32_t sel; int cl;
            if (!call_site(u, f.pc - 1, obj, ti, &lp, &sel, &cl, &adj)) terminate(c, obj, ti, "no call site entry");
            if (lp && sel > 0) {
                rt_w32(G_MEM, h + 20, f.esp); rt_w32(G_MEM, h + 24, lp); rt_w32(G_MEM, h + 28, (uint32_t)sel);
                rt_w32(G_MEM, h + 32, adj);
                if (getenv("HW_LOG_EH")) hw_log("exception %s: handler at %08x (frame %08x)", ti ? GS(rt_r32(G_MEM, ti + 4)) : "?", lp, f.esp);
                phase2(c, obj, f0, 0);
            }
        }
        if (!step(&f, u ? u->encoding : 0x01000000u)) break;
    }
    terminate(c, obj, ti, "no handler");
}
/* called from __cxa_throw (cxx.c): the guest's return address is at c->esp */
void cxx_throw(Ctx *c, uint32_t obj, uint32_t ti, uint32_t dtor)
{
    uint32_t h = obj - HDR;
    rt_w32(G_MEM, h, ti); rt_w32(G_MEM, h + 4, dtor); rt_w32(G_MEM, h + 8, 0); rt_w32(G_MEM, h + 16, 0);
    Frame f = {rt_r32(G_MEM, c->esp), c->esp + 4, c->ebp, c->ebx, c->esi, c->edi};
    raise_(c, obj, f);
}
IMP(__Unwind_Resume)                                    /* the end of a cleanup: continue phase 2 from this frame */
{
    uint32_t obj = ARG(0) + UEX;
    Frame f = {rt_r32(G_MEM, c->esp), c->esp + 4, c->ebp, c->ebx, c->esi, c->edi};
    phase2(c, obj, f, 0);
}
IMP(___cxa_begin_catch)
{
    uint32_t ue = ARG(0); if (!ue) RET(0);
    uint32_t obj = ue + UEX, h = obj - HDR;
    rt_w32(G_MEM, h + 8, rt_r32(G_MEM, h + 8) + 1);
    if (rt_r32(G_MEM, h + 12) == 0 && caught != h) { rt_w32(G_MEM, h + 12, caught); caught = h; }
    uint32_t adj = rt_r32(G_MEM, h + 32); RET(adj ? adj : obj);
}
IMP(___cxa_end_catch)
{
    uint32_t h = caught; if (!h) RETV();
    uint32_t n = rt_r32(G_MEM, h + 8) - 1; rt_w32(G_MEM, h + 8, n);
    if (n == 0) {
        caught = rt_r32(G_MEM, h + 12); rt_w32(G_MEM, h + 12, 0);
        if (!rt_r32(G_MEM, h + 16)) {                   /* not rethrown: destroy and free */
            uint32_t obj = h + HDR, dtor = rt_r32(G_MEM, h + 4);
            if (dtor) g_call(c, dtor, 1, &obj);
            g_free(h);
        } else rt_w32(G_MEM, h + 16, 0);
    }
    RETV();
}
IMP(___cxa_rethrow)
{
    uint32_t h = caught; if (!h) rt_unhandled(c, rt_r32(G_MEM, c->esp), "rethrow with no exception being handled");
    rt_w32(G_MEM, h + 16, 1);
    rt_w32(G_MEM, h + 8, rt_r32(G_MEM, h + 8) - 1);       /* this handler is done with it */
    if (rt_r32(G_MEM, h + 8) == 0) { caught = rt_r32(G_MEM, h + 12); rt_w32(G_MEM, h + 12, 0); }
    Frame f = {rt_r32(G_MEM, c->esp), c->esp + 4, c->ebp, c->ebx, c->esi, c->edi};
    raise_(c, h + HDR, f);
}
IMP(___cxa_allocate_exception) { uint32_t n = ARG(0), h = g_calloc(n + HDR); RET(h + HDR); }
IMP(___cxa_free_exception) { g_free(ARG(0) - HDR); RETV(); }
IMP(___cxa_call_unexpected) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "std::unexpected"); }
IMP(___gxx_personality_v0) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "personality routine called directly"); }
IMP(__ZSt17current_exceptionv) { rt_w32(G_MEM, ARG(0), caught ? caught + HDR : 0); RET_SRET(); }
IMP(__ZSt17rethrow_exceptionSt13exception_ptr) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "std::rethrow_exception"); }
IMP(___cxa_throw)
{
    uint32_t obj = ARG(0), ti = ARG(1);
    if (getenv("HW_LOG_EH")) hw_log("C++ exception thrown: %s (from %08x)", ti ? GS(rt_r32(G_MEM, ti + 4)) : "?", rt_r32(G_MEM, c->esp));
    cxx_throw(c, obj, ti, ARG(2));
}
