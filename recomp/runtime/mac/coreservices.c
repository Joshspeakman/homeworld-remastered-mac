/* CoreServices / Carbon File Manager for Aspyr's file layer (CreateFile_core, FindFirstFile...), plus the small
 * Carbon, Foundation and Process Manager calls the game makes.
 *
 * An FSRef (80 bytes, opaque) holds a magic word and an index into a table of host paths. Forks are host file
 * descriptors behind 16-bit reference numbers. Folder lookups (FSFindFolder) go to the port's data folder
 * (HW_DATA, default ~/Games/HomeworldNative/userdata), so saves and settings live with the port. */
#include "mac.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <mach/mach_time.h>

enum { fnfErr = -43, eofErr = -39, ioErr = -36, paramErr = -50, dupFNErr = -48, wrPermErr = -61, errFSNoMoreItems = -1417,
       rfNumErr = -51, posErr = -40, dirFulErr = -33 };
#define REF_MAGIC 0x48575246u                          /* 'HWRF' */

static char **paths; static uint32_t npaths, cappaths;
static pthread_mutex_t fs_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t path_id(const char *p)
{
    pthread_mutex_lock(&fs_lock);
    for (uint32_t k = 0; k < npaths; k++) if (!strcmp(paths[k], p)) { pthread_mutex_unlock(&fs_lock); return k; }
    if (npaths == cappaths) { cappaths = cappaths ? cappaths * 2 : 256; paths = realloc(paths, cappaths * sizeof *paths); }
    paths[npaths] = strdup(p); uint32_t k = npaths++;
    pthread_mutex_unlock(&fs_lock); return k;
}
static void ref_set(uint32_t ref, const char *p) { memset(GP(ref), 0, 80); rt_w32(G_MEM, ref, REF_MAGIC); rt_w32(G_MEM, ref + 4, path_id(p)); }
static const char *ref_path(uint32_t ref)
{
    if (!ref || rt_r32(G_MEM, ref) != REF_MAGIC) return 0;
    uint32_t k = rt_r32(G_MEM, ref + 4); return k < npaths ? paths[k] : 0;
}
const char *hw_fsref_path(uint32_t ref) { return ref_path(ref); }
void hw_fsref_make(uint32_t ref, const char *p) { ref_set(ref, p); }

/* the port's data folder */
const char *hw_data_dir(void)
{
    static char d[1024];
    if (!d[0]) {
        const char *e = getenv("HW_DATA");
        if (e && *e) strlcpy(d, e, sizeof d); else snprintf(d, sizeof d, "%s/Games/HomeworldNative/userdata", getenv("HOME"));
        char cmd[1100]; snprintf(cmd, sizeof cmd, "%s", d);
        for (char *p = cmd + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(cmd, 0755); *p = '/'; }
        mkdir(cmd, 0755);
        /* the profile folders Aspyr's launcher creates before it starts the game */
        static const char *sub[] = {"Application Support", "Application Support/Homeworld Remastered Collection",
                                    "Application Support/Homeworld Remastered Collection/RM",
                                    "Application Support/Homeworld Remastered Collection/HW1Classic",
                                    "Application Support/Homeworld Remastered Collection/HW2Classic", "Preferences", 0};
        for (int k = 0; sub[k]; k++) { snprintf(cmd, sizeof cmd, "%s/%s", d, sub[k]); mkdir(cmd, 0755); }
    }
    return d;
}
static void mkpath(const char *p)
{
    char b[1024]; strlcpy(b, p, sizeof b);
    for (char *q = b + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(b, 0755); *q = '/'; }
    mkdir(b, 0755);
}

