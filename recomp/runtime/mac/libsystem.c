/* libSystem for the guest: C library, math, time, stdio streams, process and Mach queries. Strings and memory work
 * directly on guest memory; anything returning a pointer returns a guest address. */
#include "mac.h"
#include <math.h>
#include <ctype.h>
#include <wchar.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <locale.h>
#include <iconv.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <xlocale.h>
#include <runetype.h>

#define S(n) GS(ARG(n))
static uint32_t gstr_or0(uint32_t p) { return p; }

/* ---- data imports ---- */
uint32_t hw_nxargc, hw_nxargv, hw_environ;
static uint32_t stdfile[3];                          /* guest FILE objects: stdin, stdout, stderr */
static uint32_t rune_locale;
uint32_t sys_data_import(const char *sym)
{
    if (!strcmp(sym, "___stack_chk_guard")) { uint32_t a = g_data(sym, 4); rt_w32(G_MEM, a, 0x5a17c0deu); return a; }
    if (!strcmp(sym, "___stdinp") || !strcmp(sym, "___stdoutp") || !strcmp(sym, "___stderrp")) {
        int k = sym[5] == 'i' ? 0 : sym[5] == 'o' ? 1 : 2;
        if (!stdfile[k]) { stdfile[k] = g_data(k == 0 ? "stdin FILE" : k == 1 ? "stdout FILE" : "stderr FILE", 88); rt_w32(G_MEM, stdfile[k], 0x46494c45u); rt_w32(G_MEM, stdfile[k] + 4, (uint32_t)k); }
        uint32_t a = g_data(sym, 4); rt_w32(G_MEM, a, stdfile[k]); return a;
    }
    if (!strcmp(sym, "__DefaultRuneLocale")) {
        /* _RuneLocale (i386): magic[8], encoding[32], sgetrune, sputrune, invalid_rune, runetype[256], maplower[256],
         * mapupper[256], ...: the game's inlined ctype macros index runetype at offset 52 */
        if (!rune_locale) {
            rune_locale = g_data(sym, 52 + 3 * 1024 + 64);
            memcpy(G_MEM + rune_locale, "RuneMagA", 8); memcpy(G_MEM + rune_locale + 8, "NONE", 4);
            for (int k = 0; k < 256; k++) {
                rt_w32(G_MEM, rune_locale + 52 + 4u * (uint32_t)k, _DefaultRuneLocale.__runetype[k]);
                rt_w32(G_MEM, rune_locale + 52 + 1024 + 4u * (uint32_t)k, (uint32_t)_DefaultRuneLocale.__maplower[k]);
                rt_w32(G_MEM, rune_locale + 52 + 2048 + 4u * (uint32_t)k, (uint32_t)_DefaultRuneLocale.__mapupper[k]);
            }
        }
        return rune_locale;
    }
    if (!strcmp(sym, "_mach_task_self_")) { uint32_t a = g_data(sym, 4); rt_w32(G_MEM, a, 0x103); return a; }
    if (!strcmp(sym, "_vm_page_size")) { uint32_t a = g_data(sym, 4); rt_w32(G_MEM, a, 4096); return a; }
    if (!strcmp(sym, "_NXArgc")) return g_data(sym, 4);
    if (!strcmp(sym, "__dispatch_main_q")) return g_data(sym, 128);
    if (!strcmp(sym, "__NSConcreteStackBlock") || !strcmp(sym, "__NSConcreteGlobalBlock")) return g_data(sym, 128);
    return 0;
}

/* ---- errno: one guest int per thread ---- */
static pthread_key_t errno_key; static pthread_once_t errno_once = PTHREAD_ONCE_INIT;
static void errno_init(void) { pthread_key_create(&errno_key, 0); }
static uint32_t errno_addr(void)
{
    pthread_once(&errno_once, errno_init);
    uint32_t a = (uint32_t)(uintptr_t)pthread_getspecific(errno_key);
    if (!a) { a = g_calloc(4); pthread_setspecific(errno_key, (void *)(uintptr_t)a); }
    return a;
}
static void set_errno(int e) { rt_w32(G_MEM, errno_addr(), (uint32_t)e); }
IMP(___error) { RET(errno_addr()); }

