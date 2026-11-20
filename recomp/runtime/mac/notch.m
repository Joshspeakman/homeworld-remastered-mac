/* Full screen on a display with a notch, and Game Mode.
 *
 * The launcher's Display tile starts the game in macOS full screen (SDL_WINDOW_FULLSCREEN_DESKTOP: a Space of its
 * own), which is what turns on Game Mode for an app that declares itself a game (install.sh: Info.plist
 * GCSupportsGameMode, a games category). On a display with a notch macOS keeps a full-screen window below the camera
 * housing whatever frame it asks for, so the launcher's Notch tile on Around It (HW_NOTCH=around) makes the game a
 * screen-sized borderless window instead, the Dock and menu bar hidden while it is in front, the housing over the
 * picture's top centre: the Dungeon Siege, Diablo II and Sims 2 ports' way (macOS gives that no Game Mode). */
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_metal.h>

void hw_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int hw_notch_around(void) { const char *v = getenv("HW_NOTCH"); return v && !strcmp(v, "around"); }
int hw_notch_height(void) { if (@available(macOS 12.0, *)) return (int)NSScreen.mainScreen.safeAreaInsets.top; return 0; }
static int want_fullscreen(void) { const char *v = getenv("HW_FULLSCREEN"); return v && atoi(v); }
/* full screen around the notch: a borderless window over the whole display (sdl.c makes it borderless and screen-sized) */
int hw_borderless_around(void) { return want_fullscreen() && hw_notch_around() && hw_notch_height() > 0; }
static NSRect unconstrained(id self, SEL _cmd, NSRect r, NSScreen *sc) { (void)self; (void)_cmd; (void)sc; return r; }   /* over the menu bar and notch */
static void around_front(void) { NSApp.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar; }

void hw_window_created(SDL_Window *sw)
{
    SDL_SysWMinfo info; SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(sw, &info) || info.subsystem != SDL_SYSWM_COCOA) return;
    NSWindow *w = info.info.cocoa.window; if (!w) return;
    w.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    if (getenv("HW_LOG_FPS"))
        for (NSString *n in @[NSWindowWillEnterFullScreenNotification, NSWindowDidEnterFullScreenNotification, NSWindowWillExitFullScreenNotification,
                              NSWindowDidExitFullScreenNotification])   /* how the full-screen switch went */
            [[NSNotificationCenter defaultCenter] addObserverForName:n object:w queue:nil usingBlock:^(NSNotification *x) {
                extern unsigned long long hw_ms(void); hw_log("%s at %llu ms (frame %.0fx%.0f)", x.name.UTF8String, hw_ms(), w.frame.size.width, w.frame.size.height); }];
    if (hw_borderless_around()) {
        Class c = object_getClass(w); SEL sel = @selector(constrainFrameRect:toScreen:);
        const char *types = method_getTypeEncoding(class_getInstanceMethod([NSWindow class], sel));
        if (!class_addMethod(c, sel, (IMP)unconstrained, types)) class_replaceMethod(c, sel, (IMP)unconstrained, types);
        w.collectionBehavior = (w.collectionBehavior & ~NSWindowCollectionBehaviorFullScreenPrimary) | NSWindowCollectionBehaviorFullScreenNone;
        around_front();
        [w setFrame:(w.screen ?: NSScreen.mainScreen).frame display:YES];
        /* the Dock and menu bar hidden only while the game is in front */
        [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *x) { (void)x; around_front(); }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *x) { (void)x; NSApp.presentationOptions = NSApplicationPresentationDefault; }];
        hw_log("full screen around the notch: a borderless %.0fx%.0f window (%s)", w.frame.size.width, w.frame.size.height, class_getName(c));
    }
}
void hw_window_report(SDL_Window *sw)                  /* HW_LOG_FPS: the window as macOS placed it, once the game runs */
{
    SDL_SysWMinfo info; SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(sw, &info) || info.subsystem != SDL_SYSWM_COCOA) return;
    NSWindow *w = info.info.cocoa.window; NSRect f = w.frame, s = w.screen.frame; int dw = 0, dh = 0; SDL_Metal_GetDrawableSize(sw, &dw, &dh);
    hw_log("window: %s, frame %.0fx%.0f at %.0f,%.0f on a %.0fx%.0f screen (notch %d), drawable %dx%d",
           (w.styleMask & NSWindowStyleMaskFullScreen) ? "macOS full screen" : "windowed", f.size.width, f.size.height, f.origin.x, f.origin.y,
           s.size.width, s.size.height, hw_notch_height(), dw, dh);
    for (NSWindow *x in NSApp.windows) hw_log("  app window %s frame %.0f,%.0f %.0fx%.0f level %ld visible %d alpha %.2f opaque %d", class_getName(x.class), x.frame.origin.x, x.frame.origin.y, x.frame.size.width, x.frame.size.height, (long)x.level, x.visible, x.alphaValue, x.opaque);
    NSView *cv = w.contentView; hw_log("content view %s frame %.0f,%.0f %.0fx%.0f safe top %.0f", class_getName(cv.class), cv.frame.origin.x, cv.frame.origin.y, cv.frame.size.width, cv.frame.size.height, cv.safeAreaInsets.top);
    for (NSView *sv in cv.subviews) hw_log("  subview %s frame %.0f,%.0f %.0fx%.0f layer %.0fx%.0f", class_getName(sv.class), sv.frame.origin.x, sv.frame.origin.y, sv.frame.size.width, sv.frame.size.height, sv.layer.bounds.size.width, sv.layer.bounds.size.height);
}
void hw_enter_fullscreen(SDL_Window *sw)               /* macOS full screen: a Space of its own (Game Mode) */
{
    SDL_SysWMinfo info; SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(sw, &info) || info.subsystem != SDL_SYSWM_COCOA) return;
    NSWindow *w = info.info.cocoa.window;
    if (!w || hw_borderless_around()) return;
    /* the game swaps on its render thread; AppKit windows change only on the main thread (which pumps SDL's events) */
    dispatch_async(dispatch_get_main_queue(), ^{
    if (w.styleMask & NSWindowStyleMaskFullScreen) return;
    hw_log("entering macOS full screen%s (behavior %lx, spaces hint %s, visible %d)", hw_notch_around() ? " (around the notch)" : "",
           (unsigned long)w.collectionBehavior, SDL_GetHint("SDL_VIDEO_MAC_FULLSCREEN_SPACES") ?: "-", w.visible);
    w.collectionBehavior = (w.collectionBehavior & ~(NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorFullScreenNone)) | NSWindowCollectionBehaviorFullScreenPrimary;
    [w toggleFullScreen:nil];
    });
}
void hw_fullscreen_size(int *w, int *h)                /* the full-screen window's size in points (below or around the notch) */
{
    NSRect f = NSScreen.mainScreen.frame;
    if (!hw_notch_around()) f.size.height -= hw_notch_height();
    *w = (int)f.size.width; *h = (int)f.size.height;
}