IMP(_FSPathMakeRef)          /* (const UInt8 *path, FSRef *ref, Boolean *isDirectory) */
{
    struct stat st; const char *p = GS(ARG(0));
    if (stat(p, &st)) { if (getenv("HW_LOG_FILES")) hw_log("FSPathMakeRef(%s): not found", p); RET(fnfErr); }
    if (ARG(1)) ref_set(ARG(1), p);
    if (ARG(2)) G_MEM[ARG(2)] = S_ISDIR(st.st_mode);
    RET(noErr);
}
IMP(_FSRefMakePath)          /* (ref, UInt8 *path, UInt32 max) */
{
    const char *p = ref_path(ARG(0)); if (!p) RET(fnfErr);
    if (strlen(p) + 1 > ARG(2)) RET(-2110);
    strcpy(GP(ARG(1)), p); RET(noErr);
}
static void put_hfs_name(uint32_t out, const char *name)      /* HFSUniStr255 from UTF-8 */
{
    CFStringRef s = CFStringCreateWithCString(0, name, kCFStringEncodingUTF8);
    CFIndex n = s ? CFStringGetLength(s) : 0; if (n > 255) n = 255;
    rt_w16(G_MEM, out, (uint32_t)n);
    if (s) { CFStringGetCharacters(s, CFRangeMake(0, n), (UniChar *)GP(out + 2)); CFRelease(s); }
}
static void put_utc(uint32_t o, time_t t)               /* UTCDateTime: UInt16 high, UInt32 low (seconds since 1904), UInt16 fraction */
{
    uint64_t s = (uint64_t)t + 2082844800ull;
    rt_w16(G_MEM, o, (uint32_t)(s >> 32)); rt_w32(G_MEM, o + 2, (uint32_t)s); rt_w16(G_MEM, o + 6, 0);
}
/* FSCatalogInfo (i386, 144 bytes): nodeFlags @0, volume @2, parentDirID @4, nodeID @8, dates @16.., permissions @56,
 * finderInfo @72, dataLogicalSize @104, dataPhysicalSize @112, rsrc sizes @120/@128, valence @136 */
static void put_catalog(uint32_t info, const char *p, const struct stat *st)
{
    memset(GP(info), 0, 144);
    rt_w16(G_MEM, info, S_ISDIR(st->st_mode) ? 0x10 : 0);
    rt_w16(G_MEM, info + 2, (uint32_t)-100);
    rt_w32(G_MEM, info + 8, (uint32_t)st->st_ino);
    put_utc(info + 16, st->st_birthtime); put_utc(info + 24, st->st_mtime); put_utc(info + 32, st->st_ctime);
    put_utc(info + 40, st->st_atime);
    rt_w32(G_MEM, info + 56, st->st_uid); rt_w32(G_MEM, info + 60, st->st_gid); rt_w32(G_MEM, info + 64, st->st_mode);
    rt_w64(G_MEM, info + 104, (uint64_t)st->st_size); rt_w64(G_MEM, info + 112, (uint64_t)st->st_blocks * 512);
    (void)p;
}
IMP(_FSGetCatalogInfo)       /* (ref, whichInfo, FSCatalogInfo *, HFSUniStr255 *name, FSSpec *, FSRef *parent) */
{
    const char *p = ref_path(ARG(0)); struct stat st;
    if (!p || stat(p, &st)) RET(fnfErr);
    if (ARG(2)) put_catalog(ARG(2), p, &st);
    const char *slash = strrchr(p, '/');
    if (ARG(3)) put_hfs_name(ARG(3), slash ? slash + 1 : p);
    if (ARG(5)) { char par[1024]; strlcpy(par, p, sizeof par); char *s = strrchr(par, '/'); if (s && s != par) *s = 0; else strcpy(par, "/"); ref_set(ARG(5), par); }
    RET(noErr);
}
IMP(_FSCreateFileUnicode)    /* (parentRef, nameLength, const UniChar *name, whichInfo, info, FSRef *newRef, FSSpec *) */
{
    const char *par = ref_path(ARG(0)); if (!par) RET(fnfErr);
    CFStringRef s = CFStringCreateWithCharacters(0, (const UniChar *)GP(ARG(2)), (CFIndex)ARG(1));
    char name[1024] = ""; if (s) { CFStringGetCString(s, name, sizeof name, kCFStringEncodingUTF8); CFRelease(s); }
    char p[2048]; snprintf(p, sizeof p, "%s/%s", par, name);
    int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) RET(errno == EEXIST ? dupFNErr : ioErr);
    close(fd);
    if (ARG(5)) ref_set(ARG(5), p);
    RET(noErr);
}
IMP(_FSGetDataForkName) { rt_w16(G_MEM, ARG(0), 0); RET(noErr); }

