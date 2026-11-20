/* SDL for the guest: the statically linked SDL 2.0 in the executable is replaced, function by function (OVR), where
 * it touches the operating system: initialisation, windows, displays, OpenGL contexts, events, mouse, keyboard,
 * game controllers, haptics and message boxes. These run on the host's SDL2. SDL's portable parts (surfaces, pixel
 * formats, timers, threads, strings, RWops) keep running as recompiled code.
 *
 * Guest SDL objects (SDL_Window *, SDL_GLContext, SDL_Cursor *, SDL_GameController *, ...) are small guest records
 * naming the host object. Events are pulled from the host, converted to the 32-bit layout (only events with pointers
 * differ), passed through the game's event filter, and queued on the guest side. */
#include "mac.h"
#include <SDL2/SDL.h>
#include "mgl/mgl.h"
extern int gl_metal;

#define H_WINDOW 0x57494e44u                           /* 'WIND' */
#define H_GLCTX  0x474c4354u                           /* 'GLCT' */
#define H_CURSOR 0x43555253u
#define H_CTRL   0x4354524cu
#define H_JOY    0x4a4f5953u
#define H_HAPTIC 0x48415054u
typedef struct Obj { uint32_t magic; void *host; } HObj;
static HObj objs[1024]; static int nobjs;
static pthread_mutex_t sdl_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t wrap(uint32_t magic, void *host)       /* guest handle: g_data record [magic, index] */
{
    if (!host) return 0;
    pthread_mutex_lock(&sdl_lock);
    for (int k = 0; k < nobjs; k++) if (objs[k].host == host && objs[k].magic == magic) {
        pthread_mutex_unlock(&sdl_lock); char key[48]; snprintf(key, sizeof key, "sdl object %d", k); return g_data(key, 64);
    }
    int k = nobjs < 1024 ? nobjs++ : 1023;
    objs[k].magic = magic; objs[k].host = host;
    pthread_mutex_unlock(&sdl_lock);
    char key[48]; snprintf(key, sizeof key, "sdl object %d", k);
    uint32_t g = g_data(key, 64); memset(GP(g), 0, 64); rt_w32(G_MEM, g, magic); rt_w32(G_MEM, g + 4, (uint32_t)k);
    return g;
}
static void *unwrap(uint32_t g, uint32_t magic)
{
    if (!g || g < 0xfe000000u || rt_r32(G_MEM, g) != magic) return 0;
    uint32_t k = rt_r32(G_MEM, g + 4); return k < 1024 ? objs[k].host : 0;
}
#define WIN(n) ((SDL_Window *)unwrap(ARG(n), H_WINDOW))
static uint32_t gwin(SDL_Window *w) { return wrap(H_WINDOW, w); }
static uint32_t gstr(const char *s)                    /* guest copy of a host string, cached per text */
{
    static struct { char *s; uint32_t g; } c[128]; static int n;
    if (!s) return 0;
    for (int k = 0; k < n; k++) if (!strcmp(c[k].s, s)) return c[k].g;
    uint32_t g = g_strdup(s); if (n < 128) { c[n].s = strdup(s); c[n].g = g; n++; }
    return g;
}

/* ---- initialisation ---- */
static uint32_t host_flags(uint32_t f) { return f & (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC); }
/* full screen is a macOS Space (what Game Mode needs): SDL's Spaces hint at override priority, whatever the game sets */
static void spaces_hint(void) { SDL_SetHintWithPriority("SDL_VIDEO_MAC_FULLSCREEN_SPACES", "1", SDL_HINT_OVERRIDE); }
OVR(SDL_Init) { spaces_hint(); int r = SDL_Init(host_flags(ARG(0))); if (r) hw_log("SDL_Init: %s", SDL_GetError()); RET(r); }
OVR(SDL_InitSubSystem) { RET(SDL_InitSubSystem(host_flags(ARG(0)))); }
OVR(SDL_QuitSubSystem) { SDL_QuitSubSystem(host_flags(ARG(0))); RETV(); }
OVR(SDL_WasInit) { RET(SDL_WasInit(ARG(0)) | (ARG(0) & SDL_INIT_TIMER)); }
OVR(SDL_Quit) { SDL_Quit(); RETV(); }
OVR(SDL_SetHint) { if (getenv("HW_LOG_FPS")) hw_log("SDL_SetHint(%s, %s)", GS(ARG(0)), GS(ARG(1))); RET(SDL_SetHint(GS(ARG(0)), GS(ARG(1)))); }
OVR(SDL_SetHintWithPriority) { RET(SDL_SetHintWithPriority(GS(ARG(0)), GS(ARG(1)), (SDL_HintPriority)ARG(2))); }
OVR(SDL_GetHint) { RET(gstr(SDL_GetHint(GS(ARG(0))))); }
OVR(SDL_GetCurrentVideoDriver) { RET(gstr(SDL_GetCurrentVideoDriver())); }

