/* CoreFoundation for the guest: guest CFTypeRefs are handles to host objects.
 *
 * A handle is CF_LO + 8 * index (guest memory that is never read). Each handle owns one host retain and mirrors the
 * guest's retain count: CFRetain/CFRelease on the handle; at zero the host object is released. Objects the guest
 * gets without owning them ("Get" functions) reuse the handle of the same host object, or get a permanent one.
 * The executable's constant strings (__cfstring: isa, flags, bytes, length) are converted once and cached. */
#include "mac.h"
#include "macho_tab.h"
#include <CoreFoundation/CoreFoundation.h>

#define CF_LO  0xfc000000u
#define NCF    (1u << 16)
typedef struct CFH { CFTypeRef obj; int32_t refs; } CFH;   /* refs < 0: permanent */
static CFH cft[NCF]; static uint32_t cf_free_hint = 1;
static pthread_mutex_t cf_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef by_obj;                     /* host object -> handle (non-retaining) */

static uint32_t cf_new(CFTypeRef o, int owned)           /* takes over one host retain when owned */
{
    if (!o) return 0;
    pthread_mutex_lock(&cf_lock);
    if (!by_obj) by_obj = CFDictionaryCreateMutable(0, 0, 0, 0);
    uint32_t h = (uint32_t)(uintptr_t)CFDictionaryGetValue(by_obj, o);
    if (h) {
        CFH *e = &cft[(h - CF_LO) / 8];
        if (owned) { if (e->refs >= 0) e->refs++; else CFRelease(o); }
        pthread_mutex_unlock(&cf_lock); return h;
    }
    uint32_t k = cf_free_hint;
    while (k < NCF && cft[k].obj) k++;
    if (k == NCF) { for (k = 1; k < NCF && cft[k].obj; k++) ; }
    if (k == NCF) { pthread_mutex_unlock(&cf_lock); hw_log("CoreFoundation handle table full"); abort(); }
    cf_free_hint = k + 1;
    if (!owned) CFRetain(o);
    cft[k].obj = o; cft[k].refs = owned ? 1 : -1;
    h = CF_LO + 8 * k;
    CFDictionarySetValue(by_obj, o, (const void *)(uintptr_t)h);
    pthread_mutex_unlock(&cf_lock);
    return h;
}
uint32_t cf_own(CFTypeRef o) { return cf_new(o, 1); }    /* a Create/Copy result */
uint32_t cf_get(CFTypeRef o) { return cf_new(o, 0); }    /* a Get result */

/* data imports: variables holding CF constants, callback structures */
static uint32_t cb_array, cb_dict_keys, cb_dict_values;
static uint32_t pref_app;
static CFStringRef bundle_id(void);
uint32_t cf_data_import(const char *sym)
{
    uint32_t v = 0; int is_var = 1;
    if (!strcmp(sym, "_kCFAllocatorDefault")) v = 0;
    else if (!strcmp(sym, "_kCFBooleanTrue")) v = cf_get(kCFBooleanTrue);
    else if (!strcmp(sym, "_kCFBooleanFalse")) v = cf_get(kCFBooleanFalse);
    else if (!strcmp(sym, "_kCFBundleNameKey")) v = cf_get(kCFBundleNameKey);
    else if (!strcmp(sym, "_kCFPreferencesCurrentApplication")) { if (!pref_app) pref_app = cf_get(kCFPreferencesCurrentApplication); v = pref_app; }
    else if (!strcmp(sym, "_kCFRunLoopDefaultMode") || !strcmp(sym, "_NSDefaultRunLoopMode")) v = cf_get(kCFRunLoopDefaultMode);
    else if (!strcmp(sym, "_kCFAbsoluteTimeIntervalSince1970")) { uint32_t a = g_data(sym, 8); rt_wf64(G_MEM, a, kCFAbsoluteTimeIntervalSince1970); return a; }
    else if (!strcmp(sym, "_kCFTypeArrayCallBacks")) { is_var = 0; cb_array = g_data(sym, 24); return cb_array; }
    else if (!strcmp(sym, "_kCFTypeDictionaryKeyCallBacks")) { is_var = 0; cb_dict_keys = g_data(sym, 24); return cb_dict_keys; }
    else if (!strcmp(sym, "_kCFTypeDictionaryValueCallBacks")) { is_var = 0; cb_dict_values = g_data(sym, 24); return cb_dict_values; }
    else if (!strcmp(sym, "___CFConstantStringClassReference")) return g_data(sym, 64);
    else if (!strncmp(sym, "_kCF", 4) || !strncmp(sym, "_NS", 3) || !strncmp(sym, "_kUT", 4) || !strncmp(sym, "_kTIS", 5)) {
        /* other string constants: their own names (they are only compared or used as keys) */
        CFStringRef s = CFStringCreateWithCString(0, sym + 1, kCFStringEncodingUTF8); v = cf_own(s);
    } else return 0;
    (void)is_var;
    uint32_t a = g_data(sym, 4); rt_w32(G_MEM, a, v); return a;
}

