/* CoreGraphics display services, CoreVideo display links and the Carbon keyboard layout calls, for Aspyr's display
 * code. Display IDs and display masks are the host's (32-bit values either way). CGRect/CGPoint/CGSize use 32-bit
 * floats in the guest (CGFloat is float on i386). Display capture, fades and gamma changes are not performed. */
#include "mac.h"
#include <CoreGraphics/CoreGraphics.h>
#include <OpenGL/OpenGL.h>
#include <CoreVideo/CoreVideo.h>
uint32_t cf_own(CFTypeRef o); uint32_t cf_get(CFTypeRef o); CFTypeRef cf_in(uint32_t g);
extern int hw_test_mode;

IMP(_CGMainDisplayID) { RET(CGMainDisplayID()); }
IMP(_CGDisplayIDToOpenGLDisplayMask) { RET(CGDisplayIDToOpenGLDisplayMask(ARG(0))); }
IMP(_CGGetOnlineDisplayList)    /* (max, CGDirectDisplayID *list, uint32_t *count) */
{
    CGDirectDisplayID d[32]; uint32_t n = 0, max = ARG(0) < 32 ? ARG(0) : 32;
    CGError e = CGGetOnlineDisplayList(max, ARG(1) ? d : 0, &n);
    if (ARG(1)) for (uint32_t k = 0; k < n; k++) rt_w32(G_MEM, ARG(1) + 4 * k, d[k]);
    if (ARG(2)) rt_w32(G_MEM, ARG(2), n);
    RET(e);
}
IMP(_CGDisplayBounds)            /* CGRect result through the hidden pointer: 4 floats */
{
    CGRect r = CGDisplayBounds(rt_r32(G_MEM, c->esp + 8)); uint32_t o = ARG(0);
    rt_wf32(G_MEM, o, (float)r.origin.x); rt_wf32(G_MEM, o + 4, (float)r.origin.y);
    rt_wf32(G_MEM, o + 8, (float)r.size.width); rt_wf32(G_MEM, o + 12, (float)r.size.height);
    RET_SRET();
}
IMP(_CGDisplayPixelsHigh) { RET((uint32_t)CGDisplayPixelsHigh(ARG(0))); }
IMP(_CGDisplayIsMain) { RET(CGDisplayIsMain(ARG(0))); }
IMP(_CGDisplayMirrorsDisplay) { RET(CGDisplayMirrorsDisplay(ARG(0))); }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
IMP(_CGDisplayIOServicePort) { RET(0); }
IMP(_CGDisplayModeCopyPixelEncoding) { RET(cf_own(CFSTR("--------RRRRRRRRGGGGGGGGBBBBBBBB"))); }
#pragma clang diagnostic pop
IMP(_CGDisplayCopyDisplayMode) { RET(cf_own(CGDisplayCopyDisplayMode(ARG(0)))); }
IMP(_CGDisplayCopyAllDisplayModes) { RET(cf_own(CGDisplayCopyAllDisplayModes(ARG(0), (CFDictionaryRef)cf_in(ARG(1))))); }
IMP(_CGDisplayModeGetWidth) { RET((uint32_t)CGDisplayModeGetWidth((CGDisplayModeRef)cf_in(ARG(0)))); }
IMP(_CGDisplayModeGetHeight) { RET((uint32_t)CGDisplayModeGetHeight((CGDisplayModeRef)cf_in(ARG(0)))); }
IMP(_CGDisplayModeGetRefreshRate) { RETD(CGDisplayModeGetRefreshRate((CGDisplayModeRef)cf_in(ARG(0)))); }
IMP(_CGDisplayModeRetain) { extern void imp__CFRetain(Ctx *); imp__CFRetain(c); }
IMP(_CGDisplayModeRelease) { extern void imp__CFRelease(Ctx *); imp__CFRelease(c); }
IMP(_CGDisplaySetDisplayMode) { RET(0); }               /* the game keeps the desktop mode */
IMP(_CGDisplayCapture) { RET(0); }
IMP(_CGCaptureAllDisplays) { RET(0); }
IMP(_CGDisplayRelease) { RET(0); }
IMP(_CGReleaseAllDisplays) { RET(0); }
IMP(_CGAcquireDisplayFadeReservation) { if (ARG(1)) rt_w32(G_MEM, ARG(1), 1); RET(0); }
IMP(_CGReleaseDisplayFadeReservation) { RET(0); }
IMP(_CGDisplayFade) { RET(0); }
IMP(_CGShieldingWindowLevel) { RET(CGShieldingWindowLevel()); }
IMP(_CGWindowLevelForKey) { RET(CGWindowLevelForKey((CGWindowLevelKey)ARG(0))); }
IMP(_CGGetDisplayTransferByTable)  /* (display, capacity, red*, green*, blue*, count*): identity */
{
    uint32_t n = ARG(1);
    for (uint32_t k = 0; k < n; k++) { float v = n > 1 ? (float)k / (float)(n - 1) : 0;
        rt_wf32(G_MEM, ARG(2) + 4 * k, v); rt_wf32(G_MEM, ARG(3) + 4 * k, v); rt_wf32(G_MEM, ARG(4) + 4 * k, v); }
    if (ARG(5)) rt_w32(G_MEM, ARG(5), n); RET(0);
}
IMP(_CGSetDisplayTransferByTable) { RET(0); }
IMP(_CGWarpMouseCursorPosition) { if (!hw_test_mode) CGWarpMouseCursorPosition(CGPointMake(ARGF(0), ARGF(1))); RET(0); }
IMP(_CGDisplayMoveCursorToPoint) { if (!hw_test_mode) CGDisplayMoveCursorToPoint(ARG(0), CGPointMake(ARGF(1), ARGF(2))); RET(0); }
IMP(_CGAssociateMouseAndMouseCursorPosition) { if (!hw_test_mode) CGAssociateMouseAndMouseCursorPosition(ARG(0)); RET(0); }
IMP(_CGContextRelease) { RETV(); }