/* ---- memory ---- */
IMP(_malloc)  { RET(g_malloc(ARG(0))); }
IMP(_calloc)  { uint64_t n = (uint64_t)ARG(0) * ARG(1); RET(n > 0xf0000000u ? 0 : g_calloc((uint32_t)n)); }
IMP(_realloc) { RET(g_realloc(ARG(0), ARG(1))); }
IMP(_free)    { g_free(ARG(0)); RETV(); }
IMP(_malloc_size) { RET(g_size(ARG(0))); }
IMP(_malloc_default_zone) { RET(g_data("malloc default zone", 64)); }
IMP(_malloc_create_zone)  { RET(g_data("malloc default zone", 64)); }
IMP(_malloc_destroy_zone) { RETV(); }
IMP(_malloc_zone_malloc)  { RET(g_malloc(ARG(1))); }
IMP(_malloc_zone_calloc)  { RET(g_calloc(ARG(1) * ARG(2))); }
IMP(_malloc_zone_free)    { g_free(ARG(1)); RETV(); }
IMP(_memcpy)  { memmove(GP(ARG(0)), GP(ARG(1)), ARG(2)); RET(ARG(0)); }
IMP(_memmove) { memmove(GP(ARG(0)), GP(ARG(1)), ARG(2)); RET(ARG(0)); }
IMP(_memset)  { memset(GP(ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
IMP(___bzero) { memset(GP(ARG(0)), 0, ARG(1)); RETV(); }
IMP(_memset_pattern16) { uint8_t *d = GP(ARG(0)); const uint8_t *p = GP(ARG(1)); uint32_t n = ARG(2); for (uint32_t k = 0; k < n; k++) d[k] = p[k & 15]; RETV(); }
IMP(_memcmp)  { RET(memcmp(GP(ARG(0)), GP(ARG(1)), ARG(2))); }
IMP(_memchr)  { const uint8_t *b = GP(ARG(0)); void *r = memchr(b, (int)ARG(1), ARG(2)); RET(r ? ARG(0) + (uint32_t)((const uint8_t *)r - b) : 0); }

/* ---- strings ---- */
static uint32_t gofs(uint32_t base, const char *host_base, const char *p) { return p ? base + (uint32_t)(p - host_base) : 0; }
IMP(_strlen)  { RET(strlen(S(0))); }
IMP(_strcmp)  { RET(strcmp(S(0), S(1))); }
IMP(_strncmp) { RET(strncmp(S(0), S(1), ARG(2))); }
IMP(_strcasecmp)  { RET(strcasecmp(S(0), S(1))); }
IMP(_strncasecmp) { RET(strncasecmp(S(0), S(1), ARG(2))); }
IMP(_strcoll) { RET(strcmp(S(0), S(1))); }
IMP(_strcpy)  { strcpy(GP(ARG(0)), S(1)); RET(ARG(0)); }
IMP(_strncpy) { strncpy(GP(ARG(0)), S(1), ARG(2)); RET(ARG(0)); }
IMP(_strcat)  { strcat(GP(ARG(0)), S(1)); RET(ARG(0)); }
IMP(_strncat) { strncat(GP(ARG(0)), S(1), ARG(2)); RET(ARG(0)); }
IMP(_strlcpy) { RET(strlcpy(GP(ARG(0)), S(1), ARG(2))); }
IMP(_strlcat) { RET(strlcat(GP(ARG(0)), S(1), ARG(2))); }
IMP(___strcpy_chk)  { strcpy(GP(ARG(0)), S(1)); RET(ARG(0)); }
IMP(___strncpy_chk) { strncpy(GP(ARG(0)), S(1), ARG(2)); RET(ARG(0)); }
IMP(___strcat_chk)  { strcat(GP(ARG(0)), S(1)); RET(ARG(0)); }
IMP(___strlcpy_chk) { RET(strlcpy(GP(ARG(0)), S(1), ARG(2))); }
IMP(_strchr)  { RET(gofs(ARG(0), S(0), strchr(S(0), (int)ARG(1)))); }
IMP(_strrchr) { RET(gofs(ARG(0), S(0), strrchr(S(0), (int)ARG(1)))); }
IMP(_strstr)  { RET(gofs(ARG(0), S(0), strstr(S(0), S(1)))); }
IMP(_strcasestr) { RET(gofs(ARG(0), S(0), strcasestr(S(0), S(1)))); }
IMP(_strpbrk) { RET(gofs(ARG(0), S(0), strpbrk(S(0), S(1)))); }
IMP(_strcspn) { RET(strcspn(S(0), S(1))); }
IMP(_strdup)  { RET(g_strdup(S(0))); }
static __thread uint32_t strtok_last;
IMP(_strtok)
{
    uint32_t s = ARG(0) ? ARG(0) : strtok_last; const char *d = S(1);
    if (!s) RET(0);
    while (G_MEM[s] && strchr(d, G_MEM[s])) s++;
    if (!G_MEM[s]) { strtok_last = 0; RET(0); }
    uint32_t e = s; while (G_MEM[e] && !strchr(d, G_MEM[e])) e++;
    if (G_MEM[e]) { G_MEM[e] = 0; strtok_last = e + 1; } else strtok_last = 0;
    RET(s);
}
static void endptr(uint32_t out, uint32_t base, const char *hb, const char *e) { if (out) rt_w32(G_MEM, out, base + (uint32_t)(e - hb)); }
IMP(_strtol)   { char *e; long v = strtol(S(0), &e, (int)ARG(2)); endptr(ARG(1), ARG(0), S(0), e); if (v > INT32_MAX) v = INT32_MAX; if (v < INT32_MIN) v = INT32_MIN; RET((int32_t)v); }
IMP(_strtoul)  { char *e; unsigned long v = strtoul(S(0), &e, (int)ARG(2)); endptr(ARG(1), ARG(0), S(0), e); RET(v > UINT32_MAX && S(0)[strspn(S(0), " \t")] != '-' ? UINT32_MAX : (uint32_t)v); }
IMP(_strtoull) { char *e; unsigned long long v = strtoull(S(0), &e, (int)ARG(2)); endptr(ARG(1), ARG(0), S(0), e); RET64(v); }
IMP_AS(strtod_unix, "_strtod$UNIX2003") { char *e; double v = strtod(S(0), &e); endptr(ARG(1), ARG(0), S(0), e); RETD(v); }
IMP(_atoi) { RET(atoi(S(0))); }
IMP(_atof) { RETD(atof(S(0))); }
IMP(_ldiv) { int32_t n = (int32_t)ARG(0), d = (int32_t)ARG(1); c->eax = (uint32_t)(n / d); c->edx = (uint32_t)(n % d); RETV(); }   /* ldiv_t in eax:edx */
IMP(___tolower) { RET(tolower((int)ARG(0))); }
IMP(___toupper) { RET(toupper((int)ARG(0))); }
IMP(___maskrune) { RET(__maskrune((int)ARG(0), ARG(1))); }
static __thread char errbuf[128];
IMP_AS(strerror_unix, "_strerror$UNIX2003") { static __thread uint32_t g; if (!g) g = g_malloc(128); strlcpy(GP(g), strerror((int)ARG(0)), 128); RET(g); }
IMP(_strerror_r) { RET(strerror_r((int)ARG(0), GP(ARG(1)), ARG(2))); }
IMP(_setlocale) { static uint32_t cl; if (!cl) cl = g_strdup("C"); RET(cl); }

/* wide strings (wchar_t is 32-bit for both guest and host) */
#define WS(n) ((const wchar_t *)GP(ARG(n)))
static uint32_t wofs(uint32_t base, const wchar_t *hb, const wchar_t *p) { return p ? base + 4u * (uint32_t)(p - hb) : 0; }
IMP(_wcslen) { RET(wcslen(WS(0))); }
IMP(_wcscmp) { RET(wcscmp(WS(0), WS(1))); }
IMP(_wcscpy) { wcscpy(GP(ARG(0)), WS(1)); RET(ARG(0)); }
IMP(_wcsncpy) { wcsncpy(GP(ARG(0)), WS(1), ARG(2)); RET(ARG(0)); }
IMP(_wcsncat) { wcsncat(GP(ARG(0)), WS(1), ARG(2)); RET(ARG(0)); }
IMP(_wcslcpy) { RET(wcslcpy(GP(ARG(0)), WS(1), ARG(2))); }
IMP(_wcslcat) { RET(wcslcat(GP(ARG(0)), WS(1), ARG(2))); }
IMP(_wcschr) { RET(wofs(ARG(0), WS(0), wcschr(WS(0), (wchar_t)ARG(1)))); }
IMP(_wcsstr) { RET(wofs(ARG(0), WS(0), wcsstr(WS(0), WS(1)))); }
IMP(_wmemcmp) { RET(wmemcmp(WS(0), WS(1), ARG(2))); }
IMP(_wmemmove) { wmemmove(GP(ARG(0)), WS(1), ARG(2)); RET(ARG(0)); }
static void wendptr(uint32_t out, uint32_t base, const wchar_t *hb, const wchar_t *e) { if (out) rt_w32(G_MEM, out, base + 4u * (uint32_t)(e - hb)); }
IMP(_wcstol)  { wchar_t *e; long v = wcstol(WS(0), &e, (int)ARG(2)); wendptr(ARG(1), ARG(0), WS(0), e); RET((int32_t)(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v)); }
IMP(_wcstoll) { wchar_t *e; long long v = wcstoll(WS(0), &e, (int)ARG(2)); wendptr(ARG(1), ARG(0), WS(0), e); RET64(v); }
IMP(_wcstod)  { wchar_t *e; double v = wcstod(WS(0), &e); wendptr(ARG(1), ARG(0), WS(0), e); RETD(v); }
IMP(_mbstowcs)
{
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2), k = 0;    /* "C" locale: bytes widen one to one */
    for (; !d || k < n; k++) { uint8_t b = G_MEM[s + k]; if (d) rt_w32(G_MEM, d + 4 * k, b); if (!b) break; }
    RET(k);
}
IMP(_wcstombs)
{
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2), k = 0;
    for (; !d || k < n; k++) { uint32_t w = rt_r32(G_MEM, s + 4 * k); if (d) G_MEM[d + k] = (uint8_t)(w < 256 ? w : '?'); if (!w) break; }
    RET(k);
}

/* ---- math (double results in st(0); float arguments are 4-byte slots) ---- */
IMP(_acos) { RETD(acos(ARGD(0))); }
IMP(_asin) { RETD(asin(ARGD(0))); }
IMP(_atan) { RETD(atan(ARGD(0))); }
IMP(_cos) { RETD(cos(ARGD(0))); }
IMP(_sin) { RETD(sin(ARGD(0))); }
IMP(_tan) { RETD(tan(ARGD(0))); }
IMP(_exp) { RETD(exp(ARGD(0))); }
IMP(_log) { RETD(log(ARGD(0))); }
IMP(_log10) { RETD(log10(ARGD(0))); }
IMP(_floor) { RETD(floor(ARGD(0))); }
IMP(_ceil) { RETD(ceil(ARGD(0))); }
IMP(_rint) { RETD(rint(ARGD(0))); }
IMP(_acosf) { RETD(acosf(ARGF(0))); }
IMP(_atanf) { RETD(atanf(ARGF(0))); }
IMP(_cosf) { RETD(cosf(ARGF(0))); }
IMP(_sinf) { RETD(sinf(ARGF(0))); }
IMP(_tanf) { RETD(tanf(ARGF(0))); }
IMP(_expf) { RETD(expf(ARGF(0))); }
IMP(_logf) { RETD(logf(ARGF(0))); }
IMP(_floorf) { RETD(floorf(ARGF(0))); }
IMP(_ceilf) { RETD(ceilf(ARGF(0))); }
IMP(_rintf) { RETD(rintf(ARGF(0))); }
IMP(_logbf) { RETD(logbf(ARGF(0))); }
IMP(_atan2)  { RETD(atan2(ARGD(0), ARGD(2))); }
IMP(_pow)    { RETD(pow(ARGD(0), ARGD(2))); }
IMP(_fmod)   { RETD(fmod(ARGD(0), ARGD(2))); }
IMP(_atan2f) { RETD(atan2f(ARGF(0), ARGF(1))); }
IMP(_powf)   { RETD(powf(ARGF(0), ARGF(1))); }
IMP(_fmodf)  { RETD(fmodf(ARGF(0), ARGF(1))); }
IMP(_fmaxf)  { RETD(fmaxf(ARGF(0), ARGF(1))); }
IMP(_hypotf) { RETD(hypotf(ARGF(0), ARGF(1))); }
IMP(_ldexp)  { RETD(ldexp(ARGD(0), (int)ARG(2))); }
IMP(_scalbnf){ RETD(scalbnf(ARGF(0), (int)ARG(1))); }
IMP(_frexp)  { int e; double m = frexp(ARGD(0), &e); rt_w32(G_MEM, ARG(2), (uint32_t)e); RETD(m); }
IMP(___divdi3)  { int64_t a = (int64_t)ARG64(0), b = (int64_t)ARG64(2); RET64(b ? a / b : 0); }
IMP(___moddi3)  { int64_t a = (int64_t)ARG64(0), b = (int64_t)ARG64(2); RET64(b ? a % b : 0); }
IMP(___udivdi3) { uint64_t a = ARG64(0), b = ARG64(2); RET64(b ? a / b : 0); }
IMP(___umoddi3) { uint64_t a = ARG64(0), b = ARG64(2); RET64(b ? a % b : 0); }
IMP(___fixunsdfdi) { double d = ARGD(0); RET64(d <= 0 || d != d ? 0 : d >= 18446744073709551616.0 ? UINT64_MAX : (uint64_t)d); }
IMP(___fixunssfdi) { float d = ARGF(0); RET64(d <= 0 || d != d ? 0 : d >= 18446744073709551616.0f ? UINT64_MAX : (uint64_t)d); }
IMP(_rand)  { RET(rand()); }
IMP(_srand) { srand(ARG(0)); RETV(); }

/* qsort with the guest comparator */
static __thread struct { Ctx *c; uint32_t cmp; } qs;
static int qs_cmp(void *ctx, const void *a, const void *b)
{
    (void)ctx; uint32_t args[2] = {(uint32_t)((const uint8_t *)a - G_MEM), (uint32_t)((const uint8_t *)b - G_MEM)};
    return (int)g_call(qs.c, qs.cmp, 2, args);
}
IMP(_qsort)
{
    __typeof__(qs) save = qs; qs.c = c; qs.cmp = ARG(3);
    qsort_r(GP(ARG(0)), ARG(1), ARG(2), 0, qs_cmp);
    qs = save; RETV();
}

/* ---- time ---- */
static void tm_out(uint32_t g, const struct tm *t)    /* struct tm (i386): 9 ints, long gmtoff, char *zone */
{
    int32_t v[9] = {t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon, t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst};
    for (int k = 0; k < 9; k++) rt_w32(G_MEM, g + 4u * (uint32_t)k, (uint32_t)v[k]);
    rt_w32(G_MEM, g + 36, (uint32_t)t->tm_gmtoff);
    static uint32_t zone; if (!zone) zone = g_strdup("UTC");
    rt_w32(G_MEM, g + 40, zone);
}
static void tm_in(uint32_t g, struct tm *t)
{
    memset(t, 0, sizeof *t);
    t->tm_sec = (int)rt_r32(G_MEM, g); t->tm_min = (int)rt_r32(G_MEM, g + 4); t->tm_hour = (int)rt_r32(G_MEM, g + 8);
    t->tm_mday = (int)rt_r32(G_MEM, g + 12); t->tm_mon = (int)rt_r32(G_MEM, g + 16); t->tm_year = (int)rt_r32(G_MEM, g + 20);
    t->tm_wday = (int)rt_r32(G_MEM, g + 24); t->tm_yday = (int)rt_r32(G_MEM, g + 28); t->tm_isdst = (int)rt_r32(G_MEM, g + 32);
}
IMP(_time) { time_t t = time(0); if (ARG(0)) rt_w32(G_MEM, ARG(0), (uint32_t)t); RET((uint32_t)t); }
IMP(_gettimeofday) { struct timeval tv; gettimeofday(&tv, 0); if (ARG(0)) { rt_w32(G_MEM, ARG(0), (uint32_t)tv.tv_sec); rt_w32(G_MEM, ARG(0) + 4, (uint32_t)tv.tv_usec); } RET(0); }
IMP_AS(clock_unix, "_clock$UNIX2003") { RET((uint32_t)clock()); }
IMP(_mach_absolute_time) { RET64(mach_absolute_time()); }
IMP(_mach_timebase_info) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); rt_w32(G_MEM, ARG(0), tb.numer); rt_w32(G_MEM, ARG(0) + 4, tb.denom); RET(0); }
static uint32_t static_tm(void) { static __thread uint32_t g; if (!g) g = g_calloc(48); return g; }
IMP(_localtime)   { time_t t = (int32_t)rt_r32(G_MEM, ARG(0)); struct tm r; localtime_r(&t, &r); uint32_t g = static_tm(); tm_out(g, &r); RET(g); }
IMP(_localtime_r) { time_t t = (int32_t)rt_r32(G_MEM, ARG(0)); struct tm r; localtime_r(&t, &r); tm_out(ARG(1), &r); RET(ARG(1)); }
IMP(_gmtime_r)    { time_t t = (int32_t)rt_r32(G_MEM, ARG(0)); struct tm r; gmtime_r(&t, &r); tm_out(ARG(1), &r); RET(ARG(1)); }
IMP_AS(mktime_unix, "_mktime$UNIX2003") { struct tm t; tm_in(ARG(0), &t); time_t r = mktime(&t); tm_out(ARG(0), &t); RET((uint32_t)r); }
IMP(_asctime) { struct tm t; tm_in(ARG(0), &t); static __thread uint32_t g; if (!g) g = g_malloc(32); char b[32]; asctime_r(&t, b); strlcpy(GP(g), b, 32); RET(g); }
IMP_AS(strftime_unix, "_strftime$UNIX2003") { struct tm t; tm_in(ARG(3), &t); RET(strftime(GP(ARG(0)), ARG(1), S(2), &t)); }
IMP_AS(nanosleep_unix, "_nanosleep$UNIX2003")
{
    struct timespec ts = {(int32_t)rt_r32(G_MEM, ARG(0)), (int32_t)rt_r32(G_MEM, ARG(0) + 4)}, rem;
    int r = nanosleep(&ts, &rem);
    if (r && ARG(1)) { rt_w32(G_MEM, ARG(1), (uint32_t)rem.tv_sec); rt_w32(G_MEM, ARG(1) + 4, (uint32_t)rem.tv_nsec); }
    RET(r);
}

