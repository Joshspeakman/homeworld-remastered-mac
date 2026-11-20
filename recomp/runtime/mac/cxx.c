/* libc++ / libc++abi for the guest, in the 32-bit (i386) libc++ ABI v1 layouts the game was compiled against. The game
 * inlines most of the library from its headers; these are the out-of-line pieces it imports.
 *
 * std::basic_string (12 bytes, little-endian default layout):
 *   long  form: [0] capacity+1 rounded, with bit 0 set (allocated size | 1), [4] size, [8] data pointer
 *   short form: [0] size << 1 (bit 0 clear), characters from offset 1 (char) or 4 (wchar_t); room for 10 chars / 1
 *   wchar_t plus the terminator.
 * Member functions take `this` as the first stack argument (cdecl); a class result is returned through a hidden
 * pointer the callee pops (RET_SRET). */
#include "mac.h"
#include <time.h>
#include <errno.h>
#include <sys/time.h>

/* ---- strings ---- */
typedef struct { int w; uint32_t min_cap, sdata; } SK;           /* kind: unit size, short capacity (with NUL), short data offset */
static const SK SC = {1, 11, 1}, SW = {4, 2, 4};
static uint32_t sget(const SK *k, uint32_t p) { return k->w == 1 ? G_MEM[p] : rt_r32(G_MEM, p); }
static void sput(const SK *k, uint32_t p, uint32_t v) { if (k->w == 1) G_MEM[p] = (uint8_t)v; else rt_w32(G_MEM, p, v); }
static int s_long(uint32_t s) { return G_MEM[s] & 1; }
static uint32_t s_size(const SK *k, uint32_t s) { (void)k; return s_long(s) ? rt_r32(G_MEM, s + 4) : (uint32_t)G_MEM[s] >> 1; }
static uint32_t s_data(const SK *k, uint32_t s) { return s_long(s) ? rt_r32(G_MEM, s + 8) : s + k->sdata; }
static uint32_t s_cap(const SK *k, uint32_t s) { return s_long(s) ? (rt_r32(G_MEM, s) & ~1u) - 1 : k->min_cap - 1; }
static void s_setsize(const SK *k, uint32_t s, uint32_t n)
{
    if (s_long(s)) rt_w32(G_MEM, s + 4, n); else G_MEM[s] = (uint8_t)(n << 1);
    sput(k, s_data(k, s) + n * (uint32_t)k->w, 0);
}
static uint32_t recommend(const SK *k, uint32_t n)           /* capacity (without NUL) for n characters */
{
    if (n < k->min_cap) return k->min_cap - 1;
    uint32_t a = k->w == 1 ? 16 : 4;
    return ((n + 1 + a - 1) & ~(a - 1)) - 1;
}
/* make room for at least `cap` characters keeping the first `keep` ones (does not change the size field) */
static void s_grow(const SK *k, uint32_t s, uint32_t cap, uint32_t keep)
{
    if (cap <= s_cap(k, s)) return;
    uint32_t nc = recommend(k, cap), old = s_data(k, s), was_long = s_long(s);
    uint32_t nd = g_malloc((nc + 1) * (uint32_t)k->w);
    memcpy(G_MEM + nd, G_MEM + old, keep * (uint32_t)k->w);
    uint32_t size = s_size(k, s);
    if (was_long) g_free(old);
    rt_w32(G_MEM, s, (nc + 1) | 1); rt_w32(G_MEM, s + 4, size); rt_w32(G_MEM, s + 8, nd);
}
static void s_init_empty(const SK *k, uint32_t s) { memset(G_MEM + s, 0, 12); (void)k; }
/* the units of a guest range, copied (sources may alias the string being changed) */
static uint8_t *units(const SK *k, uint32_t p, uint32_t n) { uint8_t *b = malloc(n * (uint32_t)k->w + 4); memcpy(b, G_MEM + p, n * (uint32_t)k->w); return b; }
static uint32_t ulen(const SK *k, uint32_t p) { uint32_t n = 0; while (sget(k, p + n * (uint32_t)k->w)) n++; return n; }
/* replace [pos, pos+n1) with n2 units from src (host copy) */
static void s_replace(const SK *k, uint32_t s, uint32_t pos, uint32_t n1, const uint8_t *src, uint32_t n2)
{
    uint32_t sz = s_size(k, s);
    if (pos > sz) pos = sz;
    if (n1 > sz - pos) n1 = sz - pos;
    uint32_t nsz = sz - n1 + n2, W = (uint32_t)k->w;
    s_grow(k, s, nsz, sz);
    uint32_t d = s_data(k, s);
    memmove(G_MEM + d + (pos + n2) * W, G_MEM + d + (pos + n1) * W, (sz - pos - n1) * W);
    if (n2) memcpy(G_MEM + d + pos * W, src, n2 * W);
    s_setsize(k, s, nsz);
}
static void s_assign(const SK *k, uint32_t s, const uint8_t *src, uint32_t n) { s_replace(k, s, 0, s_size(k, s), src, n); }
static void s_fill(const SK *k, uint8_t *b, uint32_t n, uint32_t ch) { for (uint32_t i = 0; i < n; i++) { if (k->w == 1) b[i] = (uint8_t)ch; else memcpy(b + 4 * i, &ch, 4); } }
static void s_init_units(const SK *k, uint32_t s, uint32_t p, uint32_t n, uint32_t reserve)
{
    s_init_empty(k, s); uint8_t *b = units(k, p, n);
    s_grow(k, s, reserve > n ? reserve : n, 0); s_setsize(k, s, 0); s_assign(k, s, b, n); free(b);
}
static void s_copy_ctor(const SK *k, uint32_t s, uint32_t o) { s_init_units(k, s, s_data(k, o), s_size(k, o), 0); }
static void s_dtor(const SK *k, uint32_t s) { (void)k; if (s_long(s)) g_free(rt_r32(G_MEM, s + 8)); }
static void s_set(const SK *k, uint32_t s, uint32_t o)
{
    if (s == o) return;
    uint8_t *b = units(k, s_data(k, o), s_size(k, o)); s_assign(k, s, b, s_size(k, o)); free(b);
}
static int s_compare(const SK *k, uint32_t s, uint32_t p)
{
    uint32_t n = s_size(k, s), m = ulen(k, p), d = s_data(k, s), l = n < m ? n : m;
    for (uint32_t i = 0; i < l; i++) {
        uint32_t a = sget(k, d + i * (uint32_t)k->w), b = sget(k, p + i * (uint32_t)k->w);
        if (a != b) return k->w == 1 ? ((uint8_t)a < (uint8_t)b ? -1 : 1) : ((int32_t)a < (int32_t)b ? -1 : 1);
    }
    return n < m ? -1 : n > m;
}
#define STR_IMPLS(K, P)                                                                                               \
IMP(P##6__initEPK##K##m)  { s_init_units(&S_, ARG(0), ARG(1), ARG(2), 0); RETV(); }                                   \
IMP(P##6__initEPK##K##mm) { s_init_units(&S_, ARG(0), ARG(1), ARG(2), ARG(3)); RETV(); }                              \
IMP(P##6__initEm##K)      { uint32_t n = ARG(1); uint8_t *b = malloc(n * 4 + 4); s_fill(&S_, b, n, ARG(2)); s_init_empty(&S_, ARG(0)); s_assign(&S_, ARG(0), b, n); free(b); RETV(); } \
IMP(P##6appendEPK##K)     { uint32_t n = ulen(&S_, ARG(1)); uint8_t *b = units(&S_, ARG(1), n); s_replace(&S_, ARG(0), s_size(&S_, ARG(0)), 0, b, n); free(b); RET(ARG(0)); } \
IMP(P##6assignEPK##K)     { uint32_t n = ulen(&S_, ARG(1)); uint8_t *b = units(&S_, ARG(1), n); s_assign(&S_, ARG(0), b, n); free(b); RET(ARG(0)); } \
IMP(P##6insertEmPK##K)    { uint32_t n = ulen(&S_, ARG(2)); uint8_t *b = units(&S_, ARG(2), n); s_replace(&S_, ARG(0), ARG(1), 0, b, n); free(b); RET(ARG(0)); } \
IMP(P##6insertEmm##K)     { uint32_t n = ARG(2); uint8_t *b = malloc(n * 4 + 4); s_fill(&S_, b, n, ARG(3)); s_replace(&S_, ARG(0), ARG(1), 0, b, n); free(b); RET(ARG(0)); } \
IMP(P##5eraseEmm)         { uint32_t s = ARG(0), pos = ARG(1), n = ARG(2), sz = s_size(&S_, s); if (pos > sz) pos = sz; if (n > sz - pos) n = sz - pos; s_replace(&S_, s, pos, n, 0, 0); RET(s); } \
IMP(P##6resizeEm##K)      { uint32_t s = ARG(0), n = ARG(1), sz = s_size(&S_, s); if (n <= sz) s_setsize(&S_, s, n); else { uint8_t *b = malloc((n - sz) * 4 + 4); s_fill(&S_, b, n - sz, ARG(2)); s_replace(&S_, s, sz, 0, b, n - sz); free(b); } RETV(); } \
IMP(P##7reserveEm)        { uint32_t s = ARG(0); s_grow(&S_, s, ARG(1), s_size(&S_, s)); s_setsize(&S_, s, s_size(&S_, s)); RETV(); } \
IMP(P##9push_backE##K)    { uint32_t ch = ARG(1); uint8_t b[4]; memcpy(b, &ch, 4); s_replace(&S_, ARG(0), s_size(&S_, ARG(0)), 0, b, 1); RETV(); } \
IMP(P##C1ERKS5_)          { s_copy_ctor(&S_, ARG(0), ARG(1)); RETV(); }                                                \
IMP(P##D1Ev)              { s_dtor(&S_, ARG(0)); RETV(); }                                                             \
IMP(P##aSERKS5_)          { s_set(&S_, ARG(0), ARG(1)); RET(ARG(0)); }                                                \
IMP(P##9__grow_byEmmmmmm)                                                                                              \
{   /* (old_cap, delta_cap, old_sz, n_copy, n_del, n_add): reallocate; keep [0,n_copy) and the tail after n_del at  \
       n_copy + n_add; the caller writes the new characters and the size */                                           \
    uint32_t s = ARG(0), oc = ARG(1), dc = ARG(2), osz = ARG(3), ncp = ARG(4), ndel = ARG(5), nadd = ARG(6), W = (uint32_t)S_.w; \
    uint32_t old = s_data(&S_, s), want = oc + dc; if (want < 2 * oc) want = 2 * oc;                                  \
    uint32_t nc = recommend(&S_, want), nd = g_malloc((nc + 1) * W);                                                   \
    memcpy(G_MEM + nd, G_MEM + old, ncp * W);                                                                          \
    uint32_t tail = osz - ndel - ncp; memcpy(G_MEM + nd + (ncp + nadd) * W, G_MEM + old + (ncp + ndel) * W, tail * W); \
    if (s_long(s)) g_free(old);                                                                                        \
    rt_w32(G_MEM, s, (nc + 1) | 1); rt_w32(G_MEM, s + 8, nd); RETV();                                                  \
}
#define S_ SC
STR_IMPLS(c, __ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE)
IMP(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm) { uint8_t *b = units(&SC, ARG(1), ARG(2)); s_replace(&SC, ARG(0), s_size(&SC, ARG(0)), 0, b, ARG(2)); free(b); RET(ARG(0)); }
IMP(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKcm) { uint8_t *b = units(&SC, ARG(1), ARG(2)); s_assign(&SC, ARG(0), b, ARG(2)); free(b); RET(ARG(0)); }
IMP(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7replaceEmmPKc) { uint32_t n = ulen(&SC, ARG(3)); uint8_t *b = units(&SC, ARG(3), n); s_replace(&SC, ARG(0), ARG(1), ARG(2), b, n); free(b); RET(ARG(0)); }
IMP(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7replaceEmmPKcm) { uint8_t *b = units(&SC, ARG(3), ARG(4)); s_replace(&SC, ARG(0), ARG(1), ARG(2), b, ARG(4)); free(b); RET(ARG(0)); }
IMP(__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_mmRKS4_)
{
    uint32_t s = ARG(0), o = ARG(1), pos = ARG(2), n = ARG(3), sz = s_size(&SC, o);
    if (pos > sz) pos = sz;
    if (n > sz - pos) n = sz - pos;
    s_init_units(&SC, s, s_data(&SC, o) + pos, n, 0); RETV();
}
IMP(__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4findEcm)
{
    uint32_t s = ARG(0), sz = s_size(&SC, s), d = s_data(&SC, s); uint8_t ch = (uint8_t)ARG(1);
    for (uint32_t i = ARG(2); i < sz; i++) if (G_MEM[d + i] == ch) RET(i);
    RET(0xffffffffu);
}
IMP(__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5rfindEcm)
{
    uint32_t s = ARG(0), sz = s_size(&SC, s), d = s_data(&SC, s), pos = ARG(2); uint8_t ch = (uint8_t)ARG(1);
    if (!sz) RET(0xffffffffu);
    if (pos >= sz) pos = sz - 1;
    for (uint32_t i = pos + 1; i-- > 0; ) if (G_MEM[d + i] == ch) RET(i);
    RET(0xffffffffu);
}
IMP(__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEPKc) { RET(s_compare(&SC, ARG(0), ARG(1))); }
#undef S_
#define S_ SW
STR_IMPLS(w, __ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE)
IMP(__ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE6appendEPKwm) { uint8_t *b = units(&SW, ARG(1), ARG(2)); s_replace(&SW, ARG(0), s_size(&SW, ARG(0)), 0, b, ARG(2)); free(b); RET(ARG(0)); }
IMP(__ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE6insertEmPKwm) { uint8_t *b = units(&SW, ARG(2), ARG(3)); s_replace(&SW, ARG(0), ARG(1), 0, b, ARG(3)); free(b); RET(ARG(0)); }
IMP(__ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE7replaceEmmmw) { uint32_t n = ARG(3); uint8_t *b = malloc(n * 4 + 4); s_fill(&SW, b, n, ARG(4)); s_replace(&SW, ARG(0), ARG(1), ARG(2), b, n); free(b); RET(ARG(0)); }
IMP(__ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEEaSEw) { uint32_t ch = ARG(1); uint8_t b[4]; memcpy(b, &ch, 4); s_assign(&SW, ARG(0), b, 1); RET(ARG(0)); }
IMP(__ZNKSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEE7compareEPKw) { RET(s_compare(&SW, ARG(0), ARG(1))); }
#undef S_
static void to_string(Ctx *c, const char *txt)
{
    uint32_t s = ARG(0), n = (uint32_t)strlen(txt), tmp = g_malloc(n + 1); memcpy(G_MEM + tmp, txt, n + 1);
    s_init_units(&SC, s, tmp, n, 0); g_free(tmp);
}
IMP(__ZNSt3__19to_stringEi) { char b[16]; snprintf(b, sizeof b, "%d", (int32_t)ARG(1)); to_string(c, b); RET_SRET(); }
IMP(__ZNSt3__19to_stringEj) { char b[16]; snprintf(b, sizeof b, "%u", ARG(1)); to_string(c, b); RET_SRET(); }
IMP(__ZNSt3__19to_stringEm) { char b[16]; snprintf(b, sizeof b, "%u", ARG(1)); to_string(c, b); RET_SRET(); }
uint32_t cxx_new_string(const char *txt)              /* a guest std::string on the heap (for other modules) */
{
    uint32_t s = g_calloc(12), n = (uint32_t)strlen(txt), tmp = g_malloc(n + 1); memcpy(G_MEM + tmp, txt, n + 1);
    s_init_units(&SC, s, tmp, n, 0); g_free(tmp); return s;
}

/* ---- operator new / delete ---- */
IMP(__Znwm) { uint32_t p = g_malloc(ARG(0)); if (!p) rt_unhandled(c, rt_r32(G_MEM, c->esp), "operator new: out of guest memory"); RET(p); }
IMP(__Znam) { uint32_t p = g_malloc(ARG(0)); if (!p) rt_unhandled(c, rt_r32(G_MEM, c->esp), "operator new[]: out of guest memory"); RET(p); }
IMP(__ZnwmRKSt9nothrow_t) { RET(g_malloc(ARG(0))); }
IMP(__ZnamRKSt9nothrow_t) { RET(g_malloc(ARG(0))); }
IMP(__ZdlPv) { g_free(ARG(0)); RETV(); }
IMP(__ZdaPv) { g_free(ARG(0)); RETV(); }

/* ---- RTTI objects and the standard exception classes ----
 * type_info (i386): [0] vtable (one of the three __cxxabiv1 vtables + 8), [4] name; __si_class_type_info adds [8]
 * the base's type_info; __vmi_class_type_info: [8] flags, [12] base count, then {type_info, offset_flags} pairs. */
static uint32_t vt_class, vt_si, vt_vmi;               /* the three type_info vtables (symbol address, entries at +8) */
static uint32_t ti_object(const char *sym, const char *name, uint32_t base)
{
    uint32_t t = g_data(sym, 16);
    if (!rt_r32(G_MEM, t)) {
        rt_w32(G_MEM, t, (base ? vt_si : vt_class) + 8); rt_w32(G_MEM, t + 4, g_strdup(name));
        if (base) rt_w32(G_MEM, t + 8, base);
    }
    return t;
}
static uint32_t ti_exception, ti_bad_alloc, ti_bad_exception, ti_logic, ti_runtime, ti_out_of_range, ti_invalid_argument, ti_future_error;
static uint32_t vt_out_of_range, vt_invalid_argument, vt_runtime, vt_logic, vt_bad_alloc, vt_bad_exception, vt_exception, vt_future_error, vt_assoc;
static uint32_t make_vtable(const char *sym, uint32_t ti, const char *d1, const char *d0, const char *what)
{
    uint32_t v = g_data(sym, 32);
    rt_w32(G_MEM, v + 4, ti); rt_w32(G_MEM, v + 8, g_thunk(d1)); rt_w32(G_MEM, v + 12, g_thunk(d0));
    if (what) rt_w32(G_MEM, v + 16, g_thunk(what));
    return v;
}
static void rtti_init(void)
{
    if (vt_class) return;
    vt_class = g_data("__ZTVN10__cxxabiv117__class_type_infoE", 64);
    vt_si = g_data("__ZTVN10__cxxabiv120__si_class_type_infoE", 64);
    vt_vmi = g_data("__ZTVN10__cxxabiv121__vmi_class_type_infoE", 64);
    for (uint32_t v = 0; v < 3; v++) {                 /* type_info's own virtual functions are never called by the game */
        uint32_t t = v == 0 ? vt_class : v == 1 ? vt_si : vt_vmi;
        for (uint32_t k = 2; k < 16; k++) rt_w32(G_MEM, t + 4 * k, g_thunk("__cxxabiv1 type_info virtual"));
    }
    ti_exception = ti_object("__ZTISt9exception", "St9exception", 0);
    ti_bad_alloc = ti_object("__ZTISt9bad_alloc", "St9bad_alloc", ti_exception);
    ti_bad_exception = ti_object("__ZTISt13bad_exception", "St13bad_exception", ti_exception);
    ti_logic = ti_object("__ZTISt11logic_error", "St11logic_error", ti_exception);
    ti_runtime = ti_object("__ZTISt13runtime_error", "St13runtime_error", ti_exception);
    ti_out_of_range = ti_object("__ZTISt12out_of_range", "St12out_of_range", ti_logic);
    ti_invalid_argument = ti_object("__ZTISt16invalid_argument", "St16invalid_argument", ti_logic);
    ti_future_error = ti_object("__ZTINSt3__112future_errorE", "NSt3__112future_errorE", ti_logic);
    vt_exception = make_vtable("__ZTVSt9exception", ti_exception, "__ZNSt9exceptionD2Ev", "__ZNSt9exceptionD0Ev", "__ZNKSt9exception4whatEv");
    vt_bad_alloc = make_vtable("__ZTVSt9bad_alloc", ti_bad_alloc, "__ZNSt9bad_allocD1Ev", "__ZNSt9bad_allocD0Ev", "__ZNKSt9bad_alloc4whatEv");
    vt_bad_exception = make_vtable("__ZTVSt13bad_exception", ti_bad_exception, "__ZNSt13bad_exceptionD2Ev", "__ZNSt13bad_exceptionD0Ev", "__ZNKSt13bad_exception4whatEv");
    vt_logic = make_vtable("__ZTVSt11logic_error", ti_logic, "__ZNSt11logic_errorD2Ev", "__ZNSt11logic_errorD0Ev", "__ZNKSt11logic_error4whatEv");
    vt_runtime = make_vtable("__ZTVSt13runtime_error", ti_runtime, "__ZNSt13runtime_errorD2Ev", "__ZNSt13runtime_errorD0Ev", "__ZNKSt13runtime_error4whatEv");
    vt_out_of_range = make_vtable("__ZTVSt12out_of_range", ti_out_of_range, "__ZNSt12out_of_rangeD1Ev", "__ZNSt12out_of_rangeD0Ev", "__ZNKSt11logic_error4whatEv");
    vt_invalid_argument = make_vtable("__ZTVSt16invalid_argument", ti_invalid_argument, "__ZNSt16invalid_argumentD1Ev", "__ZNSt16invalid_argumentD0Ev", "__ZNKSt11logic_error4whatEv");
    vt_future_error = make_vtable("__ZTVNSt3__112future_errorE", ti_future_error, "__ZNSt3__112future_errorD1Ev", "__ZNSt3__112future_errorD0Ev", "__ZNKSt11logic_error4whatEv");
    uint32_t ti_assoc = ti_object("__ZTINSt3__117__assoc_sub_stateE", "NSt3__117__assoc_sub_stateE", 0);
    vt_assoc = make_vtable("__ZTVNSt3__117__assoc_sub_stateE", ti_assoc, "__ZNSt3__117__assoc_sub_stateD2Ev", "__ZNSt3__117__assoc_sub_stateD0Ev", "__ZNSt3__117__assoc_sub_state16__on_zero_sharedEv");
    ti_object("__ZTINSt3__113basic_istreamIcNS_11char_traitsIcEEEE", "NSt3__113basic_istreamIcNS_11char_traitsIcEEEE", 0);
    ti_object("__ZTINSt3__113basic_ostreamIcNS_11char_traitsIcEEEE", "NSt3__113basic_ostreamIcNS_11char_traitsIcEEEE", 0);
    ti_object("__ZTINSt3__114basic_iostreamIcNS_11char_traitsIcEEEE", "NSt3__114basic_iostreamIcNS_11char_traitsIcEEEE", 0);
    ti_object("__ZTINSt3__115basic_streambufIcNS_11char_traitsIcEEEE", "NSt3__115basic_streambufIcNS_11char_traitsIcEEEE", 0);
}
uint32_t cxx_data_import(const char *sym)
{
    rtti_init();
    if (!strncmp(sym, "__ZTI", 5) || !strncmp(sym, "__ZTV", 5)) return g_data(sym, 64);   /* made above (or an empty one) */
    if (!strcmp(sym, "__ZSt7nothrow")) return g_data(sym, 4);
    if (!strcmp(sym, "__ZNSt3__15ctypeIcE2idE")) return g_data(sym, 8);
    return 0;
}
/* refcounted message (libc++ __libcpp_refstring): [len][cap][count] then the characters; the object holds the text */
static uint32_t refstring(const char *s)
{
    uint32_t n = (uint32_t)strlen(s), b = g_malloc(12 + n + 1);
    rt_w32(G_MEM, b, n); rt_w32(G_MEM, b + 4, n); rt_w32(G_MEM, b + 8, 0); memcpy(G_MEM + b + 12, s, n + 1);
    return b + 12;
}
static void refstring_copy(uint32_t dst, uint32_t src) { uint32_t p = rt_r32(G_MEM, src + 4); rt_w32(G_MEM, dst + 4, p); if (p) __atomic_add_fetch((uint32_t *)GP(p - 4), 1, __ATOMIC_SEQ_CST); }
static void refstring_release(uint32_t obj)
{
    uint32_t p = rt_r32(G_MEM, obj + 4);
    if (p && (int32_t)__atomic_sub_fetch((uint32_t *)GP(p - 4), 1, __ATOMIC_SEQ_CST) < 0) g_free(p - 12);
}
IMP(__ZNSt11logic_errorC2EPKc)   { rtti_init(); rt_w32(G_MEM, ARG(0), vt_logic + 8); rt_w32(G_MEM, ARG(0) + 4, refstring(GS(ARG(1)))); RETV(); }
IMP(__ZNSt11logic_errorC2ERKS_)  { rtti_init(); rt_w32(G_MEM, ARG(0), vt_logic + 8); refstring_copy(ARG(0), ARG(1)); RETV(); }
IMP(__ZNSt13runtime_errorC1EPKc) { rtti_init(); rt_w32(G_MEM, ARG(0), vt_runtime + 8); rt_w32(G_MEM, ARG(0) + 4, refstring(GS(ARG(1)))); RETV(); }
IMP(__ZNSt13runtime_errorC2EPKc) { rtti_init(); rt_w32(G_MEM, ARG(0), vt_runtime + 8); rt_w32(G_MEM, ARG(0) + 4, refstring(GS(ARG(1)))); RETV(); }
IMP(__ZNSt13runtime_errorC2ERKS_){ rtti_init(); rt_w32(G_MEM, ARG(0), vt_runtime + 8); refstring_copy(ARG(0), ARG(1)); RETV(); }
IMP(__ZNSt13runtime_errorD1Ev)   { refstring_release(ARG(0)); RETV(); }
IMP(__ZNSt13runtime_errorD2Ev)   { refstring_release(ARG(0)); RETV(); }
IMP(__ZNSt13runtime_errorD0Ev)   { refstring_release(ARG(0)); g_free(ARG(0)); RETV(); }
IMP(__ZNSt11logic_errorD2Ev)     { refstring_release(ARG(0)); RETV(); }
IMP(__ZNSt11logic_errorD0Ev)     { refstring_release(ARG(0)); g_free(ARG(0)); RETV(); }
IMP(__ZNSt12out_of_rangeD1Ev)    { refstring_release(ARG(0)); RETV(); }
IMP(__ZNSt12out_of_rangeD0Ev)    { refstring_release(ARG(0)); g_free(ARG(0)); RETV(); }
IMP(__ZNSt16invalid_argumentD1Ev){ refstring_release(ARG(0)); RETV(); }
IMP(__ZNSt16invalid_argumentD0Ev){ refstring_release(ARG(0)); g_free(ARG(0)); RETV(); }
IMP(__ZNKSt13runtime_error4whatEv) { RET(rt_r32(G_MEM, ARG(0) + 4)); }
IMP(__ZNKSt11logic_error4whatEv)   { RET(rt_r32(G_MEM, ARG(0) + 4)); }
static uint32_t lit(const char *s) { return g_strdup(s); }
IMP(__ZNKSt9bad_alloc4whatEv)     { static uint32_t m; if (!m) m = lit("std::bad_alloc"); RET(m); }
IMP(__ZNKSt13bad_exception4whatEv){ static uint32_t m; if (!m) m = lit("std::bad_exception"); RET(m); }
IMP(__ZNKSt9exception4whatEv)     { static uint32_t m; if (!m) m = lit("std::exception"); RET(m); }
IMP(__ZNSt9bad_allocC1Ev) { rtti_init(); rt_w32(G_MEM, ARG(0), vt_bad_alloc + 8); RETV(); }
IMP(__ZNSt9bad_allocC2Ev) { rtti_init(); rt_w32(G_MEM, ARG(0), vt_bad_alloc + 8); RETV(); }
IMP(__ZNSt9bad_allocD1Ev) { RETV(); }
IMP(__ZNSt9bad_allocD2Ev) { RETV(); }
IMP(__ZNSt9bad_allocD0Ev) { g_free(ARG(0)); RETV(); }
IMP(__ZNSt9exceptionD2Ev) { RETV(); }
IMP(__ZNSt9exceptionD0Ev) { g_free(ARG(0)); RETV(); }
IMP(__ZNSt13bad_exceptionD2Ev) { RETV(); }
IMP(__ZNSt13bad_exceptionD0Ev) { g_free(ARG(0)); RETV(); }
/* future_error(error_code{int value, const error_category *cat}): a logic_error with the category's message */
IMP(__ZNSt3__112future_errorC1ENS_10error_codeE)
{
    rtti_init(); uint32_t e = ARG(0); int v = (int32_t)ARG(1);
    static const char *msg[] = {"", "The associated promise has been destructed prior to the associated state becoming ready.",
                                "The future has already been retrieved from the promise or packaged_task.",
                                "The state of the promise has already been set.", "Operation not permitted on an object without an associated state."};
    rt_w32(G_MEM, e, vt_future_error + 8); rt_w32(G_MEM, e + 4, refstring(v >= 0 && v <= 4 ? msg[v] : "future_error"));
    rt_w32(G_MEM, e + 8, (uint32_t)v); rt_w32(G_MEM, e + 12, ARG(2)); RETV();
}
IMP(__ZNSt3__112future_errorD1Ev) { refstring_release(ARG(0)); RETV(); }
IMP(__ZNSt3__112future_errorD0Ev) { refstring_release(ARG(0)); g_free(ARG(0)); RETV(); }
IMP(__ZNSt3__115future_categoryEv) { RET(g_data("future_category object", 16)); }

/* ---- static-local guards (64-bit guard object; byte 0 = initialised) ---- */
static pthread_mutex_t guard_lock; static pthread_once_t guard_once = PTHREAD_ONCE_INIT;
static void guard_init(void) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&guard_lock, &a); }
IMP(___cxa_guard_acquire)
{
    uint32_t g = ARG(0);
    if (__atomic_load_n(&G_MEM[g], __ATOMIC_ACQUIRE)) RET(0);
    pthread_once(&guard_once, guard_init); pthread_mutex_lock(&guard_lock);
    if (G_MEM[g]) { pthread_mutex_unlock(&guard_lock); RET(0); }
    RET(1);                                            /* lock held until release / abort */
}
IMP(___cxa_guard_release) { __atomic_store_n(&G_MEM[ARG(0)], 1, __ATOMIC_RELEASE); pthread_mutex_unlock(&guard_lock); RETV(); }
IMP(___cxa_guard_abort) { pthread_mutex_unlock(&guard_lock); RETV(); }
IMP(___cxa_pure_virtual) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "pure virtual function called"); }
IMP(__ZSt9terminatev) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "std::terminate"); }

/* ---- threads: std::mutex = pthread_mutex_t, std::condition_variable = pthread_cond_t ---- */
IMP(__ZNSt3__15mutex4lockEv)   { pthread_mutex_lock(gmutex(ARG(0))); RETV(); }
IMP(__ZNSt3__15mutex6unlockEv) { pthread_mutex_unlock(gmutex(ARG(0))); RETV(); }
IMP(__ZNSt3__15mutexD1Ev)      { gsync_drop(ARG(0)); RETV(); }
IMP(__ZNSt3__118condition_variable10notify_oneEv) { pthread_cond_signal(gcond(ARG(0))); RETV(); }
IMP(__ZNSt3__118condition_variable10notify_allEv) { pthread_cond_broadcast(gcond(ARG(0))); RETV(); }
IMP(__ZNSt3__118condition_variableD1Ev) { gsync_drop(ARG(0)); RETV(); }
/* __do_timed_wait(unique_lock<mutex>& {mutex*, bool owns}, system_clock time_point in ns (64-bit by value)) */
IMP(__ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_5mutexEEENS_6chrono10time_pointINS5_12system_clockENS5_8durationIxNS_5ratioILx1ELx1000000000EEEEEEE)
{
    uint32_t cv = ARG(0), lk = ARG(1); int64_t ns = (int64_t)ARG64(2);
    struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    pthread_cond_timedwait(gcond(cv), gmutex(rt_r32(G_MEM, lk)), &ts); RETV();
}
IMP(__ZNSt3__16chrono12steady_clock3nowEv) { struct timespec t; clock_gettime(CLOCK_MONOTONIC_RAW, &t); RET64((int64_t)t.tv_sec * 1000000000 + t.tv_nsec); }
IMP(__ZNSt3__16chrono12system_clock3nowEv) { struct timeval t; gettimeofday(&t, 0); RET64((int64_t)t.tv_sec * 1000000 + t.tv_usec); }   /* microseconds */
IMP(__ZNSt3__120__throw_system_errorEiPKc) { hw_log("std::system_error(%d, %s)", (int)ARG(0), GS(ARG(1))); rt_unhandled(c, rt_r32(G_MEM, c->esp), "std::system_error thrown"); }
IMP(__ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "std::length_error (vector)"); }
IMP(__ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "std::out_of_range (vector)"); }
IMP(__ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "std::length_error (string)"); }

/* ---- shared ownership: __shared_count {vptr, long __shared_owners_} ---- */
IMP(__ZNSt3__114__shared_count12__add_sharedEv) { __atomic_add_fetch((int32_t *)GP(ARG(0) + 4), 1, __ATOMIC_RELAXED); RETV(); }
IMP(__ZNSt3__114__shared_count16__release_sharedEv)
{
    uint32_t t = ARG(0);
    if (__atomic_sub_fetch((int32_t *)GP(t + 4), 1, __ATOMIC_ACQ_REL) == -1) {
        uint32_t fn = rt_r32(G_MEM, rt_r32(G_MEM, t) + 8);   /* virtual __on_zero_shared(): third slot */
        g_call(c, fn, 1, &t);
    }
    RETV();
}
IMP(__ZNSt3__114__shared_countD2Ev) { RETV(); }

/* ---- promise<void> / future<void> over __assoc_sub_state (88 bytes):
 * [0] vptr, [4] shared owners, [8] exception_ptr, [12] mutex (44), [56] condition_variable (28), [84] state */
enum { ST_CONSTRUCTED = 1, ST_FUTURE_ATTACHED = 2, ST_READY = 4, ST_DEFERRED = 8 };
static uint32_t assoc_new(void)
{
    rtti_init(); uint32_t a = g_calloc(88);
    rt_w32(G_MEM, a, vt_assoc + 8);
    rt_w32(G_MEM, a + 12, 0x32aaaba7u);                   /* PTHREAD_MUTEX_INITIALIZER, COND initializer */
    rt_w32(G_MEM, a + 56, 0x3cb0b1bbu);
    return a;
}
static void assoc_release(Ctx *c, uint32_t a)
{
    if (a && __atomic_sub_fetch((int32_t *)GP(a + 4), 1, __ATOMIC_ACQ_REL) == -1) {
        uint32_t fn = rt_r32(G_MEM, rt_r32(G_MEM, a) + 8); g_call(c, fn, 1, &a);
    }
}
IMP(__ZNSt3__117__assoc_sub_state16__on_zero_sharedEv) { uint32_t a = ARG(0); gsync_drop(a + 12); gsync_drop(a + 56); g_free(a); RETV(); }
IMP(__ZNSt3__117__assoc_sub_stateD2Ev) { RETV(); }
IMP(__ZNSt3__117__assoc_sub_stateD0Ev) { g_free(ARG(0)); RETV(); }
static void assoc_set_ready(uint32_t a)
{
    pthread_mutex_lock(gmutex(a + 12));
    rt_w32(G_MEM, a + 84, rt_r32(G_MEM, a + 84) | ST_CONSTRUCTED | ST_READY);
    pthread_cond_broadcast(gcond(a + 56)); pthread_mutex_unlock(gmutex(a + 12));
}
static void assoc_wait(uint32_t a)
{
    pthread_mutex_lock(gmutex(a + 12));
    while (!(rt_r32(G_MEM, a + 84) & ST_READY)) pthread_cond_wait(gcond(a + 56), gmutex(a + 12));
    pthread_mutex_unlock(gmutex(a + 12));
}
IMP(__ZNSt3__17promiseIvEC1Ev) { rt_w32(G_MEM, ARG(0), assoc_new()); RETV(); }
IMP(__ZNSt3__17promiseIvED1Ev)
{
    uint32_t a = rt_r32(G_MEM, ARG(0));
    if (a && !(rt_r32(G_MEM, a + 84) & ST_READY) && rt_r32(G_MEM, a + 4) > 0) assoc_set_ready(a);   /* broken promise: wake waiters */
    assoc_release(c, a); RETV();
}
IMP(__ZNSt3__17promiseIvE10get_futureEv)
{
    uint32_t out = ARG(0), a = rt_r32(G_MEM, ARG(1));
    __atomic_add_fetch((int32_t *)GP(a + 4), 1, __ATOMIC_RELAXED);
    rt_w32(G_MEM, a + 84, rt_r32(G_MEM, a + 84) | ST_FUTURE_ATTACHED);
    rt_w32(G_MEM, out, a); RET_SRET();
}
IMP(__ZNSt3__17promiseIvE9set_valueEv) { assoc_set_ready(rt_r32(G_MEM, ARG(0))); RETV(); }
IMP(__ZNSt3__17promiseIvE13set_exceptionESt13exception_ptr) { uint32_t a = rt_r32(G_MEM, ARG(0)); rt_w32(G_MEM, a + 8, rt_r32(G_MEM, ARG(1))); assoc_set_ready(a); RETV(); }
IMP(__ZNSt3__117__assoc_sub_state13set_exceptionESt13exception_ptr) { uint32_t a = ARG(0); rt_w32(G_MEM, a + 8, rt_r32(G_MEM, ARG(1))); assoc_set_ready(a); RETV(); }
IMP(__ZNSt3__117__assoc_sub_state10__sub_waitERNS_11unique_lockINS_5mutexEEE)
{
    uint32_t a = ARG(0), lk = ARG(1);
    while (!(rt_r32(G_MEM, a + 84) & ST_READY)) pthread_cond_wait(gcond(a + 56), gmutex(rt_r32(G_MEM, lk)));
    RETV();
}
IMP(__ZNSt3__117__assoc_sub_state9__executeEv) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "deferred std::future (__execute)"); }
IMP(__ZNSt3__16futureIvE3getEv)
{
    uint32_t f = ARG(0), a = rt_r32(G_MEM, f);
    assoc_wait(a);
    uint32_t ex = rt_r32(G_MEM, a + 8);
    rt_w32(G_MEM, f, 0); assoc_release(c, a);
    if (ex) rt_unhandled(c, rt_r32(G_MEM, c->esp), "std::future::get: the task ended with an exception");
    RETV();
}
IMP(__ZNSt3__16futureIvED1Ev) { assoc_release(c, rt_r32(G_MEM, ARG(0))); RETV(); }
IMP(__ZNSt13exception_ptrC1ERKS_) { rt_w32(G_MEM, ARG(0), rt_r32(G_MEM, ARG(1))); RETV(); }
IMP(__ZNSt13exception_ptrD1Ev) { RETV(); }

/* ---- __dynamic_cast(src_ptr, src_type, dst_type, src2dst_offset) ---- */
static int ti_kind(uint32_t ti) { uint32_t v = rt_r32(G_MEM, ti) - 8; return v == vt_si ? 1 : v == vt_vmi ? 2 : 0; }
static int ti_same(uint32_t a, uint32_t b)
{ return a == b || !strcmp(GS(rt_r32(G_MEM, a + 4)), GS(rt_r32(G_MEM, b + 4))); }
/* find dst inside the object at obj of type ti (a public base, possibly virtual); returns its address or 0 */
static uint32_t find_base(uint32_t obj, uint32_t ti, uint32_t dst, int depth)
{
    if (depth > 64) return 0;
    if (ti_same(ti, dst)) return obj;
    int k = ti_kind(ti);
    if (k == 1) return find_base(obj, rt_r32(G_MEM, ti + 8), dst, depth + 1);
    if (k == 2) {
        uint32_t n = rt_r32(G_MEM, ti + 12);
        for (uint32_t i = 0; i < n && i < 256; i++) {
            uint32_t bt = rt_r32(G_MEM, ti + 16 + 8 * i); int32_t of = (int32_t)rt_r32(G_MEM, ti + 20 + 8 * i);
            if (!(of & 2)) continue;                     /* not public */
            int32_t off = of >> 8;
            uint32_t base = (of & 1) ? obj + (uint32_t)(int32_t)rt_r32(G_MEM, rt_r32(G_MEM, obj) + (uint32_t)off) : obj + (uint32_t)off;
            uint32_t r = find_base(base, bt, dst, depth + 1);
            if (r) return r;
        }
    }
    return 0;
}
IMP(___dynamic_cast)
{
    rtti_init();
    uint32_t p = ARG(0), dst = ARG(2);
    if (!p) RET(0);
    uint32_t vt = rt_r32(G_MEM, p);
    int32_t to_top = (int32_t)rt_r32(G_MEM, vt - 8);
    uint32_t whole = p + (uint32_t)to_top, wti = rt_r32(G_MEM, vt - 4);
    if (ti_same(wti, dst)) RET(whole);
    RET(find_base(whole, wti, dst, 0));
}

/* ---- exceptions: runtime/mac/unwind.c ---- */
uint32_t cxx_ti_exception(void) { rtti_init(); return ti_exception; }
int cxx_is_base(uint32_t thrown, uint32_t catch_ti, uint32_t obj, uint32_t *adjusted)
{
    rtti_init();
    if (ti_same(thrown, catch_ti)) { *adjusted = obj; return 1; }
    uint32_t r = find_base(obj, thrown, catch_ti, 0);
    if (r) { *adjusted = r; return 1; }
    return 0;
}