/* ---- displays ---- */
static void put_mode(uint32_t g, const SDL_DisplayMode *m)    /* SDL_DisplayMode (i386): format, w, h, refresh, driverdata */
{ rt_w32(G_MEM, g, m->format); rt_w32(G_MEM, g + 4, (uint32_t)m->w); rt_w32(G_MEM, g + 8, (uint32_t)m->h); rt_w32(G_MEM, g + 12, (uint32_t)m->refresh_rate); rt_w32(G_MEM, g + 16, 0); }
static void get_mode(uint32_t g, SDL_DisplayMode *m)
{ m->format = rt_r32(G_MEM, g); m->w = (int)rt_r32(G_MEM, g + 4); m->h = (int)rt_r32(G_MEM, g + 8); m->refresh_rate = (int)rt_r32(G_MEM, g + 12); m->driverdata = 0; }
OVR(SDL_GetNumVideoDisplays) { RET(SDL_GetNumVideoDisplays()); }
OVR(SDL_GetDisplayBounds) { SDL_Rect r; int k = SDL_GetDisplayBounds((int)ARG(0), &r); memcpy(GP(ARG(1)), &r, 16); RET(k); }
OVR(SDL_GetNumDisplayModes) { RET(SDL_GetNumDisplayModes((int)ARG(0))); }
OVR(SDL_GetDisplayMode) { SDL_DisplayMode m; int k = SDL_GetDisplayMode((int)ARG(0), (int)ARG(1), &m); if (!k) put_mode(ARG(2), &m); RET(k); }
OVR(SDL_GetDesktopDisplayMode) { SDL_DisplayMode m; int k = SDL_GetDesktopDisplayMode((int)ARG(0), &m); if (!k) put_mode(ARG(1), &m); RET(k); }
OVR(SDL_GetCurrentDisplayMode) { SDL_DisplayMode m; int k = SDL_GetCurrentDisplayMode((int)ARG(0), &m); if (!k) put_mode(ARG(1), &m); RET(k); }
OVR(SDL_GetWindowDisplayMode) { SDL_DisplayMode m; int k = SDL_GetWindowDisplayMode(WIN(0), &m); if (!k) put_mode(ARG(1), &m); RET(k); }
OVR(SDL_SetWindowDisplayMode) { if (!ARG(1)) RET(SDL_SetWindowDisplayMode(WIN(0), 0)); SDL_DisplayMode m; get_mode(ARG(1), &m); RET(SDL_SetWindowDisplayMode(WIN(0), &m)); }

/* ---- windows ---- */
typedef struct WData { SDL_Window *w; char name[48]; uint32_t value; } WData;
static WData wdata[256]; static int nwdata;
extern int hw_test_mode;                               /* HW_TEST=1: a background window that never takes focus */
OVR(SDL_CreateWindow)
{
    uint32_t flags = ARG(5);
    if (hw_test_mode) flags &= ~(uint32_t)(SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_INPUT_GRABBED);
    else if (!(flags & SDL_WINDOW_HIDDEN) && getenv("HW_FULLSCREEN"))   /* the launcher's choice: a window, then macOS full screen (notch.m) */
        flags &= ~(uint32_t)(SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!(flags & SDL_WINDOW_HIDDEN) && getenv("HW_RETINA") && atoi(getenv("HW_RETINA"))) flags |= SDL_WINDOW_ALLOW_HIGHDPI;   /* the launcher's Resolution: Retina */
    { extern void gl_bind(void); gl_bind(); }
    if (gl_metal && (flags & SDL_WINDOW_OPENGL)) flags = (flags & ~(uint32_t)SDL_WINDOW_OPENGL) | SDL_WINDOW_METAL;
    int cx = (int)ARG(1), cy = (int)ARG(2), cw = (int)ARG(3), ch = (int)ARG(4);
    if (!(flags & SDL_WINDOW_HIDDEN) && !hw_test_mode && getenv("HW_FULLSCREEN") && atoi(getenv("HW_FULLSCREEN"))) {
        /* made at its full-screen size, so entering full screen does not resize it (at Retina the game keeps the size it started with) */
        extern void hw_fullscreen_size(int *, int *); hw_fullscreen_size(&cw, &ch); cx = cy = 0;
        extern int hw_borderless_around(void); if (hw_borderless_around()) flags |= SDL_WINDOW_BORDERLESS;   /* around the notch (notch.m) */
    }
    SDL_Window *w = SDL_CreateWindow(GS(ARG(0)), cx, cy, cw, ch, flags);
    if (!w) hw_log("SDL_CreateWindow(%s, %dx%d, %x): %s", GS(ARG(0)), (int)ARG(3), (int)ARG(4), flags, SDL_GetError());
    else { hw_log("window %dx%d flags %x", cw, ch, flags); if (!(flags & SDL_WINDOW_HIDDEN)) { extern void hw_window_created(SDL_Window *); hw_window_created(w); if (gl_metal) mgl_window_created(w);
            if (!hw_test_mode && getenv("HW_FULLSCREEN") && atoi(getenv("HW_FULLSCREEN"))) { extern void hw_enter_fullscreen(SDL_Window *); hw_enter_fullscreen(w); } } }
    RET(gwin(w));
}
OVR(SDL_DestroyWindow) { SDL_Window *w = WIN(0); if (w) SDL_DestroyWindow(w); RETV(); }
OVR(SDL_GetWindowFlags) { SDL_Window *w = WIN(0); RET(w ? SDL_GetWindowFlags(w) : 0); }
OVR(SDL_GetWindowID) { SDL_Window *w = WIN(0); RET(w ? SDL_GetWindowID(w) : 0); }
OVR(SDL_GetWindowFromID) { RET(gwin(SDL_GetWindowFromID(ARG(0)))); }
OVR(SDL_GetWindowTitle) { SDL_Window *w = WIN(0); RET(gstr(w ? SDL_GetWindowTitle(w) : "")); }
OVR(SDL_SetWindowTitle) { SDL_Window *w = WIN(0); if (w) SDL_SetWindowTitle(w, GS(ARG(1))); RETV(); }
OVR(SDL_GetWindowPosition) { int x = 0, y = 0; SDL_Window *w = WIN(0); if (w) SDL_GetWindowPosition(w, &x, &y); if (ARG(1)) rt_w32(G_MEM, ARG(1), (uint32_t)x); if (ARG(2)) rt_w32(G_MEM, ARG(2), (uint32_t)y); RETV(); }
OVR(SDL_SetWindowPosition) { SDL_Window *w = WIN(0); extern int hw_borderless_around(void); if (getenv("HW_LOG_FPS")) hw_log("SDL_SetWindowPosition(%d, %d)", (int)ARG(1), (int)ARG(2)); if (hw_borderless_around()) RETV(); if (w) SDL_SetWindowPosition(w, (int)ARG(1), (int)ARG(2)); RETV(); }
OVR(SDL_GetWindowSize) { int x = 0, y = 0; SDL_Window *w = WIN(0); if (w) SDL_GetWindowSize(w, &x, &y); if (ARG(1)) rt_w32(G_MEM, ARG(1), (uint32_t)x); if (ARG(2)) rt_w32(G_MEM, ARG(2), (uint32_t)y); RETV(); }
OVR(SDL_SetWindowSize)
{
    SDL_Window *w = WIN(0); extern int hw_borderless_around(void);
    if (getenv("HW_LOG_FPS")) hw_log("SDL_SetWindowSize(%d, %d)%s", (int)ARG(1), (int)ARG(2), hw_borderless_around() ? ": kept screen-sized" : "");
    if (hw_borderless_around()) RETV();              /* around the notch the window stays the screen's size (notch.m) */
    if (w) SDL_SetWindowSize(w, (int)ARG(1), (int)ARG(2)); RETV();
}
/* the launcher's Display choice wins over the game's own windowed / full-screen setting: full screen is macOS full
 * screen (a Space of its own, which turns on Game Mode), windowed a window */