/* ---- stdio: guest FILE objects (magic 'FILE', host index) ---- */
#define NFILES 256
static FILE *files[NFILES] = {0};
static pthread_mutex_t files_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *hfile(uint32_t g)
{
    if (!g) return 0;
    if (rt_r32(G_MEM, g) != 0x46494c45u) return 0;
    uint32_t k = rt_r32(G_MEM, g + 4);
    if (k == 0) return stdin;
    if (k == 1) return stdout;
    if (k == 2) return stderr;
    return k < NFILES ? files[k] : 0;
}
uint32_t sys_new_file(FILE *f)                         /* wraps a host FILE in a guest FILE object */
{
    pthread_mutex_lock(&files_lock);
    uint32_t k = 3; while (k < NFILES && files[k]) k++;
    if (k == NFILES) { pthread_mutex_unlock(&files_lock); fclose(f); return 0; }
    files[k] = f; pthread_mutex_unlock(&files_lock);
    uint32_t g = g_calloc(88); rt_w32(G_MEM, g, 0x46494c45u); rt_w32(G_MEM, g + 4, k);
    return g;
}
void sys_write_stream(uint32_t file, const char *s, size_t n)
{
    FILE *f = file ? hfile(file) : stdout;
    if (f) fwrite(s, 1, n, f);
}
IMP(_fclose)
{
    FILE *f = hfile(ARG(0)); if (!f) RET(-1);
    uint32_t k = rt_r32(G_MEM, ARG(0) + 4);
    if (k < 3) RET(0);
    int r = fclose(f); pthread_mutex_lock(&files_lock); files[k] = 0; pthread_mutex_unlock(&files_lock);
    rt_w32(G_MEM, ARG(0), 0); g_free(ARG(0)); RET(r);
}
IMP(_fread)  { FILE *f = hfile(ARG(3)); RET(f ? fread(GP(ARG(0)), ARG(1), ARG(2), f) : 0); }
IMP_AS(fwrite_unix, "_fwrite$UNIX2003") { FILE *f = hfile(ARG(3)); RET(f ? fwrite(GP(ARG(0)), ARG(1), ARG(2), f) : 0); }
IMP(_fseek)  { FILE *f = hfile(ARG(0)); RET(f ? fseek(f, (int32_t)ARG(1), (int)ARG(2)) : -1); }
IMP(_ftell)  { FILE *f = hfile(ARG(0)); RET(f ? (uint32_t)ftell(f) : 0xffffffffu); }
IMP(_feof)   { FILE *f = hfile(ARG(0)); RET(f ? feof(f) : 1); }
IMP(_ferror) { FILE *f = hfile(ARG(0)); RET(f ? ferror(f) : 1); }
IMP(_fflush) { FILE *f = ARG(0) ? hfile(ARG(0)) : 0; RET(fflush(f)); }
IMP(_fgetc)  { FILE *f = hfile(ARG(0)); RET(f ? fgetc(f) : -1); }
IMP(_ungetc) { FILE *f = hfile(ARG(1)); RET(f ? ungetc((int)ARG(0), f) : -1); }
IMP(_fgets)  { FILE *f = hfile(ARG(2)); RET(f && fgets(GP(ARG(0)), (int)ARG(1), f) ? ARG(0) : 0); }
IMP_AS(fputs_unix, "_fputs$UNIX2003") { FILE *f = hfile(ARG(1)); RET(f ? fputs(S(0), f) : -1); }
IMP(_puts) { RET(puts(S(0))); }
int g_scan_string(const char *s, uint32_t fmt, uint32_t ap);
IMP(_fscanf)
{
    FILE *f = hfile(ARG(0)); char line[4096];
    if (!f || !fgets(line, sizeof line, f)) RET(-1);
    RET(g_scan_string(line, ARG(1), c->esp + 12));
}
IMP(_tmpnam) { static uint32_t g; if (!g) g = g_malloc(1024); char t[] = "/tmp/hwXXXXXX"; int fd = mkstemp(t); if (fd >= 0) { close(fd); unlink(t); } strlcpy(GP(ARG(0) ? ARG(0) : g), t, 1024); RET(ARG(0) ? ARG(0) : g); }