/* guest value -> host object (borrowed) */
static CFMutableDictionaryRef const_strings;
CFTypeRef cf_in(uint32_t g)
{
    if (!g) return 0;
    if (g >= CF_LO && g < CF_LO + 8 * NCF) return cft[(g - CF_LO) / 8].obj;
    if (g - mo_cfstring[0] < mo_cfstring[1]) {          /* a literal in the executable */
        pthread_mutex_lock(&cf_lock);
        if (!const_strings) const_strings = CFDictionaryCreateMutable(0, 0, 0, &kCFTypeDictionaryValueCallBacks);
        CFStringRef s = CFDictionaryGetValue(const_strings, (const void *)(uintptr_t)g);
        if (!s) {
            uint32_t flags = rt_r32(G_MEM, g + 4), p = rt_r32(G_MEM, g + 8), n = rt_r32(G_MEM, g + 12);
            if (flags & 0x10) s = CFStringCreateWithCharacters(0, (const UniChar *)GP(p), n);                /* UTF-16 */
            else s = CFStringCreateWithBytes(0, GP(p), n, kCFStringEncodingMacRoman, false);
            CFDictionarySetValue(const_strings, (const void *)(uintptr_t)g, s); CFRelease(s);
        }
        pthread_mutex_unlock(&cf_lock);
        return s;
    }
    hw_log("not a CoreFoundation object: %08x", g);
    return 0;
}
#define IN(n) cf_in(ARG(n))
IMP(_CFRetain)
{
    uint32_t g = ARG(0);
    if (g >= CF_LO && g < CF_LO + 8 * NCF) { pthread_mutex_lock(&cf_lock); CFH *e = &cft[(g - CF_LO) / 8]; if (e->refs >= 0) e->refs++; pthread_mutex_unlock(&cf_lock); }
    RET(g);
}
IMP(_CFRelease)
{
    uint32_t g = ARG(0);
    if (g >= CF_LO && g < CF_LO + 8 * NCF) {
        pthread_mutex_lock(&cf_lock); CFH *e = &cft[(g - CF_LO) / 8];
        if (e->obj && e->refs > 0 && --e->refs == 0) {
            CFDictionaryRemoveValue(by_obj, e->obj); CFRelease(e->obj); e->obj = 0;
            uint32_t k = (g - CF_LO) / 8; if (k < cf_free_hint) cf_free_hint = k;
        }
        pthread_mutex_unlock(&cf_lock);
    }
    RETV();
}
IMP(_CFGetTypeID) { CFTypeRef o = IN(0); RET(o ? CFGetTypeID(o) : 0); }
IMP(_CFStringGetTypeID) { RET(CFStringGetTypeID()); }
IMP(_CFNumberGetTypeID) { RET(CFNumberGetTypeID()); }
IMP(_CFDataGetTypeID) { RET(CFDataGetTypeID()); }
IMP(_CFAllocatorGetDefault) { RET(0); }
IMP(_CFShow) { CFShow(IN(0)); RETV(); }