OVR(SDL_SetWindowFullscreen)
{
    SDL_Window *w = WIN(0); uint32_t f = ARG(1); const char *want = getenv("HW_FULLSCREEN");
    if (want) { hw_log("SDL_SetWindowFullscreen(%x): the launcher's choice stands", f); RET(0); }   /* notch.m enters full screen */
    RET(w && !hw_test_mode ? SDL_SetWindowFullscreen(w, f) : 0);
}
OVR(SDL_ShowWindow) { SDL_Window *w = WIN(0); if (w) SDL_ShowWindow(w); RETV(); }
OVR(SDL_HideWindow) { SDL_Window *w = WIN(0); if (w) SDL_HideWindow(w); RETV(); }
OVR(SDL_RaiseWindow) { SDL_Window *w = WIN(0); if (w && !hw_test_mode) SDL_RaiseWindow(w); RETV(); }
OVR(SDL_GetWindowDisplayIndex) { SDL_Window *w = WIN(0); RET(w ? SDL_GetWindowDisplayIndex(w) : 0); }
OVR(SDL_SetWindowGrab) { SDL_Window *w = WIN(0); if (w && !hw_test_mode) SDL_SetWindowGrab(w, (SDL_bool)ARG(1)); RETV(); }
OVR(SDL_GetWindowGrab) { SDL_Window *w = WIN(0); RET(w ? SDL_GetWindowGrab(w) : 0); }
OVR(SDL_SetWindowBordered) { SDL_Window *w = WIN(0); if (w) SDL_SetWindowBordered(w, (SDL_bool)ARG(1)); RETV(); }
OVR(SDL_MinimizeWindow) { SDL_Window *w = WIN(0); if (w) SDL_MinimizeWindow(w); RETV(); }
OVR(SDL_RestoreWindow) { SDL_Window *w = WIN(0); if (w) SDL_RestoreWindow(w); RETV(); }
OVR(SDL_GetWindowWMInfo) { RET(0); }
static SDL_Window *main_window(void) { for (int k = 0; k < nobjs; k++) if (objs[k].magic == H_WINDOW && (SDL_GetWindowFlags(objs[k].host) & SDL_WINDOW_SHOWN)) return objs[k].host; return 0; }
OVR(SDL_GetKeyboardFocus) { SDL_Window *f = SDL_GetKeyboardFocus(); if (!f && hw_test_mode) f = main_window(); RET(gwin(f)); }
OVR(SDL_GetMouseFocus) { SDL_Window *f = SDL_GetMouseFocus(); if (!f && hw_test_mode) f = main_window(); RET(gwin(f)); }
OVR(SDL_SetWindowData)
{
    SDL_Window *w = WIN(0); const char *n = GS(ARG(1)); uint32_t old = 0;
    for (int k = 0; k < nwdata; k++) if (wdata[k].w == w && !strcmp(wdata[k].name, n)) { old = wdata[k].value; wdata[k].value = ARG(2); RET(old); }
    if (nwdata < 256) { wdata[nwdata].w = w; strlcpy(wdata[nwdata].name, n, 48); wdata[nwdata].value = ARG(2); nwdata++; }
    RET(0);
}
OVR(SDL_GetWindowData)
{
    SDL_Window *w = WIN(0); const char *n = GS(ARG(1));
    for (int k = 0; k < nwdata; k++) if (wdata[k].w == w && !strcmp(wdata[k].name, n)) RET(wdata[k].value);
    RET(0);
}