/* ---- file descriptors ---- */
IMP(_close) { RET(close((int)ARG(0))); }
IMP(_read)  { ssize_t r = read((int)ARG(0), GP(ARG(1)), ARG(2)); if (r < 0) set_errno(errno); RET((int32_t)r); }
IMP(_write) { ssize_t r = write((int)ARG(0), GP(ARG(1)), ARG(2)); if (r < 0) set_errno(errno); RET((int32_t)r); }
IMP(_lseek) { off_t r = lseek((int)ARG(0), (off_t)ARG64(1), (int)ARG(3)); RET64(r); }
IMP_AS(chmod_unix, "_chmod$UNIX2003") { RET(chmod(S(0), (mode_t)ARG(1))); }
IMP(_getcwd)
{
    char b[1024]; if (!getcwd(b, sizeof b)) RET(0);
    uint32_t d = ARG(0); if (!d) d = g_malloc((uint32_t)strlen(b) + 1); else if (strlen(b) + 1 > ARG(1)) RET(0);
    strcpy(GP(d), b); RET(d);
}
IMP_AS(realpath_ext, "_realpath$DARWIN_EXTSN")
{
    char b[PATH_MAX]; if (!realpath(S(0), b)) { set_errno(errno); RET(0); }
    uint32_t d = ARG(1) ? ARG(1) : g_malloc(PATH_MAX); strlcpy(GP(d), b, PATH_MAX); RET(d);
}
IMP(_mkdtemp) { RET(mkdtemp(GP(ARG(0))) ? ARG(0) : 0); }
/* mmap: anonymous memory or a private copy of a file (the game maps files read-only) */
IMP_AS(mmap_unix, "_mmap$UNIX2003")
{
    uint32_t len = ARG(1); int flags = (int)ARG(3), fd = (int)ARG(4); off_t off = (off_t)ARG64(5);
    uint32_t a = g_calloc(len);
    if (!a) { set_errno(ENOMEM); RET(0xffffffffu); }
    if (!(flags & MAP_ANON) && fd >= 0) { ssize_t r = pread(fd, GP(a), len, off); (void)r; }
    RET(a);
}
IMP_AS(munmap_unix, "_munmap$UNIX2003") { g_free(ARG(0)); RET(0); }