/* ---- strings ---- */
CFStringRef cf_str(const char *s) { return CFStringCreateWithCString(0, s, kCFStringEncodingUTF8); }
char *cf_cstr(CFTypeRef s)                                   /* malloc'd UTF-8 copy */
{
    if (!s || CFGetTypeID(s) != CFStringGetTypeID()) return strdup("");
    CFIndex n = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    char *b = malloc((size_t)n); if (!CFStringGetCString(s, b, n, kCFStringEncodingUTF8)) b[0] = 0;
    return b;
}
static CFStringEncoding enc_in(uint32_t e) { return e; }
IMP(_CFStringCreateWithCString) { RET(cf_own(CFStringCreateWithCString(0, GS(ARG(1)), enc_in(ARG(2))))); }
IMP(_CFStringCreateWithBytes) { RET(cf_own(CFStringCreateWithBytes(0, GP(ARG(1)), (CFIndex)(int32_t)ARG(2), enc_in(ARG(3)), (Boolean)ARG(4)))); }
IMP(_CFStringCreateWithCharacters) { RET(cf_own(CFStringCreateWithCharacters(0, GP(ARG(1)), (CFIndex)(int32_t)ARG(2)))); }
IMP(_CFStringCreateMutable) { RET(cf_own(CFStringCreateMutable(0, (CFIndex)(int32_t)ARG(1)))); }
IMP(_CFStringCreateMutableCopy) { RET(cf_own(CFStringCreateMutableCopy(0, (CFIndex)(int32_t)ARG(1), IN(2)))); }
IMP(_CFStringAppend) { CFStringAppend((CFMutableStringRef)IN(0), IN(1)); RETV(); }
IMP(_CFStringGetLength) { CFTypeRef s = IN(0); RET(s ? CFStringGetLength(s) : 0); }
IMP(_CFStringGetCString) { CFTypeRef s = IN(0); RET(s && CFStringGetCString(s, GP(ARG(1)), (CFIndex)(int32_t)ARG(2), enc_in(ARG(3)))); }
IMP(_CFStringGetCharacters) { CFStringGetCharacters(IN(0), CFRangeMake((int32_t)ARG(1), (int32_t)ARG(2)), GP(ARG(3))); RETV(); }
IMP(_CFStringGetBytes)       /* (str, range{loc,len}, enc, lossByte, isExternal, buf, maxBufLen, usedBufLen*) */
{
    CFIndex used = 0;
    CFIndex r = CFStringGetBytes(IN(0), CFRangeMake((int32_t)ARG(1), (int32_t)ARG(2)), ARG(3), (UInt8)ARG(4), (Boolean)ARG(5),
                                 ARG(6) ? GP(ARG(6)) : 0, (int32_t)ARG(7), &used);
    if (ARG(8)) rt_w32(G_MEM, ARG(8), (uint32_t)used);
    RET(r);
}
IMP(_CFStringGetMaximumSizeForEncoding) { RET(CFStringGetMaximumSizeForEncoding((int32_t)ARG(0), ARG(1))); }
IMP(_CFStringGetSystemEncoding) { RET(CFStringGetSystemEncoding()); }
IMP(_CFStringGetIntValue) { RET(CFStringGetIntValue(IN(0))); }
IMP(_CFStringCompare) { RET(CFStringCompare(IN(0), IN(1), ARG(2))); }
IMP(_CFStringNormalize) { CFStringNormalize((CFMutableStringRef)IN(0), ARG(1)); RETV(); }
IMP(_CFStringCreateArrayBySeparatingStrings) { RET(cf_own(CFStringCreateArrayBySeparatingStrings(0, IN(1), IN(2)))); }
IMP(_CFStringCreateByCombiningStrings) { RET(cf_own(CFStringCreateByCombiningStrings(0, IN(1), IN(2)))); }
/* CFStringCreateWithFormat(alloc, options, format, ...): %@ becomes the object's description, the rest goes through
 * the guest printf engine (the arguments are rebuilt with %@ replaced by guest C strings) */
char *g_format(uint32_t fmt, uint32_t ap);
IMP(_CFStringCreateWithFormat)
{
    char *f = cf_cstr(IN(2)); size_t n = strlen(f);
    uint32_t gf = g_malloc((uint32_t)n * 2 + 2), ap = c->esp + 16, nap = g_malloc(512), o = 0, j = 0;
    uint32_t tmp[64]; int ntmp = 0;
    for (size_t i = 0; i < n; i++) {
        G_MEM[gf + j++] = (uint8_t)f[i];
        if (f[i] != '%') continue;
        size_t k = i + 1;
        if (f[k] == '%') { G_MEM[gf + j++] = '%'; i = k; continue; }
        while (k < n && strchr("-+ #0123456789.*'hlqLjzt", f[k])) {
            if (f[k] == '*') { rt_w32(G_MEM, nap + o, rt_r32(G_MEM, ap)); o += 4; ap += 4; }
            G_MEM[gf + j++] = (uint8_t)f[k++];
        }
        char conv = f[k]; int ll = strstr(f + i, "ll") == f + i + 1 || (k > i + 1 && f[k - 1] == 'q');
        if (conv == '@') {
            CFTypeRef ob = cf_in(rt_r32(G_MEM, ap)); ap += 4;
            CFStringRef d = ob ? (CFGetTypeID(ob) == CFStringGetTypeID() ? CFRetain(ob) : CFCopyDescription(ob)) : cf_str("(null)");
            char *s = cf_cstr(d); CFRelease(d);
            uint32_t gs = g_strdup(s); free(s); if (ntmp < 64) tmp[ntmp++] = gs;
            rt_w32(G_MEM, nap + o, gs); o += 4; G_MEM[gf + j++] = 's';
        } else {
            G_MEM[gf + j++] = (uint8_t)conv;
            int w = strchr("fFeEgGaA", conv) || ll ? 8 : 4;
            memcpy(G_MEM + nap + o, G_MEM + ap, (size_t)w); o += (uint32_t)w; ap += (uint32_t)w;
        }
        i = k;
        if (o > 480) break;
    }
    G_MEM[gf + j] = 0;
    char *s = g_format(gf, nap);
    uint32_t r = cf_own(cf_str(s)); free(s); free(f);
    for (int k = 0; k < ntmp; k++) g_free(tmp[k]);
    g_free(gf); g_free(nap); RET(r);
}