/* forks */
#define NFORK 1024
static int fork_fd[NFORK];
IMP(_FSOpenFork)             /* (ref, forkNameLength, forkName, SInt8 permissions, FSIORefNum *refNum) */
{
    const char *p = ref_path(ARG(0)); if (!p) RET(fnfErr);
    int perm = (int)(int8_t)ARG(3) & 3, fl = perm == 1 ? O_RDONLY : perm == 2 ? O_WRONLY : O_RDWR;   /* fsRdPerm 1, fsWrPerm 2, fsRdWrPerm 3 */
    if (perm == 0) fl = O_RDWR;
    int fd = open(p, fl);
    if (fd < 0 && fl == O_RDWR && perm == 0) fd = open(p, O_RDONLY);
    if (fd < 0) RET(errno == ENOENT ? fnfErr : errno == EACCES ? wrPermErr : ioErr);
    pthread_mutex_lock(&fs_lock);
    int k = 1; while (k < NFORK && fork_fd[k]) k++;
    if (k == NFORK) { pthread_mutex_unlock(&fs_lock); close(fd); RET(-42); }
    fork_fd[k] = fd + 1; pthread_mutex_unlock(&fs_lock);
    rt_w16(G_MEM, ARG(4), (uint32_t)k); RET(noErr);
}
static int fdof(uint32_t ref) { int k = (int)(int16_t)ref; return k > 0 && k < NFORK ? fork_fd[k] - 1 : -1; }
static off_t seek_to(int fd, uint32_t mode, int64_t off)
{
    switch (mode & 3) {
    case 1: return lseek(fd, off, SEEK_SET);
    case 2: return lseek(fd, off, SEEK_END);
    case 3: return lseek(fd, off, SEEK_CUR);
    }
    return lseek(fd, 0, SEEK_CUR);                     /* fsAtMark */
}
IMP(_FSReadFork)             /* (refNum, UInt16 positionMode, SInt64 offset, ByteCount count, void *buf, ByteCount *actual) */
{
    int fd = fdof(ARG(0)); if (fd < 0) RET(rfNumErr);
    if (seek_to(fd, ARG(1), (int64_t)ARG64(2)) < 0) RET(posErr);
    uint32_t want = ARG(4); ssize_t r = read(fd, GP(ARG(5)), want);
    if (ARG(6)) rt_w32(G_MEM, ARG(6), r > 0 ? (uint32_t)r : 0);
    if (r < 0) RET(ioErr);
    RET((uint32_t)r < want ? eofErr : noErr);
}
IMP(_FSWriteFork)
{
    int fd = fdof(ARG(0)); if (fd < 0) RET(rfNumErr);
    if (seek_to(fd, ARG(1), (int64_t)ARG64(2)) < 0) RET(posErr);
    ssize_t r = write(fd, GP(ARG(5)), ARG(4));
    if (ARG(6)) rt_w32(G_MEM, ARG(6), r > 0 ? (uint32_t)r : 0);
    RET(r < 0 ? ioErr : noErr);
}
IMP(_FSCloseFork)
{
    int k = (int)(int16_t)ARG(0); if (k <= 0 || k >= NFORK || !fork_fd[k]) RET(rfNumErr);
    close(fork_fd[k] - 1); fork_fd[k] = 0; RET(noErr);
}
IMP(_FSGetForkSize) { int fd = fdof(ARG(0)); struct stat st; if (fd < 0 || fstat(fd, &st)) RET(rfNumErr); rt_w64(G_MEM, ARG(1), (uint64_t)st.st_size); RET(noErr); }
IMP(_FSSetForkSize)          /* (refNum, positionMode, SInt64 offset) */
{
    int fd = fdof(ARG(0)); if (fd < 0) RET(rfNumErr);
    off_t at = seek_to(fd, ARG(1), (int64_t)ARG64(2));
    RET(at < 0 || ftruncate(fd, at) ? ioErr : noErr);
}
/* PBReadForkAsync / PBWriteForkAsync (FSForkIOParam, i386): ioCompletion @12, ioResult @16 (SInt16), forkRefNum @24,
 * buffer @32, requestCount @36, actualCount @40, positionMode @44 (UInt16), positionOffset @46 (SInt64). Done at
 * once; the completion routine is called with the parameter block. */
static void fork_pb(Ctx *c, uint32_t pb, int write_)
{
    int fd = fdof(rt_r16(G_MEM, pb + 24)); int16_t res = noErr; ssize_t r = 0;
    if (fd < 0) res = rfNumErr;
    else if (seek_to(fd, rt_r16(G_MEM, pb + 44), (int64_t)rt_r64(G_MEM, pb + 46)) < 0) res = posErr;
    else {
        uint32_t n = rt_r32(G_MEM, pb + 36);
        r = write_ ? write(fd, GP(rt_r32(G_MEM, pb + 32)), n) : read(fd, GP(rt_r32(G_MEM, pb + 32)), n);
        if (r < 0) { res = ioErr; r = 0; } else if (!write_ && (uint32_t)r < n) res = eofErr;
    }
    rt_w32(G_MEM, pb + 40, (uint32_t)r); rt_w16(G_MEM, pb + 16, (uint32_t)(uint16_t)res);
    uint32_t done = rt_r32(G_MEM, pb + 12);
    if (done) g_call(c, done, 1, &pb);
}
IMP(_PBReadForkAsync) { fork_pb(c, ARG(0), 0); RETV(); }
IMP(_PBWriteForkAsync) { fork_pb(c, ARG(0), 1); RETV(); }