/* ---- OpenGL (Apple's OpenGL through SDL for now; the Metal backend replaces the context and the entry points) ---- */
void gl_bind(void);
OVR(SDL_GL_SetAttribute) { if (gl_metal) RET(0); RET(SDL_GL_SetAttribute((SDL_GLattr)ARG(0), (int)ARG(1))); }
OVR(SDL_GL_GetAttribute) { if (gl_metal) { int a = (int)ARG(0); rt_w32(G_MEM, ARG(1), a == SDL_GL_CONTEXT_MAJOR_VERSION ? 4 : a == SDL_GL_CONTEXT_MINOR_VERSION ? 1 : a == SDL_GL_DEPTH_SIZE ? 24 : a == SDL_GL_STENCIL_SIZE ? 8 : a == SDL_GL_DOUBLEBUFFER ? 1 : a == SDL_GL_RED_SIZE || a == SDL_GL_GREEN_SIZE || a == SDL_GL_BLUE_SIZE || a == SDL_GL_ALPHA_SIZE ? 8 : a == SDL_GL_CONTEXT_PROFILE_MASK ? SDL_GL_CONTEXT_PROFILE_CORE : 0); RET(0); } int v = 0; int r = SDL_GL_GetAttribute((SDL_GLattr)ARG(0), &v); rt_w32(G_MEM, ARG(1), (uint32_t)v); RET(r); }
OVR(SDL_GL_CreateContext)
{
    if (gl_metal) { RET(wrap(H_GLCTX, mgl_create_context(WIN(0)))); }
    SDL_GLContext g = SDL_GL_CreateContext(WIN(0));
    if (!g) hw_log("SDL_GL_CreateContext: %s", SDL_GetError());
    gl_bind(); RET(wrap(H_GLCTX, g));
}
OVR(SDL_GL_DeleteContext) { SDL_GLContext g = unwrap(ARG(0), H_GLCTX); if (gl_metal) RETV(); if (g) SDL_GL_DeleteContext(g); RETV(); }
static uint32_t cur_ctx, cur_win;
OVR(SDL_GL_MakeCurrent) { cur_win = ARG(0); cur_ctx = ARG(1); if (gl_metal) RET(mgl_make_current(WIN(0), 0)); RET(SDL_GL_MakeCurrent(WIN(0), unwrap(ARG(1), H_GLCTX))); }
OVR(SDL_GL_GetCurrentContext) { if (gl_metal) RET(cur_ctx); RET(wrap(H_GLCTX, SDL_GL_GetCurrentContext())); }
OVR(SDL_GL_GetCurrentWindow) { if (gl_metal) RET(cur_win); RET(gwin(SDL_GL_GetCurrentWindow())); }
static int swap_interval;
OVR(SDL_GL_SetSwapInterval) { swap_interval = (int)ARG(0); if (getenv("HW_VSYNC")) swap_interval = atoi(getenv("HW_VSYNC")); if (gl_metal) { mgl_set_swap_interval(hw_test_mode ? 0 : swap_interval); RET(0); } RET(SDL_GL_SetSwapInterval(swap_interval)); }
OVR(SDL_GL_GetSwapInterval) { if (gl_metal) RET(swap_interval); RET(SDL_GL_GetSwapInterval()); }
OVR(SDL_GL_GetDrawableSize) { int x = 0, y = 0; if (gl_metal) mgl_drawable_size(WIN(0), &x, &y); else SDL_GL_GetDrawableSize(WIN(0), &x, &y); if (ARG(1)) rt_w32(G_MEM, ARG(1), (uint32_t)x); if (ARG(2)) rt_w32(G_MEM, ARG(2), (uint32_t)y); RETV(); }
OVR(SDL_GL_ExtensionSupported) { if (gl_metal) { const char *n = GS(ARG(0)); const unsigned char *(*gs)(unsigned, unsigned) = mgl_lookup("glGetStringi"); for (unsigned k = 0; ; k++) { const unsigned char *e = gs(0x1F03, k); if (!e) RET(0); if (!strcmp((const char *)e, n)) RET(1); } } RET(SDL_GL_ExtensionSupported(GS(ARG(0)))); }
OVR(SDL_GL_LoadLibrary) { RET(0); }
OVR(SDL_GL_GetProcAddress)
{
    const char *n = GS(ARG(0));
    extern int hw_has_impl(const char *sym);
    if (!hw_has_impl(n)) { if (getenv("HW_LOG_GL")) hw_log("GL entry point not provided: %s", n); RET(0); }
    RET(g_thunk(n));
}
extern void hw_frame_done(void); extern unsigned long long hw_frames(void); void hw_shot_maybe(SDL_Window *w, unsigned long long frame);
void hw_script_step(SDL_Window *w, unsigned long long frame);
extern unsigned long long hw_ms(void);
/* Gearbox's online service client (_SparkInterface, 0x6d28c0) never logs in offline (no Steam auth ticket) and stays
 * "not started" (state +0x100c = 0); on quit the game then waits 10 s for a telemetry flush that never happens.
 * State 9 is what a failed connection leaves (the quit path and LeviathanRaiseEvent skip their work for it): set it
 * once the game runs, unless HW_SPARK=1. */
#define SPARK_STATE 0x6d38ccu
static void spark_offline(void)
{
    static int done; if (done || getenv("HW_SPARK")) return;
    uint32_t v = rt_r32(G_MEM, SPARK_STATE);
    if (v == 0 || v == 9) { rt_w32(G_MEM, SPARK_STATE, 9); done = 1; hw_log("Gearbox online services: offline"); }
}
OVR(SDL_GL_SwapWindow) { static int nswap; if (++nswap == 120) spark_offline(); if (nswap == 900 && getenv("HW_LOG_FPS")) { extern void hw_window_report(SDL_Window *); hw_window_report(WIN(0)); } hw_shot_maybe(WIN(0), hw_ms()); hw_script_step(WIN(0), hw_ms()); if (gl_metal) mgl_swap(WIN(0)); else SDL_GL_SwapWindow(WIN(0)); hw_frame_done(); RETV(); }