/* ---- process ---- */
static struct { uint32_t fn, arg; } atexit_list[1024]; static int natexit;
IMP(___cxa_atexit) { if (natexit < 1024) { atexit_list[natexit].fn = ARG(0); atexit_list[natexit].arg = ARG(1); natexit++; } RET(0); }
void sys_exit_handlers(void)
{
    if (getenv("HW_SKIP_ATEXIT")) return;
    Ctx *c = mac_ctx();
    while (natexit > 0) { natexit--; uint32_t a = atexit_list[natexit].arg; uint32_t fn = atexit_list[natexit].fn; atexit_list[natexit].fn = 0; if (fn) g_call(c, fn, 1, &a); }
}
/* exit(): the game's exit handlers (static destructors, atexit) first, as C does, then the host's */
IMP(_exit)  { extern unsigned long long hw_ms(void); hw_log("exit(%d) at %llu ms", (int)ARG(0), hw_ms()); sys_exit_handlers(); hw_log("exit handlers done at %llu ms", hw_ms()); fflush(0); exit((int)ARG(0)); }
IMP(__exit) { extern unsigned long long hw_ms(void); hw_log("_exit(%d) at %llu ms from %08x", (int)ARG(0), hw_ms(), rt_r32(G_MEM, c->esp)); fflush(0); _exit((int)ARG(0)); }
IMP(_abort) { hw_log("abort() called from %08x", rt_r32(G_MEM, c->esp)); rt_unhandled(c, rt_r32(G_MEM, c->esp), "abort"); }
IMP(___stack_chk_fail) { uint32_t g = g_data("___stack_chk_guard", 4); hw_log("stack check: guard %08x, frame copy [ebp-0x10] %08x (ebp %08x), slot 68103c = %08x", rt_r32(G_MEM, g), rt_r32(G_MEM, c->ebp - 16), c->ebp, rt_r32(G_MEM, 0x68103c)); rt_unhandled(c, rt_r32(G_MEM, c->esp), "stack protector: guest stack corrupted"); }
static struct { char *name; uint32_t g; } envs[256]; static int nenvs; static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;
IMP(_getenv)
{
    const char *n = S(0), *v = getenv(n);
    if (!v) RET(0);
    pthread_mutex_lock(&env_lock);
    for (int k = 0; k < nenvs; k++) if (!strcmp(envs[k].name, n)) {
        if (strcmp(GS(envs[k].g), v)) envs[k].g = g_strdup(v);
        uint32_t r = envs[k].g; pthread_mutex_unlock(&env_lock); RET(r);
    }
    uint32_t g = g_strdup(v);
    if (nenvs < 256) { envs[nenvs].name = strdup(n); envs[nenvs].g = g; nenvs++; }
    pthread_mutex_unlock(&env_lock); RET(g);
}
IMP(_getpid) { RET(getpid()); }
IMP(_gethostname) { RET(gethostname(GP(ARG(0)), ARG(1))); }
extern char hw_exe_path[1024];
char hw_exe_path[1024];
IMP(__NSGetExecutablePath)
{
    size_t n = strlen(hw_exe_path) + 1; uint32_t cap = rt_r32(G_MEM, ARG(1));
    if (n > cap) { rt_w32(G_MEM, ARG(1), (uint32_t)n); RET(-1); }
    memcpy(GP(ARG(0)), hw_exe_path, n); RET(0);
}
IMP(_sysconf)
{
    int n = (int)ARG(0);
    if (n == _SC_NPROCESSORS_ONLN || n == _SC_NPROCESSORS_CONF) RET(sysconf(n));
    if (n == _SC_PAGESIZE) RET(4096);
    long v = sysconf(n); RET((int32_t)(v > INT32_MAX ? INT32_MAX : v));
}
/* sysctl: values a 32-bit game asks for (memory size capped so 32-bit arithmetic does not overflow) */
static int sysctl_value(const char *name, uint8_t *out, uint32_t *len)
{
    uint64_t v64 = 0; int is64 = 0; uint32_t v32 = 0; const char *str = 0; size_t hl = 8;
    if (!strcmp(name, "hw.memsize")) { sysctlbyname("hw.memsize", &v64, &hl, 0, 0); if (v64 > (3ull << 30)) v64 = 3ull << 30; is64 = 1; }
    else if (!strcmp(name, "hw.physmem")) { v32 = 0x7fffffff; }
    else if (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.activecpu") || !strcmp(name, "hw.logicalcpu") || !strcmp(name, "hw.physicalcpu")) { int n = 0; size_t l = 4; sysctlbyname("hw.ncpu", &n, &l, 0, 0); v32 = (uint32_t)n; }
    else if (!strcmp(name, "hw.cpufrequency") || !strcmp(name, "hw.cpufrequency_max")) { v64 = 3200000000ull; is64 = 1; }
    else if (!strcmp(name, "hw.busfrequency")) { v64 = 100000000ull; is64 = 1; }
    else if (!strcmp(name, "hw.model")) str = "MacBookPro16,1";
    else if (!strcmp(name, "hw.machine")) str = "i386";
    else if (!strcmp(name, "kern.osrelease")) str = "19.6.0";       /* reported as macOS 10.15 */
    else if (!strcmp(name, "kern.ostype")) str = "Darwin";
    else if (!strcmp(name, "kern.osversion")) str = "19H2";
    else if (!strcmp(name, "machdep.cpu.brand_string")) str = "Intel(R) Core(TM) i7 CPU";
    else if (!strcmp(name, "hw.cachelinesize")) v32 = 64;
    else if (!strcmp(name, "hw.l2cachesize")) v32 = 4 << 20;
    else return -1;
    if (str) {
        uint32_t n = (uint32_t)strlen(str) + 1;
        if (out) { if (*len < n) return -1; memcpy(out, str, n); }
        *len = n; return 0;
    }
    if (is64 && (!out || *len >= 8)) { if (out) memcpy(out, &v64, 8); *len = 8; return 0; }
    if (is64) v32 = (uint32_t)(v64 > UINT32_MAX ? UINT32_MAX : v64);
    if (out) { if (*len < 4) return -1; memcpy(out, &v32, 4); }
    *len = 4; return 0;
}
IMP(_sysctlbyname)
{
    uint32_t len = ARG(2) ? rt_r32(G_MEM, ARG(2)) : 0;
    int r = sysctl_value(S(0), ARG(1) ? GP(ARG(1)) : 0, &len);
    if (r) { if (getenv("HW_LOG_SYSCTL")) hw_log("sysctlbyname(%s): not provided", S(0)); set_errno(ENOENT); RET(-1); }
    if (ARG(2)) rt_w32(G_MEM, ARG(2), len); RET(0);
}
IMP(_sysctl)
{
    int32_t a = (int32_t)rt_r32(G_MEM, ARG(0)), b = ARG(1) > 1 ? (int32_t)rt_r32(G_MEM, ARG(0) + 4) : -1;
    const char *n = 0;
    if (a == CTL_HW && b == HW_MEMSIZE) n = "hw.memsize"; else if (a == CTL_HW && b == HW_NCPU) n = "hw.ncpu";
    else if (a == CTL_HW && b == HW_PHYSMEM) n = "hw.physmem"; else if (a == CTL_HW && b == HW_MODEL) n = "hw.model";
    else if (a == CTL_HW && b == HW_MACHINE) n = "hw.machine"; else if (a == CTL_HW && b == HW_CPU_FREQ) n = "hw.cpufrequency";
    else if (a == CTL_KERN && b == KERN_OSRELEASE) n = "kern.osrelease"; else if (a == CTL_KERN && b == KERN_OSTYPE) n = "kern.ostype";
    if (!n) { if (getenv("HW_LOG_SYSCTL")) hw_log("sysctl(%d, %d): not provided", a, b); set_errno(ENOENT); RET(-1); }
    uint32_t len = ARG(3) ? rt_r32(G_MEM, ARG(3)) : 0;
    if (sysctl_value(n, ARG(2) ? GP(ARG(2)) : 0, &len)) RET(-1);
    if (ARG(3)) rt_w32(G_MEM, ARG(3), len); RET(0);
}
IMP_AS(getrlimit_unix, "_getrlimit$UNIX2003")
{
    struct rlimit r; int k = getrlimit((int)ARG(0), &r);
    rt_w64(G_MEM, ARG(1), r.rlim_cur); rt_w64(G_MEM, ARG(1) + 8, r.rlim_max); RET(k);
}
IMP_AS(setrlimit_unix, "_setrlimit$UNIX2003") { RET(0); }
IMP(_mach_host_self) { RET(0x203); }
IMP(_host_page_size) { rt_w32(G_MEM, ARG(1), 4096); RET(0); }
IMP(_host_info)            /* HOST_BASIC_INFO: max_cpus, avail_cpus, memory_size (capped), cpu_type... */
{
    uint32_t out = ARG(2), cnt = rt_r32(G_MEM, ARG(3));
    int n = 0; size_t l = 4; sysctlbyname("hw.ncpu", &n, &l, 0, 0);
    uint32_t v[12] = {(uint32_t)n, (uint32_t)n, 0x7fffffff, 7, 4, (uint32_t)n, (uint32_t)n, (uint32_t)n, (uint32_t)n, 0, 0, 0};
    uint64_t mem = 3ull << 30; memcpy(&v[10], &mem, 8);
    for (uint32_t k = 0; k < cnt && k < 12; k++) rt_w32(G_MEM, out + 4 * k, v[k]);
    RET(0);
}
IMP(_host_statistics)      /* HOST_VM_INFO: report plenty of free pages */
{
    uint32_t out = ARG(2), cnt = rt_r32(G_MEM, ARG(3));
    for (uint32_t k = 0; k < cnt; k++) rt_w32(G_MEM, out + 4 * k, 0);
    if (cnt > 0) rt_w32(G_MEM, out, 0x80000);          /* free_count: 2 GB of 4 KB pages */
    RET(0);
}
IMP(_mach_vm_region) { RET(1); }                       /* KERN_INVALID_ADDRESS */
IMP(_semaphore_create)
{
    semaphore_t s; kern_return_t r = semaphore_create(mach_task_self(), &s, (int)ARG(2), (int)ARG(3));
    rt_w32(G_MEM, ARG(1), s); RET(r);
}
IMP(_semaphore_destroy) { RET(semaphore_destroy(mach_task_self(), ARG(1))); }
IMP(_semaphore_signal)  { RET(semaphore_signal(ARG(0))); }
IMP(_semaphore_wait)    { RET(semaphore_wait(ARG(0))); }
IMP(_signal) { RET(0); }
IMP(_sigaction) { if (ARG(2)) memset(GP(ARG(2)), 0, 12); RET(0); }
IMP(_dlopen)                                           /* every library resolves through the runtime's symbols */
{
    if (getenv("HW_LOG_DLSYM")) hw_log("dlopen(%s)", ARG(0) ? S(0) : "(main program)");
    RET(0x00100000u);                                  /* a handle (any non-null value: dlsym ignores it) */
}
IMP(_dlsym)                                            /* symbols the runtime implements (C names get their underscore) */
{
    char n[256]; snprintf(n, sizeof n, "_%s", GS(ARG(1)));
    extern int hw_has_impl(const char *sym);
    if (hw_has_impl(n)) RET(g_thunk(n));
    if (hw_has_impl(n + 1)) RET(g_thunk(n + 1));        /* GL entry points are registered by their plain names */
    if (getenv("HW_LOG_DLSYM")) hw_log("dlsym(%s): not provided", GS(ARG(1)));
    RET(0);
}
uint32_t sys_new_file(FILE *f);
static uint32_t do_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (getenv("HW_LOG_FILES")) hw_log("fopen(%s, %s)%s", path, mode, f ? "" : " failed");
    if (!f) { set_errno(errno); return 0; }
    return sys_new_file(f);
}
IMP(_fopen) { RET(do_fopen(S(0), S(1))); }
IMP_AS(fopen_unix, "_fopen$UNIX2003") { RET(do_fopen(S(0), S(1))); }
IMP(_dlclose) { RET(0); }
IMP(_dlerror) { static uint32_t g; if (!g) g = g_strdup("dynamic loading is not available"); RET(g); }
IMP(_execv) { set_errno(ENOSYS); RET(-1); }
IMP(_posix_spawn) { RET(ENOSYS); }
IMP(_posix_spawnattr_init) { RET(0); }
IMP(_posix_spawnattr_destroy) { RET(0); }
IMP(_posix_spawnattr_setflags) { RET(0); }
IMP(_posix_spawnattr_setbinpref_np) { RET(0); }
IMP(_getifaddrs) { rt_w32(G_MEM, ARG(0), 0); RET(0); }
IMP(_freeifaddrs) { RETV(); }
IMP(_getaddrinfo) { RET(8); }                          /* EAI_NONAME until networking is ported */
IMP(_inet_ntoa) { static uint32_t g; if (!g) g = g_malloc(16); uint32_t a = ARG(0); snprintf(GP(g), 16, "%u.%u.%u.%u", a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24); RET(g); }
IMP_AS(nftw_inode64, "_nftw$INODE64$UNIX2003") { RET(0); }   /* directory walks: none (logged by callers that need them) */

