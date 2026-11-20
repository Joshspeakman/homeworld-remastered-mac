/* pthreads for the guest. Guest mutexes, condition variables and once flags are the game's own structures (i386
 * sizes: mutex 44, cond 28, once 8, attr 12/16 bytes); each maps to a host object found by its guest address, created
 * on first use (statically initialised ones have only the signature). Guest threads are host threads, each with its
 * own guest context and stack. */
#include "mac.h"
#include <errno.h>
#include <setjmp.h>
#include <time.h>
#include <sys/time.h>

/* ---- objects by guest address ---- */
#define NOBJ_BITS 16
#define NOBJ (1u << NOBJ_BITS)
/* Open addressing by guest address. A destroyed object leaves a tombstone (its kind negated) so probe chains stay
 * intact; a new object takes the first tombstone on its chain. (Without that, objects made and destroyed at ever
 * new heap addresses, such as std::mutex and std::promise states, filled the table with tombstones and the game
 * stopped after some minutes of play.) */
typedef struct Obj { uint32_t addr; int kind; void *h; } Obj;
static Obj objs[NOBJ]; static unsigned nlive;
static pthread_mutex_t obj_lock = PTHREAD_MUTEX_INITIALIZER;
enum { K_MUTEX = 1, K_COND = 2 };
static unsigned hash(uint32_t a) { return (a * 2654435761u) >> (32 - NOBJ_BITS); }
static void *obj_get(uint32_t addr, int kind, int recursive)
{
    unsigned k = hash(addr) & (NOBJ - 1); int tomb = -1, empty = -1;
    pthread_mutex_lock(&obj_lock);
    for (unsigned n = 0; n < NOBJ; n++, k = (k + 1) & (NOBJ - 1)) {
        if (objs[k].addr == addr && objs[k].kind == kind) { void *h = objs[k].h; pthread_mutex_unlock(&obj_lock); return h; }
        if (!objs[k].addr) { empty = (int)k; break; }
        if (objs[k].kind < 0 && tomb < 0) tomb = (int)k;
    }
    if (tomb >= 0) k = (unsigned)tomb; else if (empty >= 0) k = (unsigned)empty;
    else { pthread_mutex_unlock(&obj_lock); hw_log("too many guest synchronisation objects (%u live)", nlive); abort(); }
    void *h;
    if (kind == K_MUTEX) {
        pthread_mutex_t *m = malloc(sizeof *m); pthread_mutexattr_t at; pthread_mutexattr_init(&at);
        if (recursive) pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &at); pthread_mutexattr_destroy(&at); h = m;
    } else { pthread_cond_t *cv = malloc(sizeof *cv); pthread_cond_init(cv, 0); h = cv; }
    objs[k].addr = addr; objs[k].kind = kind; objs[k].h = h;
    static unsigned created; created++;
    static int every = -1; if (every < 0) every = getenv("HW_LOG_SYNC") ? 1024 : 8192;
    if (++nlive % (unsigned)every == 0 || (every == 1024 && created % 4096 == 0)) hw_log("%u guest synchronisation objects live (%u made)", nlive, created);
    pthread_mutex_unlock(&obj_lock); return h;
}
static void obj_drop(uint32_t addr, int kind)
{
    unsigned k = hash(addr) & (NOBJ - 1);
    pthread_mutex_lock(&obj_lock);
    for (unsigned n = 0; n < NOBJ && objs[k].addr; n++, k = (k + 1) & (NOBJ - 1))
        if (objs[k].addr == addr && objs[k].kind == kind) {
            /* keep the slot (probe chains stay intact) but give it a fresh object on the next init */
            if (kind == K_MUTEX) { pthread_mutex_destroy(objs[k].h); } else pthread_cond_destroy(objs[k].h);
            free(objs[k].h); objs[k].kind = -kind; objs[k].h = 0; nlive--; break;
        }
    pthread_mutex_unlock(&obj_lock);
}
/* guest mutex layout: [0] signature, [4] type (written by our pthread_mutex_init from the attribute) */
#define MUTEX_SIG_INIT 0x32aaaba7u                    /* PTHREAD_MUTEX_INITIALIZER */
#define MUTEX_SIG_RECURSIVE_INIT 0x32aaaba2u           /* PTHREAD_RECURSIVE_MUTEX_INITIALIZER */
pthread_mutex_t *gmutex(uint32_t a)
{
    uint32_t sig = rt_r32(G_MEM, a);
    int rec = sig == MUTEX_SIG_RECURSIVE_INIT || (sig == 0x4d555458u && rt_r32(G_MEM, a + 4) == PTHREAD_MUTEX_RECURSIVE);
    return obj_get(a, K_MUTEX, rec);
}
IMP(_pthread_mutexattr_init) { rt_w32(G_MEM, ARG(0), 0x4d545841u); rt_w32(G_MEM, ARG(0) + 4, PTHREAD_MUTEX_DEFAULT); RET(0); }
IMP_AS(pthread_mutexattr_destroy_unix, "_pthread_mutexattr_destroy$UNIX2003") { RET(0); }
IMP(_pthread_mutexattr_settype) { rt_w32(G_MEM, ARG(0) + 4, ARG(1)); RET(0); }
IMP(_pthread_mutex_init)
{
    uint32_t m = ARG(0), at = ARG(1);
    obj_drop(m, K_MUTEX);
    rt_w32(G_MEM, m, 0x4d555458u); rt_w32(G_MEM, m + 4, at ? rt_r32(G_MEM, at + 4) : PTHREAD_MUTEX_DEFAULT);
    gmutex(m); RET(0);
}
IMP(_pthread_mutex_destroy) { obj_drop(ARG(0), K_MUTEX); RET(0); }
IMP(_pthread_mutex_lock)    { RET(pthread_mutex_lock(gmutex(ARG(0)))); }
IMP(_pthread_mutex_trylock) { RET(pthread_mutex_trylock(gmutex(ARG(0)))); }
IMP(_pthread_mutex_unlock)  { RET(pthread_mutex_unlock(gmutex(ARG(0)))); }
IMP_AS(pthread_cond_init_unix, "_pthread_cond_init$UNIX2003") { obj_drop(ARG(0), K_COND); obj_get(ARG(0), K_COND, 0); RET(0); }
IMP(_pthread_cond_destroy)   { obj_drop(ARG(0), K_COND); RET(0); }
IMP(_pthread_cond_signal)    { RET(pthread_cond_signal(obj_get(ARG(0), K_COND, 0))); }
IMP(_pthread_cond_broadcast) { RET(pthread_cond_broadcast(obj_get(ARG(0), K_COND, 0))); }
IMP_AS(pthread_cond_wait_unix, "_pthread_cond_wait$UNIX2003") { RET(pthread_cond_wait(obj_get(ARG(0), K_COND, 0), gmutex(ARG(1)))); }
IMP_AS(pthread_cond_timedwait_unix, "_pthread_cond_timedwait$UNIX2003")
{
    struct timespec ts = {(int32_t)rt_r32(G_MEM, ARG(2)), (int32_t)rt_r32(G_MEM, ARG(2) + 4)};
    RET(pthread_cond_timedwait(obj_get(ARG(0), K_COND, 0), gmutex(ARG(1)), &ts));
}