/* ---- numbers, booleans, data ---- */
static CFNumberType num_type(uint32_t t, int *size)       /* i386 sizes: long/CFIndex/NSInteger 4, CGFloat float */
{
    switch (t) {
    case 1: *size = 1; return kCFNumberSInt8Type; case 2: *size = 2; return kCFNumberSInt16Type;
    case 3: *size = 4; return kCFNumberSInt32Type; case 4: *size = 8; return kCFNumberSInt64Type;
    case 5: *size = 4; return kCFNumberFloat32Type; case 6: *size = 8; return kCFNumberFloat64Type;
    case 7: *size = 1; return kCFNumberCharType; case 8: *size = 2; return kCFNumberShortType;
    case 9: *size = 4; return kCFNumberIntType; case 10: *size = 4; return kCFNumberSInt32Type;
    case 11: *size = 8; return kCFNumberLongLongType; case 12: *size = 4; return kCFNumberFloatType;
    case 13: *size = 8; return kCFNumberDoubleType; case 14: case 15: *size = 4; return kCFNumberSInt32Type;
    case 16: *size = 4; return kCFNumberFloat32Type;
    }
    *size = 4; return kCFNumberSInt32Type;
}
IMP(_CFNumberCreate) { int sz; CFNumberType t = num_type(ARG(1), &sz); uint8_t v[8] = {0}; memcpy(v, GP(ARG(2)), (size_t)sz); RET(cf_own(CFNumberCreate(0, t, v))); }
IMP(_CFNumberGetValue) { int sz; CFNumberType t = num_type(ARG(1), &sz); uint8_t v[8] = {0}; Boolean ok = CFNumberGetValue(IN(0), t, v); memcpy(GP(ARG(2)), v, (size_t)sz); RET(ok); }
IMP(_CFDataCreate) { RET(cf_own(CFDataCreate(0, GP(ARG(1)), (int32_t)ARG(2)))); }
IMP(_CFDataCreateMutable) { RET(cf_own(CFDataCreateMutable(0, (int32_t)ARG(1)))); }
IMP(_CFDataCreateMutableCopy) { RET(cf_own(CFDataCreateMutableCopy(0, (int32_t)ARG(1), IN(2)))); }
IMP(_CFDataAppendBytes) { CFDataAppendBytes((CFMutableDataRef)IN(0), GP(ARG(1)), (int32_t)ARG(2)); RETV(); }
IMP(_CFDataGetLength) { RET(CFDataGetLength(IN(0))); }
IMP(_CFDataGetBytes) { CFDataGetBytes(IN(0), CFRangeMake((int32_t)ARG(1), (int32_t)ARG(2)), GP(ARG(3))); RETV(); }
/* CFDataGetBytePtr: the bytes must be in guest memory; a copy is kept per data object (read-only use) */
IMP(_CFDataGetBytePtr)
{
    CFDataRef d = IN(0); CFIndex n = CFDataGetLength(d);
    uint32_t g = g_malloc((uint32_t)n + 1); memcpy(GP(g), CFDataGetBytePtr(d), (size_t)n);
    RET(g);                                                  /* (leaks one copy per call: rare, small) */
}