/* ---- events ---- */
#define QN 1024
static uint8_t queue[QN][56]; static int qhead, qtail;     /* guest-layout events */
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t filter_fn, filter_data;
static uint32_t watch[16][2]; static int nwatch;            /* SDL_AddEventWatch: (callback, userdata) in guest memory */
static int q_count(void) { return (qtail - qhead + QN) % QN; }
static void q_push(const uint8_t *ev) { if (q_count() < QN - 1) { memcpy(queue[qtail], ev, 56); qtail = (qtail + 1) % QN; } }
static void to_guest(const SDL_Event *e, uint8_t *g)
{
    memset(g, 0, 56); memcpy(g, e, 56);
    if (e->type == SDL_DROPFILE || e->type == SDL_DROPTEXT) {
        uint32_t f = e->drop.file ? g_strdup(e->drop.file) : 0; SDL_free(e->drop.file);
        memset(g + 8, 0, 48); memcpy(g + 8, &f, 4);
    } else if (e->type >= SDL_USEREVENT) {
        uint32_t d1 = (uint32_t)(uintptr_t)e->user.data1, d2 = (uint32_t)(uintptr_t)e->user.data2;
        memcpy(g + 16, &d1, 4); memcpy(g + 20, &d2, 4); memset(g + 24, 0, 32);
    }
}
static void from_guest(const uint8_t *g, SDL_Event *e)
{
    memcpy(e, g, 56);
    if (e->type >= SDL_USEREVENT) { uint32_t d1, d2; memcpy(&d1, g + 16, 4); memcpy(&d2, g + 20, 4); e->user.data1 = (void *)(uintptr_t)d1; e->user.data2 = (void *)(uintptr_t)d2; }
}
extern void *objc_autoreleasePoolPush(void); extern void objc_autoreleasePoolPop(void *);
static void pump_events(Ctx *c);
static void pump(Ctx *c) { void *pool = objc_autoreleasePoolPush(); pump_events(c); objc_autoreleasePoolPop(pool); }   /* AppKit's autoreleased objects, per pump */
static void pump_events(Ctx *c)
{
    static unsigned long pumps; if (getenv("HW_LOG_EVENTS") && !(++pumps % 300)) hw_log("pump %lu: host queue %d, guest queue %d", pumps, SDL_PeepEvents(0, 0, SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT), q_count());
    SDL_PumpEvents();
    SDL_Event e; uint8_t g[56];
    static int log_events = -1; if (log_events < 0) log_events = getenv("HW_LOG_EVENTS") != 0;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_SYSWMEVENT) continue;
        if (log_events && e.type != SDL_MOUSEMOTION) hw_log("event %#x key %d/%d mod %x button %d at %d,%d", e.type, e.type == SDL_KEYDOWN || e.type == SDL_KEYUP ? e.key.keysym.sym : 0,
                                                            e.type == SDL_KEYDOWN || e.type == SDL_KEYUP ? e.key.keysym.scancode : 0, e.type == SDL_KEYDOWN ? e.key.keysym.mod : 0,
                                                            e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP ? e.button.button : 0, e.button.x, e.button.y);
        else if (log_events) { static int nm; if (!(nm++ % 30)) hw_log("motion %d,%d", e.motion.x, e.motion.y); }
        to_guest(&e, g);
        if (filter_fn || nwatch) {                      /* the game's filter, then its watchers, see the event in guest memory */
            uint32_t ge = g_malloc(56); memcpy(GP(ge), g, 56);
            if (filter_fn) {
                uint32_t args[2] = {filter_data, ge}; uint32_t keep = g_call(c, filter_fn, 2, args);
                if (!keep) { g_free(ge); continue; }
            }
            /* event watches: Aspyr's DirectInput keyboard and mouse (what the game reads in play) are fed this way */
            for (int k = 0; k < nwatch; k++) { uint32_t args[2] = {watch[k][1], ge}; g_call(c, watch[k][0], 2, args); }
            memcpy(g, GP(ge), 56); g_free(ge);
        }
        pthread_mutex_lock(&q_lock); q_push(g); pthread_mutex_unlock(&q_lock);
    }
}
OVR(SDL_PumpEvents) { pump(c); RETV(); }
OVR(SDL_SetEventFilter) { filter_fn = ARG(0); filter_data = ARG(1); RETV(); }
OVR(SDL_AddEventWatch) { if (nwatch < 16) { watch[nwatch][0] = ARG(0); watch[nwatch][1] = ARG(1); nwatch++; } hw_log("SDL_AddEventWatch(%#x): %d watching", ARG(0), nwatch); RETV(); }
OVR(SDL_DelEventWatch)
{
    for (int k = 0; k < nwatch; k++) if (watch[k][0] == ARG(0) && watch[k][1] == ARG(1)) { memmove(watch[k], watch[k + 1], sizeof watch[0] * (size_t)(nwatch - k - 1)); nwatch--; break; }
    RETV();
}
/* SDL_FilterEvents(filter, userdata): the queued events the filter returns 0 for are dropped */
OVR(SDL_FilterEvents)
{
    uint32_t fn = ARG(0), ud = ARG(1); uint32_t ge = g_malloc(56);
    pthread_mutex_lock(&q_lock);
    uint8_t keep[QN][56]; int n = 0;
    for (int i = qhead; i != qtail; i = (i + 1) % QN) {
        memcpy(GP(ge), queue[i], 56); uint32_t args[2] = {ud, ge};
        if (g_call(c, fn, 2, args)) memcpy(keep[n++], queue[i], 56);
    }
    qhead = qtail = 0; for (int k = 0; k < n; k++) q_push(keep[k]);
    pthread_mutex_unlock(&q_lock); g_free(ge); RETV();
}
OVR(SDL_GetEventFilter) { if (ARG(0)) rt_w32(G_MEM, ARG(0), filter_fn); if (ARG(1)) rt_w32(G_MEM, ARG(1), filter_data); RET(filter_fn != 0); }
/* SDL_PeepEvents(events, numevents, action: ADD 0 / PEEK 1 / GET 2, minType, maxType) */
OVR(SDL_PeepEvents)
{
    uint32_t ev = ARG(0); int n = (int)ARG(1), act = (int)ARG(2); uint32_t lo = ARG(3), hi = ARG(4); int done = 0;
    pthread_mutex_lock(&q_lock);
    if (act == SDL_ADDEVENT) { for (int k = 0; k < n; k++) { q_push(GP(ev + 56u * (uint32_t)k)); done++; } }
    else {
        for (int i = qhead; i != qtail && done < n; ) {
            uint32_t t; memcpy(&t, queue[i], 4);
            if (t >= lo && t <= hi) {
                if (ev) memcpy(GP(ev + 56u * (uint32_t)done), queue[i], 56);
                done++;
                if (act == SDL_GETEVENT) {             /* remove: shift the rest down */
                    for (int j = i; (j + 1) % QN != qtail; j = (j + 1) % QN) memcpy(queue[j], queue[(j + 1) % QN], 56);
                    qtail = (qtail - 1 + QN) % QN; continue;
                }
            }
            i = (i + 1) % QN;
        }
    }
    pthread_mutex_unlock(&q_lock);
    RET(done);
}
OVR(SDL_PollEvent)
{
    if (!q_count()) pump(c);
    pthread_mutex_lock(&q_lock);
    if (!q_count()) { pthread_mutex_unlock(&q_lock); RET(0); }
    if (ARG(0)) { memcpy(GP(ARG(0)), queue[qhead], 56); qhead = (qhead + 1) % QN; }
    pthread_mutex_unlock(&q_lock); RET(1);
}
OVR(SDL_PushEvent)
{
    uint8_t g[56]; memcpy(g, GP(ARG(0)), 56);
    pthread_mutex_lock(&q_lock); q_push(g); pthread_mutex_unlock(&q_lock); RET(1);
}
OVR(SDL_FlushEvent) { uint32_t t = ARG(0); pthread_mutex_lock(&q_lock);
    int w = qhead; for (int i = qhead; i != qtail; i = (i + 1) % QN) { uint32_t et; memcpy(&et, queue[i], 4); if (et != t) { if (w != i) memcpy(queue[w], queue[i], 56); w = (w + 1) % QN; } }
    qtail = w; pthread_mutex_unlock(&q_lock); RETV(); }