/* atomics */
IMP(_OSAtomicAdd64) { RET64(__atomic_add_fetch((int64_t *)GP(ARG(2)), (int64_t)ARG64(0), __ATOMIC_SEQ_CST)); }
IMP(_OSAtomicCompareAndSwap32Barrier) { uint32_t e = ARG(0); RET(__atomic_compare_exchange_n((uint32_t *)GP(ARG(2)), &e, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); }
IMP(_OSAtomicCompareAndSwap64Barrier) { uint64_t e = ARG64(0); RET(__atomic_compare_exchange_n((uint64_t *)GP(ARG(4)), &e, ARG64(2), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); }

/* blocks / dispatch (the SDL code that used them is replaced) */
IMP(__Block_object_assign) { RETV(); }
IMP(__Block_object_dispose) { RETV(); }
IMP(_dispatch_time) { RET64(0); }
IMP(_dispatch_after) { RETV(); }

/* iconv: guest char ** in/out pointers */
static iconv_t icv[64]; static int nicv;
static pthread_mutex_t icv_lock = PTHREAD_MUTEX_INITIALIZER;
IMP(_iconv_open)
{
    iconv_t h = iconv_open(S(0), S(1)); if (h == (iconv_t)-1) { set_errno(errno); RET(0xffffffffu); }
    pthread_mutex_lock(&icv_lock);
    for (int k = 0; k < 64; k++) if (!icv[k]) { icv[k] = h; if (k >= nicv) nicv = k + 1; pthread_mutex_unlock(&icv_lock); RET(k + 1); }
    pthread_mutex_unlock(&icv_lock); iconv_close(h); set_errno(EMFILE); RET(0xffffffffu);
}
IMP(_iconv_close) { uint32_t k = ARG(0); pthread_mutex_lock(&icv_lock); if (k && k <= (uint32_t)nicv && icv[k - 1]) { iconv_close(icv[k - 1]); icv[k - 1] = 0; } pthread_mutex_unlock(&icv_lock); RET(0); }
IMP(_iconvctl) { RET(0); }
IMP(_iconv)
{
    uint32_t k = ARG(0); if (!k || k > (uint32_t)nicv) RET(0xffffffffu);
    uint32_t pin = ARG(1), pinl = ARG(2), pout = ARG(3), poutl = ARG(4);
    char *in = pin && rt_r32(G_MEM, pin) ? (char *)GP(rt_r32(G_MEM, pin)) : 0, *out = pout ? (char *)GP(rt_r32(G_MEM, pout)) : 0;
    size_t inl = pinl ? rt_r32(G_MEM, pinl) : 0, outl = poutl ? rt_r32(G_MEM, poutl) : 0;
    char *in0 = in, *out0 = out;
    size_t r = iconv(icv[k - 1], in ? &in : 0, &inl, out ? &out : 0, &outl);
    if (in0) rt_w32(G_MEM, pin, rt_r32(G_MEM, pin) + (uint32_t)(in - in0));
    if (out0) rt_w32(G_MEM, pout, rt_r32(G_MEM, pout) + (uint32_t)(out - out0));
    if (pinl) rt_w32(G_MEM, pinl, (uint32_t)inl);
    if (poutl) rt_w32(G_MEM, poutl, (uint32_t)outl);
    if (r == (size_t)-1) set_errno(errno);
    RET((uint32_t)r);
}
IMP(dyld_stub_binder) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "dyld_stub_binder (all slots are bound at load)"); }