/* ---- arrays and dictionaries (values are guest handles in, host objects inside) ---- */
static const CFArrayCallBacks *acb(uint32_t g) { return g == cb_array && g ? &kCFTypeArrayCallBacks : 0; }
IMP(_CFArrayCreate)
{
    uint32_t vals = ARG(1), n = ARG(2); const void **v = calloc(n + 1, sizeof *v);
    for (uint32_t k = 0; k < n; k++) v[k] = cf_in(rt_r32(G_MEM, vals + 4 * k));
    CFArrayRef a = CFArrayCreate(0, v, n, acb(ARG(3)) ? &kCFTypeArrayCallBacks : &kCFTypeArrayCallBacks); free(v);
    RET(cf_own(a));
}
IMP(_CFArrayCreateMutable) { RET(cf_own(CFArrayCreateMutable(0, (int32_t)ARG(1), &kCFTypeArrayCallBacks))); }
IMP(_CFArrayAppendValue) { CFArrayAppendValue((CFMutableArrayRef)IN(0), IN(1)); RETV(); }
IMP(_CFArrayGetCount) { CFTypeRef a = IN(0); RET(a ? CFArrayGetCount(a) : 0); }
IMP(_CFArrayGetValueAtIndex) { RET(cf_get(CFArrayGetValueAtIndex(IN(0), (int32_t)ARG(1)))); }
IMP(_CFArrayApplyFunction)   /* (array, range{loc,len}, applier(value, context), context) */
{
    CFArrayRef a = IN(0); int32_t loc = (int32_t)ARG(1), len = (int32_t)ARG(2); uint32_t fn = ARG(3), ctx = ARG(4);
    for (int32_t k = loc; k < loc + len; k++) { uint32_t args[2] = {cf_get(CFArrayGetValueAtIndex(a, k)), ctx}; g_call(c, fn, 2, args); }
    RETV();
}
IMP(_CFDictionaryCreate)     /* (alloc, keys*, values*, n, keyCallBacks, valueCallBacks) */
{
    uint32_t n = ARG(3); const void **k = calloc(n + 1, sizeof *k), **v = calloc(n + 1, sizeof *v);
    for (uint32_t i = 0; i < n; i++) { k[i] = cf_in(rt_r32(G_MEM, ARG(1) + 4 * i)); v[i] = cf_in(rt_r32(G_MEM, ARG(2) + 4 * i)); }
    CFDictionaryRef d = CFDictionaryCreate(0, k, v, n, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    free(k); free(v); RET(cf_own(d));
}
IMP(_CFDictionaryCreateMutable) { RET(cf_own(CFDictionaryCreateMutable(0, (int32_t)ARG(1), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks))); }
IMP(_CFDictionaryCreateMutableCopy) { RET(cf_own(CFDictionaryCreateMutableCopy(0, (int32_t)ARG(1), IN(2)))); }
IMP(_CFDictionaryGetValue) { CFTypeRef d = IN(0); RET(d ? cf_get(CFDictionaryGetValue(d, IN(1))) : 0); }
IMP(_CFDictionaryContainsKey) { CFTypeRef d = IN(0); RET(d && CFDictionaryContainsKey(d, IN(1))); }
IMP(_CFDictionaryGetCount) { CFTypeRef d = IN(0); RET(d ? CFDictionaryGetCount(d) : 0); }
IMP(_CFDictionarySetValue) { CFDictionarySetValue((CFMutableDictionaryRef)IN(0), IN(1), IN(2)); RETV(); }
IMP(_CFDictionaryAddValue) { CFDictionaryAddValue((CFMutableDictionaryRef)IN(0), IN(1), IN(2)); RETV(); }
IMP(_CFDictionaryGetKeysAndValues)
{
    CFDictionaryRef d = IN(0); CFIndex n = CFDictionaryGetCount(d);
    const void **k = calloc((size_t)n + 1, sizeof *k), **v = calloc((size_t)n + 1, sizeof *v);
    CFDictionaryGetKeysAndValues(d, k, v);
    for (CFIndex i = 0; i < n; i++) {
        if (ARG(1)) rt_w32(G_MEM, ARG(1) + 4 * (uint32_t)i, cf_get(k[i]));
        if (ARG(2)) rt_w32(G_MEM, ARG(2) + 4 * (uint32_t)i, cf_get(v[i]));
    }
    free(k); free(v); RETV();
}

/* ---- bundle: the Steam app (HomeworldRemastered.app), not this program ---- */
static CFBundleRef main_bundle(void)
{
    static CFBundleRef b;
    if (!b) {
        char app[1100]; snprintf(app, sizeof app, "%s/..", hw_game_dir);
        char real[1100]; if (!realpath(app, real)) strlcpy(real, app, sizeof real);
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)real, (CFIndex)strlen(real), true);
        b = CFBundleCreate(0, u); CFRelease(u);
        if (!b) hw_log("cannot open the game's bundle %s", real);
    }
    return b;
}
static CFStringRef bundle_id(void) { CFBundleRef b = main_bundle(); return b ? CFBundleGetIdentifier(b) : 0; }
IMP(_CFBundleGetMainBundle) { RET(cf_get(main_bundle())); }
IMP(_CFBundleCopyResourceURL) { RET(cf_own(CFBundleCopyResourceURL(IN(0), IN(1), IN(2), IN(3)))); }
IMP(_CFBundleCopyResourceURLForLocalization) { RET(cf_own(CFBundleCopyResourceURLForLocalization(IN(0), IN(1), IN(2), IN(3), IN(4)))); }
IMP(_CFBundleCopyResourcesDirectoryURL) { RET(cf_own(CFBundleCopyResourcesDirectoryURL(IN(0)))); }
IMP(_CFBundleCopyExecutableURL) { RET(cf_own(CFBundleCopyExecutableURL(IN(0)))); }
IMP(_CFBundleCopyLocalizedString) { RET(cf_own(CFBundleCopyLocalizedString(IN(0), IN(1), IN(2), IN(3)))); }
IMP(_CFBundleGetValueForInfoDictionaryKey) { RET(cf_get(CFBundleGetValueForInfoDictionaryKey(IN(0), IN(1)))); }