/* CoreVideo: the display's refresh period (CVTime {int64 value; int32 scale; int32 flags} by hidden pointer) */
IMP(_CVDisplayLinkCreateWithCGDisplay) { rt_w32(G_MEM, ARG(1), 0xcdcd0000u | (ARG(0) & 0xffff)); RET(0); }
IMP(_CVDisplayLinkRelease) { RETV(); }
IMP(_CVDisplayLinkGetNominalOutputVideoRefreshPeriod)
{
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID()); double hz = m ? CGDisplayModeGetRefreshRate(m) : 60; if (m) CGDisplayModeRelease(m);
    if (hz <= 0) hz = 60;
    uint32_t o = ARG(0); rt_w64(G_MEM, o, 1000); rt_w32(G_MEM, o + 8, (uint32_t)(hz * 1000)); rt_w32(G_MEM, o + 12, 0);
    RET_SRET();
}

/* Carbon keyboard layout (only SDL's keymap code asked; SDL is replaced) */
IMP(_TISCopyCurrentKeyboardLayoutInputSource) { RET(0); }
IMP(_TISGetInputSourceProperty) { RET(0); }
IMP(_UCKeyTranslate) { RET(-50); }

/* ASL::SDL::GetCGDirectDisplayID(SDL_Window *): the window's display (SDL_GetWindowWMInfo is not available) */
OVR(_ZN3ASL3SDL20GetCGDirectDisplayIDEP10SDL_Window) { RET(CGMainDisplayID()); }

/* GetPCIDisplayInfo(CGDirectDisplayID, info *): the GPU's PCI identity from IOKit, which picks the game's quality
 * defaults. info: [0] device-id, [4] vendor-id, [8] revision-id, [12] subsystem-id, [16] model (64 chars).
 * Apple GPUs are not in the 2015 tables: report a Mac AMD GPU of the time (HW_GPU=vendor:device:model overrides). */
OVR(GetPCIDisplayInfo)
{
    uint32_t o = ARG(1); unsigned vendor = 0x1002, device = 0x6938; char model[64] = "AMD Radeon R9 M395X";
    const char *e = getenv("HW_GPU");
    if (e) { char m[64] = ""; if (sscanf(e, "%x:%x:%63[^\n]", &vendor, &device, m) >= 2 && m[0]) strlcpy(model, m, sizeof model); }
    if (!o) RET(0xe00002bcu);
    rt_w32(G_MEM, o, device); rt_w32(G_MEM, o + 4, vendor); rt_w32(G_MEM, o + 8, 0); rt_w32(G_MEM, o + 12, 0);
    strlcpy(GP(o + 16), model, 64);
    RET(0);
}