/* ---- the POSIX calls Aspyr's ASL::FsApi::Native fetches with dlsym ---- */
static void stat64_out(uint32_t g, const struct stat *st)   /* struct stat (64-bit inodes), i386: 108 bytes */
{
    memset(GP(g), 0, 108);
    rt_w32(G_MEM, g, (uint32_t)st->st_dev); rt_w16(G_MEM, g + 4, st->st_mode); rt_w16(G_MEM, g + 6, st->st_nlink);
    rt_w64(G_MEM, g + 8, st->st_ino); rt_w32(G_MEM, g + 16, st->st_uid); rt_w32(G_MEM, g + 20, st->st_gid);
    rt_w32(G_MEM, g + 24, (uint32_t)st->st_rdev);
    rt_w32(G_MEM, g + 28, (uint32_t)st->st_atimespec.tv_sec); rt_w32(G_MEM, g + 32, (uint32_t)st->st_atimespec.tv_nsec);
    rt_w32(G_MEM, g + 36, (uint32_t)st->st_mtimespec.tv_sec); rt_w32(G_MEM, g + 40, (uint32_t)st->st_mtimespec.tv_nsec);
    rt_w32(G_MEM, g + 44, (uint32_t)st->st_ctimespec.tv_sec); rt_w32(G_MEM, g + 48, (uint32_t)st->st_ctimespec.tv_nsec);
    rt_w32(G_MEM, g + 52, (uint32_t)st->st_birthtimespec.tv_sec); rt_w32(G_MEM, g + 56, (uint32_t)st->st_birthtimespec.tv_nsec);
    rt_w64(G_MEM, g + 60, (uint64_t)st->st_size); rt_w64(G_MEM, g + 68, (uint64_t)st->st_blocks);
    rt_w32(G_MEM, g + 76, (uint32_t)st->st_blksize); rt_w32(G_MEM, g + 80, st->st_flags);
}
static void stat32_out(uint32_t g, const struct stat *st)   /* the old struct stat (32-bit inodes), i386: 96 bytes */
{
    memset(GP(g), 0, 96);
    rt_w32(G_MEM, g, (uint32_t)st->st_dev); rt_w32(G_MEM, g + 4, (uint32_t)st->st_ino); rt_w16(G_MEM, g + 8, st->st_mode);
    rt_w16(G_MEM, g + 10, st->st_nlink); rt_w32(G_MEM, g + 12, st->st_uid); rt_w32(G_MEM, g + 16, st->st_gid);
    rt_w32(G_MEM, g + 20, (uint32_t)st->st_rdev);
    rt_w32(G_MEM, g + 24, (uint32_t)st->st_atimespec.tv_sec); rt_w32(G_MEM, g + 32, (uint32_t)st->st_mtimespec.tv_sec);
    rt_w32(G_MEM, g + 40, (uint32_t)st->st_ctimespec.tv_sec); rt_w64(G_MEM, g + 48, (uint64_t)st->st_size);
    rt_w64(G_MEM, g + 56, (uint64_t)st->st_blocks); rt_w32(G_MEM, g + 64, (uint32_t)st->st_blksize);
}
IMP_AS(stat_inode64, "_stat$INODE64") { struct stat st; int r = stat(S(0), &st); if (r) set_errno(errno); else stat64_out(ARG(1), &st); RET(r); }
IMP_AS(lstat_inode64, "_lstat$INODE64") { struct stat st; int r = lstat(S(0), &st); if (r) set_errno(errno); else stat64_out(ARG(1), &st); RET(r); }
IMP_AS(fstat_inode64, "_fstat$INODE64") { struct stat st; int r = fstat((int)ARG(0), &st); if (r) set_errno(errno); else stat64_out(ARG(1), &st); RET(r); }
IMP(_stat) { struct stat st; int r = stat(S(0), &st); if (r) set_errno(errno); else stat32_out(ARG(1), &st); RET(r); }
IMP(_open) { int r = open(S(0), (int)ARG(1), (mode_t)ARG(2)); if (r < 0) set_errno(errno); if (getenv("HW_LOG_FILES")) hw_log("open(%s, %x) = %d", S(0), ARG(1), r); RET(r); }
IMP_AS(open_unix, "_open$UNIX2003") { int r = open(S(0), (int)ARG(1), (mode_t)ARG(2)); if (r < 0) set_errno(errno); RET(r); }
IMP(_freopen) { RET(0); }
IMP(_access) { int r = access(S(0), (int)ARG(1)); if (r) set_errno(errno); RET(r); }
IMP(_chdir) { int r = chdir(S(0)); if (r) set_errno(errno); RET(r); }
IMP(_mkdir) { int r = mkdir(S(0), (mode_t)ARG(1)); if (r) set_errno(errno); if (getenv("HW_LOG_FILES")) hw_log("mkdir(%s) = %d", S(0), r); RET(r); }
IMP(_rmdir) { int r = rmdir(S(0)); if (r) set_errno(errno); RET(r); }
IMP(_remove) { int r = remove(S(0)); if (r) set_errno(errno); RET(r); }
IMP(_unlink) { int r = unlink(S(0)); if (r) set_errno(errno); RET(r); }