/* ---- preferences: the game's own domain (its bundle identifier) ---- */
static CFStringRef app_domain(uint32_t g)
{ return (!g || (pref_app && g == pref_app)) ? (bundle_id() ? bundle_id() : CFSTR("com.aspyr.homeworldremastered")) : cf_in(g); }
IMP(_CFPreferencesSetAppValue) { CFPreferencesSetAppValue(IN(0), IN(1), app_domain(ARG(2))); RETV(); }
IMP(_CFPreferencesAppSynchronize) { RET(CFPreferencesAppSynchronize(app_domain(ARG(0)))); }
IMP(_CFPreferencesGetAppBooleanValue)
{
    Boolean valid = 0, v = CFPreferencesGetAppBooleanValue(IN(0), app_domain(ARG(1)), &valid);
    if (ARG(2)) G_MEM[ARG(2)] = valid; RET(v);
}

/* ---- URLs ---- */
IMP(_CFURLCreateWithFileSystemPath) { RET(cf_own(CFURLCreateWithFileSystemPath(0, IN(1), (CFURLPathStyle)ARG(2), (Boolean)ARG(3)))); }
IMP(_CFURLCreateWithString) { RET(cf_own(CFURLCreateWithString(0, IN(1), IN(2)))); }
IMP(_CFURLCreateWithBytes) { RET(cf_own(CFURLCreateWithBytes(0, GP(ARG(1)), (int32_t)ARG(2), ARG(3), IN(4)))); }
IMP(_CFURLCreateCopyAppendingPathComponent) { RET(cf_own(CFURLCreateCopyAppendingPathComponent(0, IN(1), IN(2), (Boolean)ARG(3)))); }
IMP(_CFURLCopyFileSystemPath) { RET(cf_own(CFURLCopyFileSystemPath(IN(0), (CFURLPathStyle)ARG(1)))); }
IMP(_CFURLGetFileSystemRepresentation) { RET(CFURLGetFileSystemRepresentation(IN(0), (Boolean)ARG(1), GP(ARG(2)), (int32_t)ARG(3))); }
IMP(_CFURLCopyScheme) { RET(cf_own(CFURLCopyScheme(IN(0)))); }
IMP(_CFURLCopyHostName) { RET(cf_own(CFURLCopyHostName(IN(0)))); }
IMP(_CFURLCopyPath) { RET(cf_own(CFURLCopyPath(IN(0)))); }
IMP(_CFURLCopyUserName) { RET(cf_own(CFURLCopyUserName(IN(0)))); }
IMP(_CFURLCopyPassword) { RET(cf_own(CFURLCopyPassword(IN(0)))); }
IMP(_CFURLCopyFragment) { RET(cf_own(CFURLCopyFragment(IN(0), IN(1)))); }
IMP(_CFURLGetPortNumber) { RET(CFURLGetPortNumber(IN(0))); }
IMP(_CFURLCreateDataAndPropertiesFromResource)   /* (alloc, url, CFDataRef *data, CFDictionaryRef *props, desired, SInt32 *err) */
{
    CFURLRef u = IN(1); char p[PATH_MAX]; Boolean ok = 0;
    if (u && CFURLGetFileSystemRepresentation(u, true, (UInt8 *)p, sizeof p)) {
        FILE *f = fopen(p, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            uint8_t *b = malloc((size_t)n + 1); size_t r = fread(b, 1, (size_t)n, f); fclose(f);
            if (ARG(2)) rt_w32(G_MEM, ARG(2), cf_own(CFDataCreate(0, b, (CFIndex)r)));
            free(b); ok = 1;
        }
    }
    if (ARG(3)) rt_w32(G_MEM, ARG(3), 0);
    if (ARG(5)) rt_w32(G_MEM, ARG(5), ok ? 0 : (uint32_t)-10);   /* kCFURLResourceNotFoundError */
    RET(ok);
}

