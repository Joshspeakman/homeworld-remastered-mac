/* Mach-O image loader, import dispatch and guest threads for the recompiled i386 executable.
 *
 * The image's segments are copied from the user's executable into guest memory at their link addresses. Every import
 * pointer slot gets a thunk address (RT_IMPORT_BASE + 16 * index), so a call through the slot, or through a copy of
 * it in a register or a vtable, reaches rt_import(index), which runs the native implementation of that symbol
 * (tools/gen_mac_imptab.py collects IMP(...) definitions). Data imports (stdio streams, CFString class, typeinfo
 * vtables...) get runtime-owned guest objects. Weak definitions the executable wins were resolved at build time
 * (macho_tab.c fixups). */
#include "mac.h"
#include "macho_tab.h"
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <setjmp.h>

#define RT_IMPORT_BASE 0xfff00000u
#define DATA_LO        0xfe000000u                    /* runtime-owned guest data objects */
#define DATA_HI        0xffe00000u

struct rt_imp { uint32_t iat; const char *name; };
extern const struct rt_imp rt_imptab[]; extern const unsigned rt_imptab_n;
struct mac_impl { const char *name; void (*fn)(Ctx *); };
extern const struct mac_impl mac_impls[]; extern const unsigned mac_nimpls;

int hw_trace, hw_stub;
uint32_t hw_image_lo = 0xffffffffu, hw_image_hi;
char hw_game_dir[1024];