OVR(SDL_FlushEvents) { uint32_t lo = ARG(0), hi = ARG(1); pthread_mutex_lock(&q_lock);
    int w = qhead; for (int i = qhead; i != qtail; i = (i + 1) % QN) { uint32_t et; memcpy(&et, queue[i], 4); if (et < lo || et > hi) { if (w != i) memcpy(queue[w], queue[i], 56); w = (w + 1) % QN; } }
    qtail = w; pthread_mutex_unlock(&q_lock); RETV(); }
OVR(SDL_EventState) { RET(SDL_EventState(ARG(0), (int)ARG(1))); }
OVR(SDL_HasEvent) { uint32_t t = ARG(0); int r = 0; pthread_mutex_lock(&q_lock); for (int i = qhead; i != qtail; i = (i + 1) % QN) { uint32_t et; memcpy(&et, queue[i], 4); if (et == t) r = 1; } pthread_mutex_unlock(&q_lock); RET(r); }

/* ---- mouse and keyboard ---- */
static int script_mx, script_my, script_active; static Uint32 script_buttons;
static int script_mouse(Ctx *c)                       /* test runs: the scripted pointer is the mouse */
{
    if (!hw_test_mode || !script_active) return 0;
    if (ARG(0)) rt_w32(G_MEM, ARG(0), (uint32_t)script_mx); if (ARG(1)) rt_w32(G_MEM, ARG(1), (uint32_t)script_my);
    c->eax = script_buttons; c->esp += 4; return 1;
}
OVR(SDL_GetMouseState) { if (script_mouse(c)) return; int x = 0, y = 0; uint32_t b = SDL_GetMouseState(&x, &y); if (ARG(0)) rt_w32(G_MEM, ARG(0), (uint32_t)x); if (ARG(1)) rt_w32(G_MEM, ARG(1), (uint32_t)y); RET(b); }
OVR(SDL_GetGlobalMouseState) { int x = 0, y = 0; uint32_t b = SDL_GetGlobalMouseState(&x, &y); if (ARG(0)) rt_w32(G_MEM, ARG(0), (uint32_t)x); if (ARG(1)) rt_w32(G_MEM, ARG(1), (uint32_t)y); RET(b); }
OVR(SDL_GetRelativeMouseState) { int x = 0, y = 0; uint32_t b = SDL_GetRelativeMouseState(&x, &y); if (ARG(0)) rt_w32(G_MEM, ARG(0), (uint32_t)x); if (ARG(1)) rt_w32(G_MEM, ARG(1), (uint32_t)y); RET(b); }
OVR(SDL_GetRelativeMouseMode) { RET(SDL_GetRelativeMouseMode()); }
OVR(SDL_SetRelativeMouseMode) { RET(hw_test_mode ? 0 : SDL_SetRelativeMouseMode((SDL_bool)ARG(0))); }
OVR(SDL_WarpMouseInWindow) { if (!hw_test_mode) SDL_WarpMouseInWindow(WIN(0), (int)ARG(1), (int)ARG(2)); RETV(); }
OVR(SDL_ShowCursor) { RET(hw_test_mode ? 1 : SDL_ShowCursor((int)ARG(0))); }
OVR(SDL_CreateSystemCursor) { RET(wrap(H_CURSOR, SDL_CreateSystemCursor((SDL_SystemCursor)ARG(0)))); }
/* guest SDL_Surface (i386): flags, format*, w, h, pitch, pixels; SDL_PixelFormat: format first */
OVR(SDL_CreateColorCursor)
{
    uint32_t s = ARG(0); if (!s) RET(0);
    uint32_t fmt = rt_r32(G_MEM, rt_r32(G_MEM, s + 4)); int w = (int)rt_r32(G_MEM, s + 8), h = (int)rt_r32(G_MEM, s + 12), pitch = (int)rt_r32(G_MEM, s + 16);
    SDL_Surface *hs = SDL_CreateRGBSurfaceWithFormatFrom(GP(rt_r32(G_MEM, s + 20)), w, h, SDL_BITSPERPIXEL(fmt), pitch, fmt);
    SDL_Cursor *cur = hs ? SDL_CreateColorCursor(hs, (int)ARG(1), (int)ARG(2)) : 0;
    if (hs) SDL_FreeSurface(hs);
    RET(wrap(H_CURSOR, cur));
}
OVR(SDL_SetCursor) { SDL_Cursor *cur = unwrap(ARG(0), H_CURSOR); if (cur) SDL_SetCursor(cur); RETV(); }
OVR(SDL_FreeCursor) { SDL_Cursor *cur = unwrap(ARG(0), H_CURSOR); if (cur) SDL_FreeCursor(cur); RETV(); }
OVR(SDL_GetKeyFromScancode) { RET(SDL_GetKeyFromScancode((SDL_Scancode)ARG(0))); }
OVR(SDL_GetScancodeFromKey) { RET(SDL_GetScancodeFromKey((SDL_Keycode)ARG(0))); }
OVR(SDL_GetModState) { RET(SDL_GetModState()); }
OVR(SDL_GetKeyboardState)
{
    static uint32_t g; int n = 0; const Uint8 *k = SDL_GetKeyboardState(&n);
    if (!g) g = g_calloc(SDL_NUM_SCANCODES);
    memcpy(GP(g), k, (size_t)(n < SDL_NUM_SCANCODES ? n : SDL_NUM_SCANCODES));
    if (ARG(0)) rt_w32(G_MEM, ARG(0), SDL_NUM_SCANCODES); RET(g);
}
OVR(SDL_StartTextInput) { SDL_StartTextInput(); RETV(); }
OVR(SDL_StopTextInput) { SDL_StopTextInput(); RETV(); }
OVR(SDL_IsTextInputActive) { RET(SDL_IsTextInputActive()); }