/* ---- streams (files; HTTP streams fail) ---- */
IMP(_CFReadStreamCreateWithFile) { RET(cf_own(CFReadStreamCreateWithFile(0, IN(1)))); }
IMP(_CFWriteStreamCreateWithFile) { RET(cf_own(CFWriteStreamCreateWithFile(0, IN(1)))); }
IMP(_CFReadStreamOpen) { RET(CFReadStreamOpen((CFReadStreamRef)IN(0))); }
IMP(_CFWriteStreamOpen) { RET(CFWriteStreamOpen((CFWriteStreamRef)IN(0))); }
IMP(_CFReadStreamClose) { CFReadStreamClose((CFReadStreamRef)IN(0)); RETV(); }
IMP(_CFWriteStreamClose) { CFWriteStreamClose((CFWriteStreamRef)IN(0)); RETV(); }
IMP(_CFReadStreamRead) { RET(CFReadStreamRead((CFReadStreamRef)IN(0), GP(ARG(1)), (int32_t)ARG(2))); }
IMP(_CFReadStreamCopyError) { RET(cf_own(CFReadStreamCopyError((CFReadStreamRef)IN(0)))); }
IMP(_CFReadStreamCopyProperty) { RET(cf_own(CFReadStreamCopyProperty((CFReadStreamRef)IN(0), IN(1)))); }
IMP(_CFReadStreamCreateForHTTPRequest) { RET(0); }
IMP(_CFHTTPMessageCreateRequest) { RET(0); }
IMP(_CFHTTPMessageSetBody) { RETV(); }
IMP(_CFHTTPMessageSetHeaderFieldValue) { RETV(); }
IMP(_CFHTTPMessageCopyAllHeaderFields) { RET(0); }
IMP(_CFHTTPMessageCopyHeaderFieldValue) { RET(0); }
IMP(_CFHTTPMessageGetResponseStatusCode) { RET(0); }

/* ---- property lists ---- */
IMP(_CFPropertyListCreateFromXMLData)        /* (alloc, data, options, CFStringRef *errorString) */
{
    CFErrorRef e = 0; CFPropertyListRef p = CFPropertyListCreateWithData(0, IN(1), ARG(2), 0, &e);
    if (e) CFRelease(e);
    if (ARG(3)) rt_w32(G_MEM, ARG(3), 0);
    RET(cf_own(p));
}
IMP(_CFPropertyListCreateWithStream)         /* (alloc, stream, length, options, format*, error*) */
{
    CFPropertyListFormat fmt = 0; CFErrorRef e = 0;
    CFPropertyListRef p = CFPropertyListCreateWithStream(0, (CFReadStreamRef)IN(1), (int32_t)ARG(2), ARG(3), &fmt, &e);
    if (ARG(4)) rt_w32(G_MEM, ARG(4), (uint32_t)fmt);
    if (ARG(5)) rt_w32(G_MEM, ARG(5), e ? cf_own(e) : 0); else if (e) CFRelease(e);
    RET(cf_own(p));
}
IMP(_CFPropertyListWrite)                    /* (plist, stream, format, options, error*) */
{
    CFErrorRef e = 0; CFIndex n = CFPropertyListWrite(IN(0), (CFWriteStreamRef)IN(1), (CFPropertyListFormat)ARG(2), ARG(3), &e);
    if (ARG(4)) rt_w32(G_MEM, ARG(4), e ? cf_own(e) : 0); else if (e) CFRelease(e);
    RET(n);
}

/* ---- run loop ---- */
IMP(_CFRunLoopGetCurrent) { RET(cf_get(CFRunLoopGetCurrent())); }
IMP(_CFRunLoopGetMain) { RET(cf_get(CFRunLoopGetMain())); }
IMP(_CFRunLoopAddSource) { RETV(); }
IMP(_CFRunLoopRemoveSource) { RETV(); }
IMP(_CFRunLoopRunInMode) { RET(CFRunLoopRunInMode(IN(0) ? IN(0) : kCFRunLoopDefaultMode, ARGD(1), (Boolean)ARG(3))); }