void hw_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fputs("hw: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* ---- thunks: index -> symbol -> implementation ---- */
#define MAX_THUNKS 8192
static const char *thunk_name[MAX_THUNKS];            /* symbol without the library ("_malloc") */
static void (*thunk_fn[MAX_THUNKS])(Ctx *);
static unsigned nthunks;
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

static const struct mac_impl *find_impl(const char *sym)
{
    unsigned lo = 0, hi = mac_nimpls;                  /* the table is sorted by name */
    while (lo < hi) {
        unsigned m = (lo + hi) / 2; int r = strcmp(mac_impls[m].name, sym);
        if (!r) return &mac_impls[m];
        if (r < 0) lo = m + 1; else hi = m;
    }
    return 0;
}
static const char *bare(const char *name) { const char *b = strchr(name, '!'); return b ? b + 1 : name; }

static uint32_t thunk_of_index(unsigned k) { return RT_IMPORT_BASE + 16u * k; }
uint32_t g_thunk(const char *symbol)
{
    symbol = bare(symbol);
    pthread_mutex_lock(&thunk_lock);
    for (unsigned k = 0; k < nthunks; k++)
        if (thunk_name[k] && !strcmp(thunk_name[k], symbol)) { pthread_mutex_unlock(&thunk_lock); return thunk_of_index(k); }
    if (nthunks >= MAX_THUNKS) { hw_log("too many import thunks"); abort(); }
    unsigned k = nthunks++;
    thunk_name[k] = strdup(symbol);
    const struct mac_impl *im = find_impl(symbol); thunk_fn[k] = im ? im->fn : 0;
    pthread_mutex_unlock(&thunk_lock);
    return thunk_of_index(k);
}

void rt_import(Ctx *c, uint32_t index)
{
    if (index >= nthunks) rt_unhandled(c, rt_r32(G_MEM, c->esp), "call to an unknown import thunk");
    void (*fn)(Ctx *) = thunk_fn[index];
    if (hw_trace)
        fprintf(stderr, "hw: [%08x] %s(%x, %x, %x, %x)\n", rt_r32(G_MEM, c->esp), thunk_name[index],
                rt_r32(G_MEM, c->esp + 4), rt_r32(G_MEM, c->esp + 8), rt_r32(G_MEM, c->esp + 12), rt_r32(G_MEM, c->esp + 16));
    if (!fn) {
        static __thread char buf[300];
        snprintf(buf, sizeof buf, "unimplemented import %s", thunk_name[index]);
        if (hw_stub || strchr(thunk_name[index], '#')) {   /* Steam interface methods: offline answers are 0 */
            static char seen[MAX_THUNKS];
            if (!seen[index]) { seen[index] = 1; hw_log("%s (called from %08x): returning 0", buf, rt_r32(G_MEM, c->esp)); }
            c->eax = 0; c->edx = 0; c->esp += 4; return;
        }
        rt_unhandled(c, rt_r32(G_MEM, c->esp), buf);
    }
    fn(c);
}

/* ---- runtime-owned data objects ---- */
static uint32_t data_top = DATA_LO;
static struct { const char *name; uint32_t addr; } dobj[4096]; static int ndobj;
static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t g_data(const char *name, uint32_t size)
{
    name = bare(name);
    pthread_mutex_lock(&data_lock);
    for (int k = 0; k < ndobj; k++) if (!strcmp(dobj[k].name, name)) { pthread_mutex_unlock(&data_lock); return dobj[k].addr; }
    uint32_t a = data_top; data_top += (size + 15) & ~15u;
    if (data_top > DATA_HI || ndobj == 4096) { hw_log("runtime data area full"); abort(); }
    dobj[ndobj].name = strdup(name); dobj[ndobj].addr = a; ndobj++;
    pthread_mutex_unlock(&data_lock);
    return a;
}

/* Data imports are provided by the modules that own them (libsystem.c, cxx.c, cf.c, ...): each returns the guest
 * address for a symbol it knows, or 0. */
uint32_t sys_data_import(const char *sym), cxx_data_import(const char *sym), cf_data_import(const char *sym);
__attribute__((weak)) uint32_t sys_data_import(const char *sym) { (void)sym; return 0; }
__attribute__((weak)) uint32_t cxx_data_import(const char *sym) { (void)sym; return 0; }
__attribute__((weak)) uint32_t cf_data_import(const char *sym) { (void)sym; return 0; }
static uint32_t resolve(const char *name, int is_lazy)
{
    const char *sym = bare(name);
    if (is_lazy || find_impl(sym)) return g_thunk(sym);
    uint32_t a = sys_data_import(sym);
    if (!a) a = cxx_data_import(sym);
    if (!a) a = cf_data_import(sym);
    if (a) return a;
    /* not known as data: C++ functions referenced by address (destructors passed to __cxa_throw, vtable slots)
     * become thunks that fault with their name if called; anything else gets a zeroed object */
    if (!strncmp(sym, "__Z", 3) && strncmp(sym, "__ZTV", 5) && strncmp(sym, "__ZTI", 5) && strncmp(sym, "__ZTS", 5))
        return g_thunk(sym);
    if (getenv("HW_LOG_DATA")) hw_log("data import %s: zeroed placeholder", name);
    return g_data(sym, 256);
}

/* ---- thread-local variables (__thread_vars descriptors: thunk, key, offset) ---- */
static uint32_t tlv_lo, tlv_size; static uint32_t tlv_tmpl_lo, tlv_tmpl_n;   /* template: initialised bytes */
static pthread_key_t tlv_key;
IMP(__tlv_bootstrap)                                   /* eax = descriptor; returns the variable's address in eax */
{
    uint32_t desc = c->eax, blk = (uint32_t)(uintptr_t)pthread_getspecific(tlv_key);
    if (!blk) {
        blk = g_calloc(tlv_size ? tlv_size : 16);
        if (tlv_tmpl_n) memcpy(G_MEM + blk, G_MEM + tlv_tmpl_lo, tlv_tmpl_n);
        pthread_setspecific(tlv_key, (void *)(uintptr_t)blk);
    }
    c->eax = blk + rt_r32(G_MEM, desc + 8);
    c->esp += 4;
}

/* ---- guest contexts and calls into guest code ---- */
static pthread_key_t ctx_key;
/* Guest stacks come from the guest region (a bump allocator); a thread's stack goes back to a free list when the
 * thread ends (the key destructor below) and the next thread of that size reuses it, so threads that come and go
 * (loading, audio streams) do not use up the guest's 4 GB. */
typedef struct CtxBox { Ctx c; uint32_t lo, size; } CtxBox;   /* c first: a Ctx * is its box */
static struct { uint32_t lo, size; } free_stacks[64]; static int nfree_stacks;
static pthread_mutex_t stacks_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t new_stack(uint32_t size)               /* guest stack: its lowest address */
{
    pthread_mutex_lock(&stacks_lock);
    for (int k = 0; k < nfree_stacks; k++) if (free_stacks[k].size == size) {
        uint32_t lo = free_stacks[k].lo; free_stacks[k] = free_stacks[--nfree_stacks];
        pthread_mutex_unlock(&stacks_lock); return lo;
    }
    pthread_mutex_unlock(&stacks_lock);
    uint32_t g_region(uint32_t); uint32_t lo = g_region(size + 0x10000);
    if (!lo) { hw_log("out of guest memory for a stack"); abort(); }
    return lo;
}
static void ctx_release(void *p)                       /* a thread with a guest context ended */
{
    CtxBox *b = p;
    pthread_mutex_lock(&stacks_lock);
    if (nfree_stacks < 64) { free_stacks[nfree_stacks].lo = b->lo; free_stacks[nfree_stacks].size = b->size; nfree_stacks++; }
    pthread_mutex_unlock(&stacks_lock);
    free(b);
}
Ctx *mac_ctx(void)
{
    Ctx *c = pthread_getspecific(ctx_key);
    if (!c) {
        CtxBox *b = calloc(1, sizeof *b); c = &b->c;
        b->size = pthread_main_np() ? (16u << 20) : (2u << 20); b->lo = new_stack(b->size);
        c->esp = (b->lo + b->size + 0x10000 - 64) & ~15u;
        c->fcw = 0x37f; c->mxcsr = 0x1f80;
        pthread_setspecific(ctx_key, c);
    }
    return c;
}
uint32_t g_call(Ctx *c, uint32_t fn, int nargs, const uint32_t *args)
{
    Ctx save = *c;
    uint32_t sp = (c->esp - 4u * (uint32_t)nargs - 64) & ~15u;   /* arguments 16-byte aligned, as at a call site */
    for (int k = 0; k < nargs; k++) rt_w32(G_MEM, sp + 4u * k, args[k]);
    sp -= 4; rt_w32(G_MEM, sp, 0);                                /* return address: lifted code returns in C */
    c->esp = sp; c->f.df = 0;
    rt_call(c, fn);
    uint32_t r = c->eax, rd = c->edx; double st0 = c->st[c->top]; uint32_t top = c->top;
    *c = save; c->eax = r; c->edx = rd;
    if (top != save.top) { c->st[c->top] = st0; (void)st0; }     /* (float results: callers read st via g_call_d) */
    return r;
}

/* ---- loading ---- */
static void map_image(const char *path)
{
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { hw_log("cannot open %s", path); exit(1); }
    uint8_t *file = mmap(0, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (file == MAP_FAILED) { hw_log("cannot map %s", path); exit(1); }
    for (unsigned k = 0; k < mo_segs_n; k++) {
        const struct mo_seg *s = &mo_segs[k];
        if (!strcmp(s->name, "__PAGEZERO") || !strcmp(s->name, "__LINKEDIT") || !s->vsize) continue;
        if (s->fileoff + s->filesize > (uint64_t)st.st_size) { hw_log("%s: segment %s beyond the file", path, s->name); exit(1); }
        memcpy(G_MEM + s->va, file + s->fileoff, s->filesize);
        if (s->va < hw_image_lo) hw_image_lo = s->va;
        if (s->va + s->vsize > hw_image_hi) hw_image_hi = s->va + s->vsize;
    }
    munmap(file, (size_t)st.st_size);
}

extern const struct rt_imp rt_imptab[];
static void link_image(void)
{
    /* the thunk indices of the image's own import slots are their table order (lift.py imp_index) */
    for (unsigned k = 0; k < rt_imptab_n; k++) {
        const char *sym = bare(rt_imptab[k].name);
        thunk_name[k] = sym; const struct mac_impl *im = find_impl(sym); thunk_fn[k] = im ? im->fn : 0;
    }
    nthunks = rt_imptab_n;
    for (unsigned k = 0; k < mo_fixups_n; k++) rt_w32(G_MEM, mo_fixups[k].va, mo_fixups[k].value);
    for (unsigned k = 0; k < rt_imptab_n; k++) {
        uint32_t slot = rt_imptab[k].iat;
        /* lazy slots (__la_symbol_ptr, called through stubs) are always functions; others may be data */
        extern int hw_slot_is_lazy(uint32_t va);
        uint32_t v = hw_slot_is_lazy(slot) ? thunk_of_index(k) : resolve(rt_imptab[k].name, 0);
        if (v >= RT_IMPORT_BASE && hw_slot_is_lazy(slot)) v = thunk_of_index(k);
        rt_w32(G_MEM, slot, v);
    }
    for (unsigned k = 0; k < mo_binds_n; k++)
        rt_w32(G_MEM, mo_binds[k].va, resolve(mo_binds[k].name, 0) + (uint32_t)mo_binds[k].addend);
}

/* section bounds the loader needs, from the segment table: lazy pointers lie in __DATA between the non-lazy pointers
 * and the static constructors. lift.py records them in macho_tab (mo_lazy). */
extern const uint32_t mo_lazy[2];
int hw_slot_is_lazy(uint32_t va) { return va - mo_lazy[0] < mo_lazy[1]; }
extern const uint32_t mo_tdata[4];                     /* thread-local template: data va, size; bss va, size */

void hw_load(const char *exe)
{
    map_image(exe);
    link_image();
    extern void objc_fix_class_refs(void); objc_fix_class_refs();
    pthread_key_create(&tlv_key, 0);
    if (mo_tlv[1]) {
        uint32_t lo = mo_tdata[0] ? mo_tdata[0] : mo_tdata[2];
        uint32_t hi = mo_tdata[2] ? mo_tdata[2] + mo_tdata[3] : mo_tdata[0] + mo_tdata[1];
        tlv_lo = lo; tlv_size = hi - lo; tlv_tmpl_lo = mo_tdata[0]; tlv_tmpl_n = mo_tdata[1];
        (void)tlv_lo;
    }
    for (const char *p = getenv("HW_PEEK"); p && *p; ) {     /* HW_PEEK=addr,addr: words after linking (debugging) */
        char *e; uint32_t a = (uint32_t)strtoul(p, &e, 16);
        hw_log("peek %08x = %08x -> %08x", a, rt_r32(G_MEM, a), rt_r32(G_MEM, rt_r32(G_MEM, a)));
        p = *e ? e + 1 : e;
    }
    hw_log("image %08x-%08x: %u imports, %u fixups, %u binds, %u static constructors", hw_image_lo, hw_image_hi,
           rt_imptab_n, mo_fixups_n, mo_binds_n, mo_init_n);
}

/* argv/envp/apple in guest memory, then the static constructors and main(argc, argv, envp, apple) */
int hw_run(int argc, char **argv)
{
    pthread_key_create(&ctx_key, ctx_release);
    Ctx *c = mac_ctx();
    uint32_t gargv = g_calloc(4u * (uint32_t)(argc + 1));
    for (int k = 0; k < argc; k++) rt_w32(G_MEM, gargv + 4u * k, g_strdup(argv[k]));
    extern char **environ; int ne = 0; while (environ[ne]) ne++;
    uint32_t genv = g_calloc(4u * (uint32_t)(ne + 1));
    for (int k = 0; k < ne; k++) rt_w32(G_MEM, genv + 4u * k, g_strdup(environ[k]));
    uint32_t gapple = g_calloc(8); rt_w32(G_MEM, gapple, g_strdup(argv[0]));
    extern uint32_t hw_nxargc, hw_nxargv, hw_environ;   /* libsystem.c: _NXArgc, _NXArgv, _environ */
    hw_nxargc = (uint32_t)argc; hw_nxargv = gargv; hw_environ = genv;
    uint32_t args[4] = {(uint32_t)argc, gargv, genv, gapple};
    for (unsigned k = 0; k < mo_init_n; k++) {
        if (getenv("HW_LOG_INIT")) hw_log("static constructor %u/%u at %08x", k + 1, mo_init_n, mo_init[k]);
        g_call(c, mo_init[k], 4, args);
    }
    hw_log("entering main at %08x", mo_entry);
    uint32_t r = g_call(c, mo_entry, 4, args);
    extern unsigned long long hw_ms(void); hw_log("main returned %d at %llu ms", (int)r, hw_ms());
    extern void sys_exit_handlers(void); sys_exit_handlers();
    hw_log("exit handlers done at %llu ms", hw_ms());
    return (int)r;
}

/* ---- function replacements: lift.py puts `if (rt_override(c, addr)) return;` at the entry of every candidate
 * (override_tab.c); OVR(symbol) implementations take the call, everything else runs as recompiled ---- */
struct mo_ovr { uint32_t addr; const char *name; };
extern const struct mo_ovr mo_overrides[]; extern const unsigned mo_overrides_n;
static void (**ovr_fn)(Ctx *); static pthread_once_t ovr_once = PTHREAD_ONCE_INIT;
static void ovr_init(void)
{
    ovr_fn = calloc(mo_overrides_n + 1, sizeof *ovr_fn); unsigned n = 0;
    char key[512];
    for (unsigned k = 0; k < mo_overrides_n; k++) {
        snprintf(key, sizeof key, "=%s", mo_overrides[k].name + 1);    /* OVR(SDL_Init) names "_SDL_Init" */
        const struct mac_impl *im = find_impl(key);
        if (im) { ovr_fn[k] = im->fn; n++; }
    }
    if (getenv("HW_LOG_INIT")) hw_log("%u of %u override points replaced natively", n, mo_overrides_n);
}
int rt_override(Ctx *c, uint32_t addr)
{
    pthread_once(&ovr_once, ovr_init);
    unsigned lo = 0, hi = mo_overrides_n;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (mo_overrides[m].addr < addr) lo = m + 1; else hi = m; }
    if (lo == mo_overrides_n || mo_overrides[lo].addr != addr || !ovr_fn[lo]) return 0;
    if (hw_trace) fprintf(stderr, "hw: [%08x] =%s(%x, %x, %x)\n", rt_r32(G_MEM, c->esp), mo_overrides[lo].name, rt_r32(G_MEM, c->esp + 4), rt_r32(G_MEM, c->esp + 8), rt_r32(G_MEM, c->esp + 12));
    ovr_fn[lo](c);
    return 1;
}

/* ---- for the platform layer ---- */
int hw_test_mode;                                      /* HW_TEST=1: background window, no focus/capture/fullscreen */
int hw_has_impl(const char *sym) { return find_impl(sym) != 0; }
static uint64_t frames;
unsigned long long hw_frames(void) { return frames; }
#include <time.h>
static uint64_t t0;
unsigned long long hw_ms(void) { uint64_t n = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000000; if (!t0) t0 = n; return n - t0; }
void hw_frame_done(void)
{
    frames++; (void)hw_ms();
    static uint64_t last_ms, last_frames; uint64_t now = hw_ms();
    static uint64_t last_watch;
    if (getenv("HW_WATCH") && now - last_watch >= 2000) {    /* HW_WATCH=addr,addr: guest words every 2 s (debugging) */
        last_watch = now; char line[512]; int o = 0;
        for (const char *p = getenv("HW_WATCH"); p && *p && o < 480; ) { char *e; uint32_t a = (uint32_t)strtoul(p, &e, 16); o += snprintf(line + o, sizeof line - (size_t)o, " %08x=%08x", a, rt_r32(G_MEM, a)); p = *e ? e + 1 : e; }
        hw_log("watch at %.1f s:%s", now / 1000.0, line);
    }
    if (getenv("HW_LOG_FPS") && now - last_ms >= 2000) { hw_log("%.0f fps (frame %llu, %.1f s)", (frames - last_frames) * 1000.0 / (now - last_ms), (unsigned long long)frames, now / 1000.0); last_ms = now; last_frames = frames; }
    static uint64_t quit_ms; static int qinit; if (!qinit) { qinit = 1; const char *q = getenv("HW_QUIT_MS"); quit_ms = q ? strtoull(q, 0, 10) : 0; }
    if (quit_ms && now >= quit_ms) { hw_log("quitting after %.1f s (HW_QUIT_MS)", now / 1000.0); fflush(0); _exit(0); }
    static uint64_t quit_after; static int init;
    if (!init) { init = 1; const char *q = getenv("HW_QUIT_AFTER"); quit_after = q ? strtoull(q, 0, 10) : 0; }
    if (getenv("HW_LOG_FRAMES") && frames % 600 == 0) hw_log("frame %llu", (unsigned long long)frames);
    if (quit_after && frames >= quit_after) { hw_log("quitting after %llu frames (HW_QUIT_AFTER)", (unsigned long long)frames); fflush(0); _exit(0); }
}