/* pthread_once_t: [0] signature, [4] state: 0 not run, 1 running, 2 done */
static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t once_cv = PTHREAD_COND_INITIALIZER;
IMP(_pthread_once)
{
    uint32_t o = ARG(0), fn = ARG(1);
    pthread_mutex_lock(&once_lock);
    while (rt_r32(G_MEM, o + 4) == 1) pthread_cond_wait(&once_cv, &once_lock);
    if (rt_r32(G_MEM, o + 4) == 2) { pthread_mutex_unlock(&once_lock); RET(0); }
    rt_w32(G_MEM, o + 4, 1); pthread_mutex_unlock(&once_lock);
    g_call(c, fn, 0, 0);
    pthread_mutex_lock(&once_lock); rt_w32(G_MEM, o + 4, 2); pthread_cond_broadcast(&once_cv); pthread_mutex_unlock(&once_lock);
    RET(0);
}

/* thread-specific data: host keys holding guest values; guest destructors are not run (threads end with the game) */
IMP(_pthread_key_create) { pthread_key_t k; int r = pthread_key_create(&k, 0); rt_w32(G_MEM, ARG(0), (uint32_t)k); RET(r); }
IMP(_pthread_getspecific) { RET((uint32_t)(uintptr_t)pthread_getspecific((pthread_key_t)ARG(0))); }
IMP(_pthread_setspecific) { RET(pthread_setspecific((pthread_key_t)ARG(0), (void *)(uintptr_t)ARG(1))); }