/* directory iteration: a snapshot of the entries */
typedef struct It { char *dir; char **names; uint32_t n, pos; } It;
static It *its[256];
IMP(_FSOpenIterator)         /* (containerRef, options, FSIterator *iterator) */
{
    const char *p = ref_path(ARG(0)); if (!p) RET(fnfErr);
    DIR *d = opendir(p); if (!d) RET(fnfErr);
    It *it = calloc(1, sizeof *it); it->dir = strdup(p);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        it->names = realloc(it->names, (it->n + 1) * sizeof *it->names); it->names[it->n++] = strdup(e->d_name);
    }
    closedir(d);
    int k = 1; while (k < 256 && its[k]) k++;
    if (k == 256) { RET(-108); }
    its[k] = it; rt_w32(G_MEM, ARG(2), (uint32_t)k); RET(noErr);
}
IMP(_FSCloseIterator)
{
    uint32_t k = ARG(0); if (!k || k >= 256 || !its[k]) RET(paramErr);
    for (uint32_t i = 0; i < its[k]->n; i++) free(its[k]->names[i]);
    free(its[k]->names); free(its[k]->dir); free(its[k]); its[k] = 0; RET(noErr);
}
/* (iterator, maximumObjects, ItemCount *actual, Boolean *changed, whichInfo, FSCatalogInfo *infos, FSRef *refs,
 *  FSSpec *specs, HFSUniStr255 *names) */
IMP(_FSGetCatalogInfoBulk)
{
    uint32_t k = ARG(0); It *it = k && k < 256 ? its[k] : 0; if (!it) RET(paramErr);
    uint32_t max = ARG(1), got = 0;
    while (got < max && it->pos < it->n) {
        char p[2048]; snprintf(p, sizeof p, "%s/%s", it->dir, it->names[it->pos++]);
        struct stat st; if (stat(p, &st)) continue;
        if (ARG(5)) put_catalog(ARG(5) + 144 * got, p, &st);
        if (ARG(6)) ref_set(ARG(6) + 80 * got, p);
        if (ARG(8)) put_hfs_name(ARG(8) + 512 * got, it->names[it->pos - 1]);
        got++;
    }
    if (ARG(2)) rt_w32(G_MEM, ARG(2), got);
    if (ARG(3)) G_MEM[ARG(3)] = 0;
    RET(got == 0 && it->pos >= it->n ? errFSNoMoreItems : noErr);
}
IMP(_FSGetVolumeInfo)        /* (volume, index, FSVolumeRefNum *actual, whichInfo, FSVolumeInfo *info, HFSUniStr255 *name, FSRef *root) */
{
    struct statvfs v; statvfs(hw_data_dir(), &v);
    if (ARG(2)) rt_w16(G_MEM, ARG(2), (uint32_t)-100);
    if (ARG(4)) {
        uint32_t i = ARG(4); memset(GP(i), 0, 128);
        uint64_t total = (uint64_t)v.f_blocks * v.f_frsize, freeb = (uint64_t)v.f_bavail * v.f_frsize;
        rt_w32(G_MEM, i + 40, 4096);                          /* blockSize */
        rt_w32(G_MEM, i + 44, (uint32_t)(total / 4096 > UINT32_MAX ? UINT32_MAX : total / 4096));
        rt_w32(G_MEM, i + 48, (uint32_t)(freeb / 4096 > UINT32_MAX ? UINT32_MAX : freeb / 4096));
        rt_w64(G_MEM, i + 52, total); rt_w64(G_MEM, i + 60, freeb);
    }
    if (ARG(5)) put_hfs_name(ARG(5), "Macintosh HD");
    if (ARG(6)) ref_set(ARG(6), "/");
    RET(noErr);
}
/* FSFindFolder(vRefNum, OSType folderType, Boolean create, FSRef *found) */
IMP(_FSFindFolder)
{
    uint32_t t = ARG(1); const char *sub = 0; char p[1024];
    switch (t) {
    case 0x61737570: sub = "Application Support"; break;   /* 'asup' */
    case 0x70726566: sub = "Preferences"; break;           /* 'pref' */
    case 0x646f6373: sub = "Documents"; break;             /* 'docs' */
    case 0x63616368: sub = "Caches"; break;                /* 'cach' */
    case 0x74656d70: case 0x74656d71: { const char *tmp = getenv("TMPDIR"); snprintf(p, sizeof p, "%s", tmp ? tmp : "/tmp"); goto have; }
    case 0x64657363: sub = "Desktop"; break;               /* 'desk' */
    case 0x6375736b: sub = ""; break;                      /* 'cusr' (home) */
    default:
        hw_log("FSFindFolder('%c%c%c%c'): using the data folder", t >> 24, (t >> 16) & 255, (t >> 8) & 255, t & 255);
        sub = "Other";
    }
    snprintf(p, sizeof p, "%s/%s", hw_data_dir(), sub);
have:
    mkpath(p);
    ref_set(ARG(3), p); RET(noErr);
}

