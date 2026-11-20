/* A small Objective-C bridge for the few places outside SDL where Aspyr's code sends messages directly
 * (ASL_AppMac's constructor: an autorelease pool and the OS version). The executable uses the legacy (ObjC 1) ABI:
 * class references hold class name strings and selector references hold selector name strings, so the loader turns
 * each class reference into a guest "class object" and objc_msgSend dispatches on the selector's name. A message
 * that is not served here stops with "[Class selector]". */
#include "mac.h"
#include "macho_tab.h"
#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
extern int hw_test_mode;
uint32_t cf_own(CFTypeRef o); CFTypeRef cf_in(uint32_t g); const char *hw_data_dir(void);

#define CLS_MAGIC 0x434c5353u                          /* class object: [0] magic, [4] name */
#define OBJ_MAGIC 0x4f424a49u                          /* instance: [0] magic, [4] class name */
void objc_fix_class_refs(void)
{
    for (uint32_t a = mo_clsrefs[0]; a < mo_clsrefs[0] + mo_clsrefs[1]; a += 4) {
        uint32_t nm = rt_r32(G_MEM, a); if (!nm) continue;
        char key[160]; snprintf(key, sizeof key, "objc class %s", GS(nm));
        uint32_t o = g_data(key, 16); rt_w32(G_MEM, o, CLS_MAGIC); rt_w32(G_MEM, o + 4, nm);
        rt_w32(G_MEM, a, o);
    }
}
static const char *cls_name(uint32_t r)
{
    if (!r) return "nil";
    uint32_t m = rt_r32(G_MEM, r);
    if (m == CLS_MAGIC || m == OBJ_MAGIC) return GS(rt_r32(G_MEM, r + 4));
    if (r >= 0xfc000000u && r < 0xfc080000u) return "(CoreFoundation object)";
    return "?";
}
static uint32_t new_obj(const char *cls)
{
    uint32_t o = g_calloc(16); rt_w32(G_MEM, o, OBJ_MAGIC); rt_w32(G_MEM, o + 4, g_strdup(cls)); return o;
}
static void unknown(Ctx *c, uint32_t r, uint32_t sel)
{
    static __thread char m[200];
    snprintf(m, sizeof m, "Objective-C message not bridged: [%s %s]", cls_name(r), GS(sel));
    rt_unhandled(c, rt_r32(G_MEM, c->esp), m);
}
IMP(_objc_msgSend)
{
    uint32_t r = ARG(0), sel = ARG(1); const char *s = GS(sel), *cl = cls_name(r);
    if (!r) RET(0);                                    /* messages to nil return 0 */
    int is_class = !(r >= 0xfc000000u && r < 0xfc080000u) && rt_r32(G_MEM, r) == CLS_MAGIC;
    if (is_class && (!strcmp(s, "alloc") || !strcmp(s, "new") || !strcmp(s, "processInfo"))) RET(new_obj(cl));
    if (!strcmp(s, "init") || !strcmp(s, "retain") || !strcmp(s, "autorelease")) RET(r);
    if (!strcmp(s, "release") || !strcmp(s, "drain")) RETV();
    if (is_class && !strcmp(s, "defaultManager")) RET(new_obj(cl));
    if (!strcmp(s, "URLForDirectory:inDomain:appropriateForURL:create:error:")) {
        /* NSSearchPathDirectory -> the port's data folder (as FSFindFolder) */
        uint32_t d = ARG(2); const char *sub = d == 14 ? "Application Support" : d == 13 ? "Caches" : d == 9 ? "Documents" : d == 5 ? "" : "Other";
        char p[1100]; snprintf(p, sizeof p, "%s/%s", hw_data_dir(), sub); mkdir(p, 0755);
        if (ARG(6)) rt_w32(G_MEM, ARG(6), 0);
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)p, (CFIndex)strlen(p), true);
        RET(cf_own(u));
    }
    if (!strcmp(s, "fileSystemRepresentation")) {
        CFTypeRef u = cf_in(r); char p[1100] = "";
        if (u && CFGetTypeID(u) == CFURLGetTypeID()) CFURLGetFileSystemRepresentation(u, true, (UInt8 *)p, sizeof p);
        RET(g_strdup(p));
    }
    if (is_class && !strcmp(cl, "NSCursor") && (!strcmp(s, "hide") || !strcmp(s, "unhide"))) {
        if (!hw_test_mode) SDL_ShowCursor(s[0] == 'u' ? SDL_ENABLE : SDL_DISABLE);
        RETV();
    }
    if (!strcmp(s, "respondsToSelector:")) {
        const char *q = GS(ARG(2));
        RET(!strcmp(q, "operatingSystemVersion") || !strcmp(q, "isOperatingSystemAtLeastVersion:"));
    }
    unknown(c, r, sel);
}
/* struct results: objc_msgSend_stret(result*, self, sel, ...) pops the result pointer like any struct return */
IMP(_objc_msgSend_stret)
{
    uint32_t out = ARG(0), r = ARG(1), sel = ARG(2); const char *s = GS(sel);
    if (!strcmp(s, "operatingSystemVersion")) {        /* NSOperatingSystemVersion {major, minor, patch}: reported 10.15.7 */
        rt_w32(G_MEM, out, 10); rt_w32(G_MEM, out + 4, 15); rt_w32(G_MEM, out + 8, 7); RET_SRET();
    }
    if (!r) { memset(GP(out), 0, 16); RET_SRET(); }
    static __thread char m[200]; snprintf(m, sizeof m, "Objective-C struct message not bridged: [%s %s]", cls_name(r), s);
    rt_unhandled(c, rt_r32(G_MEM, c->esp), m);
}
IMP(_objc_msgSend_fpret) { if (!ARG(0)) RETD(0); unknown(c, ARG(0), ARG(1)); }
IMP(_objc_sync_enter) { RET(0); }
IMP(_objc_sync_exit) { RET(0); }