/* ---- game controllers, joysticks, haptics ---- */
OVR(SDL_NumJoysticks) { RET(SDL_NumJoysticks()); }
OVR(SDL_IsGameController) { RET(SDL_IsGameController((int)ARG(0))); }
OVR(SDL_GameControllerOpen) { RET(wrap(H_CTRL, SDL_GameControllerOpen((int)ARG(0)))); }
OVR(SDL_GameControllerClose) { SDL_GameController *g = unwrap(ARG(0), H_CTRL); if (g) SDL_GameControllerClose(g); RETV(); }
OVR(SDL_GameControllerGetAttached) { SDL_GameController *g = unwrap(ARG(0), H_CTRL); RET(g ? SDL_GameControllerGetAttached(g) : 0); }
OVR(SDL_GameControllerGetJoystick) { SDL_GameController *g = unwrap(ARG(0), H_CTRL); RET(g ? wrap(H_JOY, SDL_GameControllerGetJoystick(g)) : 0); }
OVR(SDL_GameControllerGetButton) { SDL_GameController *g = unwrap(ARG(0), H_CTRL); RET(g ? SDL_GameControllerGetButton(g, (SDL_GameControllerButton)ARG(1)) : 0); }
OVR(SDL_GameControllerGetAxis) { SDL_GameController *g = unwrap(ARG(0), H_CTRL); RET(g ? (uint32_t)(int32_t)SDL_GameControllerGetAxis(g, (SDL_GameControllerAxis)ARG(1)) : 0); }
OVR(SDL_GameControllerName) { SDL_GameController *g = unwrap(ARG(0), H_CTRL); RET(gstr(g ? SDL_GameControllerName(g) : 0)); }
OVR(SDL_GameControllerEventState) { RET(SDL_GameControllerEventState((int)ARG(0))); }
OVR(SDL_JoystickInstanceID) { SDL_Joystick *j = unwrap(ARG(0), H_JOY); RET(j ? (uint32_t)SDL_JoystickInstanceID(j) : (uint32_t)-1); }
OVR(SDL_JoystickOpen) { RET(wrap(H_JOY, SDL_JoystickOpen((int)ARG(0)))); }
OVR(SDL_JoystickClose) { SDL_Joystick *j = unwrap(ARG(0), H_JOY); if (j) SDL_JoystickClose(j); RETV(); }
OVR(SDL_HapticOpenFromJoystick) { SDL_Joystick *j = unwrap(ARG(0), H_JOY); RET(j ? wrap(H_HAPTIC, SDL_HapticOpenFromJoystick(j)) : 0); }
OVR(SDL_HapticClose) { SDL_Haptic *h = unwrap(ARG(0), H_HAPTIC); if (h) SDL_HapticClose(h); RETV(); }
OVR(SDL_HapticRumbleInit) { SDL_Haptic *h = unwrap(ARG(0), H_HAPTIC); RET(h ? SDL_HapticRumbleInit(h) : -1); }
OVR(SDL_HapticRumblePlay) { SDL_Haptic *h = unwrap(ARG(0), H_HAPTIC); RET(h ? SDL_HapticRumblePlay(h, ARGF(1), ARG(2)) : -1); }
OVR(SDL_HapticRumbleStop) { SDL_Haptic *h = unwrap(ARG(0), H_HAPTIC); RET(h ? SDL_HapticRumbleStop(h) : -1); }
OVR(SDL_HapticNewEffect) { RET((uint32_t)-1); }      /* custom effects: rumble only for now */
OVR(SDL_HapticRunEffect) { RET((uint32_t)-1); }

/* ---- message boxes: SDL_MessageBoxData (i386): flags, window, title, message, numbuttons, buttons{flags,id,text}, colors ---- */
OVR(SDL_ShowMessageBox)
{
    uint32_t d = ARG(0); int n = (int)rt_r32(G_MEM, d + 16); uint32_t bp = rt_r32(G_MEM, d + 20);
    SDL_MessageBoxButtonData *b = calloc((size_t)(n > 0 ? n : 1), sizeof *b);
    for (int k = 0; k < n; k++) { b[k].flags = rt_r32(G_MEM, bp + 12u * (uint32_t)k); b[k].buttonid = (int)rt_r32(G_MEM, bp + 12u * (uint32_t)k + 4); b[k].text = GS(rt_r32(G_MEM, bp + 12u * (uint32_t)k + 8)); }
    SDL_MessageBoxData md = {rt_r32(G_MEM, d), 0, GS(rt_r32(G_MEM, d + 8)), GS(rt_r32(G_MEM, d + 12)), n, b, 0};
    hw_log("message box: %s: %s", md.title, md.message);
    int hit = n > 0 ? b[0].buttonid : 0, r = 0;
    if (!hw_test_mode) r = SDL_ShowMessageBox(&md, &hit);
    free(b); if (ARG(1)) rt_w32(G_MEM, ARG(1), (uint32_t)hit); RET(r);
}
OVR(SDL_ShowSimpleMessageBox)
{
    hw_log("message box: %s: %s", GS(ARG(1)), GS(ARG(2)));
    RET(hw_test_mode ? 0 : SDL_ShowSimpleMessageBox(ARG(0), GS(ARG(1)), GS(ARG(2)), 0));
}
OVR(SDL_SetClipboardText) { RET(SDL_SetClipboardText(GS(ARG(0)))); }
OVR(SDL_GetClipboardText) { char *t = SDL_GetClipboardText(); uint32_t g = g_strdup(t ? t : ""); SDL_free(t); RET(g); }
OVR(SDL_HasClipboardText) { RET(SDL_HasClipboardText()); }