/* ---- Carbon / Process Manager / misc ---- */
IMP(_Gestalt)
{
    uint32_t sel = ARG(0), out = ARG(1), v;
    switch (sel) {
    case 0x73797376: v = 0x1095; break;       /* 'sysv': 10.9.5 as BCD */
    case 0x73797331: v = 10; break;           /* 'sys1' */
    case 0x73797332: v = 15; break;           /* 'sys2': reported as 10.15 */
    case 0x73797333: v = 7; break;            /* 'sys3' */
    case 0x70636c6b: v = 3200000000u; break;  /* 'pclk' */
    case 0x72616d20: v = 0x7fffffff; break;   /* 'ram ' */
    default: RET(-5551);                      /* gestaltUndefSelectorErr */
    }
    if (out) rt_w32(G_MEM, out, v); RET(noErr);
}
IMP(_Microseconds) { uint64_t us = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000; rt_w64(G_MEM, ARG(0), us); RETV(); }   /* UnsignedWide {lo, hi} on x86 */
IMP(_TickCount) { RET((uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 60 / 1000000000ull)); }
IMP(_DurationToNanoseconds)  /* Duration: > 0 milliseconds, < 0 microseconds */
{
    int32_t d = (int32_t)ARG(0); uint64_t ns = d > 0 ? (uint64_t)d * 1000000ull : (uint64_t)(-(int64_t)d) * 1000ull;
    RET64(ns);                                         /* UnsignedWide {lo, hi}: edx:eax */
}
IMP(_UpdateSystemActivity) { RET(0); }
IMP(_modff) { float ip; float r = modff(ARGF(0), &ip); rt_wf32(G_MEM, ARG(1), ip); RETD(r); }
IMP(_GetCurrentProcess) { rt_w32(G_MEM, ARG(0), 0); rt_w32(G_MEM, ARG(0) + 4, 2); RET(0); }   /* kCurrentProcess */
IMP(_GetProcessPID) { rt_w32(G_MEM, ARG(1), (uint32_t)getpid()); RET(0); }
IMP(_GetProcessBundleLocation)   /* (psn, FSRef *location): the game's app bundle */
{
    char app[1100], real[1100]; snprintf(app, sizeof app, "%s/..", hw_game_dir);
    if (!realpath(app, real)) strlcpy(real, app, sizeof real);
    ref_set(ARG(1), real); RET(0);
}
IMP(_CSIdentityQueryCreateForCurrentUser) { RET(0); }
IMP(_CSIdentityQueryExecute) { RET(0); }
IMP(_CSIdentityQueryCopyResults) { RET(0); }
IMP(_CSIdentityGetUUID) { RET(0); }
IMP(_FindNextComponent) { RET(0); }
IMP(_OpenAComponent) { RET(-2000); }
IMP(_CloseComponent) { RET(0); }
IMP(_SetSystemUIMode) { RET(0); }
IMP(_LMGetKbdType) { RET(0x28); }
IMP(_KBGetLayoutType) { RET(0); }                    /* kKeyboardANSI */

/* Foundation */
uint32_t cf_own(CFTypeRef o);
IMP(_NSTemporaryDirectory)
{
    const char *t = getenv("TMPDIR"); RET(cf_own(CFStringCreateWithCString(0, t ? t : "/tmp/", kCFStringEncodingUTF8)));
}
char *cf_cstr(CFTypeRef s); CFTypeRef cf_in(uint32_t g);
IMP(_NSLog) { char *s = cf_cstr(cf_in(ARG(0))); hw_log("NSLog: %s", s); free(s); RETV(); }
IMP(_NSEqualRects) { RET(!memcmp(GP(c->esp + 4), GP(c->esp + 20), 16)); }   /* two NSRects (4 floats) by value */
IMP(_NSPointInRect)      /* (NSPoint {x,y}, NSRect {x,y,w,h}) */
{
    float px = ARGF(0), py = ARGF(1), x = ARGF(2), y = ARGF(3), w = ARGF(4), h = ARGF(5);
    RET(px >= x && py >= y && px < x + w && py < y + h);
}