/* ---- time, dates, UUIDs ---- */
IMP(_CFAbsoluteTimeGetCurrent) { RETD(CFAbsoluteTimeGetCurrent()); }
IMP(_CFTimeZoneCopyDefault) { RET(cf_own(CFTimeZoneCopyDefault())); }
IMP(_CFTimeZoneGetSecondsFromGMT) { RETD(CFTimeZoneGetSecondsFromGMT(IN(0), ARGD(1))); }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
IMP(_CFAbsoluteTimeGetDayOfWeek) { RET(CFAbsoluteTimeGetDayOfWeek(ARGD(0), IN(2))); }
IMP(_CFAbsoluteTimeGetGregorianDate)         /* sret CFGregorianDate {SInt32 year; SInt8 month, day, hour, minute; double second} */
{
    double at = rt_rf64(G_MEM, c->esp + 8); CFTimeZoneRef tz = cf_in(rt_r32(G_MEM, c->esp + 16));
    CFGregorianDate d = CFAbsoluteTimeGetGregorianDate(at, tz);
    uint32_t o = ARG(0); rt_w32(G_MEM, o, (uint32_t)d.year); G_MEM[o + 4] = (uint8_t)d.month; G_MEM[o + 5] = (uint8_t)d.day;
    G_MEM[o + 6] = (uint8_t)d.hour; G_MEM[o + 7] = (uint8_t)d.minute; rt_wf64(G_MEM, o + 8, d.second);
    RET_SRET();
}
IMP(_CFAbsoluteTimeAddGregorianUnits)        /* (double at, tz, CFGregorianUnits {5 x SInt32, double seconds} by value) */
{
    double at = ARGD(0); CFTimeZoneRef tz = IN(2); uint32_t u = c->esp + 16;
    CFGregorianUnits g = {(SInt32)rt_r32(G_MEM, u), (SInt32)rt_r32(G_MEM, u + 4), (SInt32)rt_r32(G_MEM, u + 8),
                          (SInt32)rt_r32(G_MEM, u + 12), (SInt32)rt_r32(G_MEM, u + 16), rt_rf64(G_MEM, u + 20)};
    RETD(CFAbsoluteTimeAddGregorianUnits(at, tz, g));
}
#pragma clang diagnostic pop
IMP(_CFDateGetAbsoluteTime) { RETD(CFDateGetAbsoluteTime(IN(0))); }
IMP(_CFLocaleCreate) { RET(cf_own(CFLocaleCreate(0, IN(1)))); }
IMP(_CFDateFormatterCreate) { RET(cf_own(CFDateFormatterCreate(0, IN(1), (CFDateFormatterStyle)ARG(2), (CFDateFormatterStyle)ARG(3)))); }
IMP(_CFDateFormatterSetFormat) { CFDateFormatterSetFormat((CFDateFormatterRef)IN(0), IN(1)); RETV(); }
IMP(_CFDateFormatterCreateDateFromString)    /* (alloc, formatter, string, CFRange *rangep) */
{
    CFRange r, *rp = 0;
    if (ARG(3)) { r = CFRangeMake((int32_t)rt_r32(G_MEM, ARG(3)), (int32_t)rt_r32(G_MEM, ARG(3) + 4)); rp = &r; }
    CFDateRef d = CFDateFormatterCreateDateFromString(0, (CFDateFormatterRef)IN(1), IN(2), rp);
    if (rp) { rt_w32(G_MEM, ARG(3), (uint32_t)r.location); rt_w32(G_MEM, ARG(3) + 4, (uint32_t)r.length); }
    RET(cf_own(d));
}
IMP(_CFUUIDCreate) { RET(cf_own(CFUUIDCreate(0))); }
IMP(_CFUUIDGetUUIDBytes) { CFUUIDBytes b = CFUUIDGetUUIDBytes(cf_in(rt_r32(G_MEM, c->esp + 8))); memcpy(GP(ARG(0)), &b, 16); RET_SRET(); }
IMP(_CFUUIDGetConstantUUIDWithBytes)
{
    uint8_t b[16]; for (int k = 0; k < 16; k++) b[k] = (uint8_t)ARG(1 + k);
    RET(cf_get(CFUUIDGetConstantUUIDWithBytes(0, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15])));
}