/* ---- threads: guest pthread_t = index into this table ---- */
#define NTH 256
typedef struct GThread { pthread_t h; uint32_t start, arg, result; int used, detached; jmp_buf exit_jb; char name[64]; } GThread;
static GThread th[NTH]; static pthread_mutex_t th_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int my_index = -1;
static int self_index(void)
{
    if (my_index >= 0) return my_index;
    pthread_mutex_lock(&th_lock);
    int k = 1; while (k < NTH && th[k].used) k++;
    if (k == NTH) { pthread_mutex_unlock(&th_lock); return 0; }
    th[k].used = 1; th[k].h = pthread_self(); my_index = k;
    pthread_mutex_unlock(&th_lock); return k;
}
static void *thread_main(void *p)
{
    int k = (int)(intptr_t)p; my_index = k;
    Ctx *c = mac_ctx();
    if (!setjmp(th[k].exit_jb)) th[k].result = g_call(c, th[k].start, 1, &th[k].arg);
    if (th[k].detached) { pthread_mutex_lock(&th_lock); th[k].used = 0; pthread_mutex_unlock(&th_lock); }   /* nobody joins it: the slot is free */
    return 0;
}
IMP(_pthread_attr_init) { memset(GP(ARG(0)), 0, 40); RET(0); }
IMP(_pthread_attr_destroy) { RET(0); }
IMP(_pthread_attr_setdetachstate) { rt_w32(G_MEM, ARG(0) + 8, ARG(1)); RET(0); }
IMP(_pthread_attr_setstacksize) { rt_w32(G_MEM, ARG(0) + 12, ARG(1)); RET(0); }
IMP(_pthread_create)
{
    uint32_t out = ARG(0), attr = ARG(1);
    pthread_mutex_lock(&th_lock);
    int k = 1; while (k < NTH && th[k].used) k++;
    if (k == NTH) { pthread_mutex_unlock(&th_lock); RET(EAGAIN); }
    memset(&th[k], 0, sizeof th[k]); th[k].used = 1; th[k].start = ARG(2); th[k].arg = ARG(3);
    th[k].detached = attr && rt_r32(G_MEM, attr + 8) == PTHREAD_CREATE_DETACHED;
    pthread_mutex_unlock(&th_lock);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8u << 20);   /* host stack: lifted code is deep */
    if (th[k].detached) pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&th[k].h, &at, thread_main, (void *)(intptr_t)k);
    pthread_attr_destroy(&at);
    if (r) { th[k].used = 0; RET(r); }
    if (out) rt_w32(G_MEM, out, (uint32_t)k);
    RET(0);
}
IMP_AS(pthread_join_unix, "_pthread_join$UNIX2003")
{
    int k = (int)ARG(0); if (k <= 0 || k >= NTH || !th[k].used) RET(ESRCH);
    int r = pthread_join(th[k].h, 0);
    if (ARG(1)) rt_w32(G_MEM, ARG(1), th[k].result);
    th[k].used = 0; RET(r);
}
IMP(_pthread_exit)
{
    int k = self_index();
    th[k].result = ARG(0);
    longjmp(th[k].exit_jb, 1);
}
IMP(_pthread_self) { RET(self_index()); }
IMP(_pthread_equal) { RET(ARG(0) == ARG(1)); }
IMP(_pthread_main_np) { RET(pthread_main_np()); }
IMP_AS(pthread_cancel_unix, "_pthread_cancel$UNIX2003") { RET(0); }
IMP_AS(pthread_setcanceltype_unix, "_pthread_setcanceltype$UNIX2003") { if (ARG(1)) rt_w32(G_MEM, ARG(1), 0); RET(0); }
IMP_AS(pthread_sigmask_unix, "_pthread_sigmask$UNIX2003") { RET(0); }
IMP(_pthread_setname_np) { int k = self_index(); strlcpy(th[k].name, GS(ARG(0)), sizeof th[k].name); pthread_setname_np(th[k].name); RET(0); }
IMP(_pthread_getname_np) { int k = (int)ARG(0); if (k < 0 || k >= NTH) RET(ESRCH); strlcpy(GP(ARG(1)), th[k].name, ARG(2)); RET(0); }

pthread_cond_t *gcond(uint32_t a) { return obj_get(a, K_COND, 0); }
void gsync_drop(uint32_t a) { obj_drop(a, K_MUTEX); obj_drop(a, K_COND); }