/* HW_SHOT=<file.png>,<ms>: save the back buffer at the first swap after <ms> milliseconds since the first frame */
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <ImageIO/ImageIO.h>
#include <CoreServices/CoreServices.h>
void hw_shot_now(SDL_Window *w, const char *path, unsigned long long frame);
void hw_shot_maybe(SDL_Window *w, unsigned long long frame)
{
    static char path[1024]; static unsigned long long at; static int init;
    if (!init) { init = 1; const char *e = getenv("HW_SHOT"); if (e) { const char *k = strrchr(e, ','); if (k) { snprintf(path, sizeof path, "%.*s", (int)(k - e), e); at = strtoull(k + 1, 0, 10); } } }
    if (!path[0] || frame < at) return;
    path[0] = 0; char p2[1024]; snprintf(p2, sizeof p2, "%s", getenv("HW_SHOT")); *strrchr(p2, ',') = 0;
    hw_shot_now(w, p2, frame);
}
void hw_shot_now(SDL_Window *w, const char *path, unsigned long long frame)
{
    int dw = 0, dh = 0; SDL_GL_GetDrawableSize(w, &dw, &dh);
    if (gl_metal) mgl_drawable_size(w, &dw, &dh);
    uint8_t *px = malloc((size_t)dw * dh * 4);
    if (gl_metal) mgl_read_back(0, 0, dw, dh, px);
    else { glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glReadBuffer(GL_BACK); glPixelStorei(GL_PACK_ALIGNMENT, 1);
           glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px); }
    for (int y = 0; y < dh / 2; y++) for (int x = 0; x < dw * 4; x++) { uint8_t t = px[y * dw * 4 + x]; px[y * dw * 4 + x] = px[(dh - 1 - y) * dw * 4 + x]; px[(dh - 1 - y) * dw * 4 + x] = t; }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px, (size_t)dw, (size_t)dh, 8, (size_t)dw * 4, cs, kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, 0);
    CGImageDestinationAddImage(dst, img, 0); CGImageDestinationFinalize(dst);
    CFRelease(dst); CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs); free(px);
    hw_log("screenshot %s (%dx%d, frame %llu)", path, dw, dh, frame);
}

/* HW_SCRIPT="<ms>:<action>;..." drives the game in test runs (each action runs at the first swap after <ms> milliseconds). Actions: click X Y, rclick X Y, move X Y,
 * key NAME (an SDL key name: Return, Escape, Space, A...), text STRING, shot FILE.png. Coordinates are window points. */
static SDL_Event pending_up; static unsigned long long pending_up_ms;
void hw_script_step(SDL_Window *w, unsigned long long frame)
{
    static char *script; static int init;
    if (pending_up_ms && frame >= pending_up_ms) { SDL_PushEvent(&pending_up); pending_up_ms = 0; script_buttons = 0; }
    if (!init) { init = 1; const char *e = getenv("HW_SCRIPT"); if (e) script = strdup(e); }
    if (!script || !w) return;
    for (char *p = script; p && *p; ) {
        char *end = strchr(p, ';'); size_t n = end ? (size_t)(end - p) : strlen(p);
        char item[256]; snprintf(item, sizeof item, "%.*s", (int)n, p);
        unsigned long long at = strtoull(item, 0, 10); char *act = strchr(item, ':');
        if (act && at <= frame && act[-1] != '!') {
            act[-1] = '!'; memcpy(p + (act - item) - 1, "!", 1);       /* done: mark it in the script */
            act++; int x = 0, y = 0; char arg[200] = "";
            SDL_Event e; memset(&e, 0, sizeof e); Uint32 id = SDL_GetWindowID(w);
            if (sscanf(act, "click %d %d", &x, &y) == 2 || sscanf(act, "rclick %d %d", &x, &y) == 2) {
                int b = act[0] == 'r' ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
                script_mx = x; script_my = y; script_active = 1;
                e.type = SDL_MOUSEMOTION; e.motion.windowID = id; e.motion.x = x; e.motion.y = y; SDL_PushEvent(&e);
                memset(&e, 0, sizeof e); e.type = SDL_MOUSEBUTTONDOWN; e.button.windowID = id; e.button.button = (Uint8)b; e.button.state = SDL_PRESSED; e.button.clicks = 1; e.button.x = x; e.button.y = y; SDL_PushEvent(&e);
                script_buttons = SDL_BUTTON(b); pending_up = e; pending_up.type = SDL_MOUSEBUTTONUP; pending_up.button.state = SDL_RELEASED; pending_up_ms = frame + 120;
                hw_log("script frame %llu: %s", frame, act);
            } else if (sscanf(act, "move %d %d", &x, &y) == 2) {
                script_mx = x; script_my = y; script_active = 1;
                e.type = SDL_MOUSEMOTION; e.motion.windowID = id; e.motion.x = x; e.motion.y = y; SDL_PushEvent(&e);
            } else if (sscanf(act, "key %199s", arg) == 1) {
                SDL_Keycode k = SDL_GetKeyFromName(arg);
                e.type = SDL_KEYDOWN; e.key.windowID = id; e.key.state = SDL_PRESSED; e.key.keysym.sym = k; e.key.keysym.scancode = SDL_GetScancodeFromKey(k); SDL_PushEvent(&e);
                pending_up = e; pending_up.type = SDL_KEYUP; pending_up.key.state = SDL_RELEASED; pending_up_ms = frame + 120;
                hw_log("script frame %llu: %s", frame, act);
            } else if (sscanf(act, "text %199[^\n]", arg) == 1) {
                e.type = SDL_TEXTINPUT; e.text.windowID = id; snprintf(e.text.text, sizeof e.text.text, "%s", arg); SDL_PushEvent(&e);
            } else if (sscanf(act, "shot %199s", arg) == 1) {
                extern void hw_shot_now(SDL_Window *w, const char *path, unsigned long long frame); hw_shot_now(w, arg, frame);
            }
        }
        p = end ? end + 1 : 0;
    }
}
