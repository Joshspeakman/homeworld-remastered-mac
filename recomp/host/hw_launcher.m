/* Homeworld Remastered launcher, in the manner of the Dungeon Siege ports' launch windows: the collection's logo over the
 * starfield of Aspyr's own launcher, a card for each game (a screenshot with the game's logo, lit when chosen), a row
 * of setting tiles (Display, Renderer, Movies, Saves, Updates) and Quit / Play. Then the recompiled game
 * (Contents/MacOS/hw2hd) starts in this process, so the app in the Dock is the game.
 *
 * Art: the logos (with their hover glow) and the background come from the game's own Resources folder (Aspyr's
 * launcher art, in the user's copy, Info.plist HWGamePath); the cards' screenshots from the data folder's art/ (or a
 * copy in the app's Resources/art), fetched at install time by tools/hw_art.py. Nothing from the game is in this project.
 * Updates: install.sh gives the app an updater (Contents/MacOS/update) that rebuilds it from the branch it follows.
 * Development: HW_LAUNCHER_SHOT=<png> renders the window to an image (HW_LAUNCHER_HOVER=<code> lights an item). */
#import <Cocoa/Cocoa.h>
#include <unistd.h>

static NSColor *accent(void) { return [NSColor colorWithSRGBRed:0.40 green:0.80 blue:1.00 alpha:1]; }
static NSColor *accent_dim(void) { return [NSColor colorWithSRGBRed:0.20 green:0.42 blue:0.58 alpha:1]; }
static NSColor *text_c(void) { return [NSColor colorWithSRGBRed:0.90 green:0.95 blue:1.00 alpha:1]; }
static NSColor *dim(void) { return [NSColor colorWithSRGBRed:0.55 green:0.66 blue:0.75 alpha:1]; }
static NSFont *hwfont(CGFloat size) { return [NSFont fontWithName:@"AvenirNext-Bold" size:size] ?: [NSFont systemFontOfSize:size weight:NSFontWeightBold]; }
static NSFont *body(CGFloat size, BOOL bold)
{ return [NSFont fontWithName:bold ? @"AvenirNext-DemiBold" : @"AvenirNext-Medium" size:size] ?: [NSFont systemFontOfSize:size weight:bold ? NSFontWeightSemibold : NSFontWeightRegular]; }
static void on_main(dispatch_block_t b) { CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, b); CFRunLoopWakeUp(CFRunLoopGetMain()); }

// ---------------------------------------------------------------- settings tiles
@interface HWTile : NSObject
@property NSString *title, *key; @property NSArray<NSDictionary *> *choices; @property NSInteger index;
@property (copy) void (^activate)(void);            /* a tile that runs something instead of cycling (Saves, Updates) */
@property (copy) NSString *(^summary)(void);        /* a tile showing others' values (Graphics Options) */
@property BOOL wide;
@end
@implementation HWTile @end

// ---------------------------------------------------------------- the view
enum { HIT_NONE = -1, HIT_PLAY = 100, HIT_QUIT = 101, HIT_BACK = 102, HIT_CARD = 200, HIT_TILE = 300, HIT_ROW = 400 };   /* tiles, panel rows: + 10*k + 0 left / 1 value / 2 right */
enum { W = 960, H = 660 };

@interface HWLaunchView : NSView
@property NSArray<HWTile *> *tiles; @property NSInteger game, hover, pressed, focus;   /* focus: -1 the cards, else a tile */
@property NSArray<HWTile *> *rows; @property BOOL open; @property NSInteger rowFocus;   /* the Graphics Options panel */
@property NSImage *backdrop, *collection; @property NSArray<NSImage *> *shots, *logos, *logosLit; @property NSString *hint;
@property (copy) void (^onPlay)(void); @property (copy) void (^onQuit)(void); @property (copy) void (^onChange)(void);
@end

@implementation HWLaunchView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (NSRect)cardRect:(NSInteger)i { return NSMakeRect(60 + i * 436, 246, 404, 228); }
/* the tiles in a row under the cards (a wide one counts twice) */
- (NSRect)tileRect:(NSInteger)k
{
    NSInteger units = 0, at = 0, n = (NSInteger)self.tiles.count;
    for (NSInteger j = 0; j < n; j++) { if (j == k) at = units; units += self.tiles[j].wide ? 2 : 1; }
    CGFloat u = (840 - 16 * (units - 1)) / (CGFloat)units;
    return NSMakeRect(60 + at * (u + 16), 500, self.tiles[k].wide ? 2 * u + 16 : u, 66);
}
- (NSRect)arrow:(NSInteger)k right:(BOOL)right { NSRect r = [self tileRect:k]; return NSMakeRect(right ? NSMaxX(r) - 22 : r.origin.x + 1, r.origin.y + 26, 21, 34); }
- (NSRect)playRect { return NSMakeRect(W - 60 - 200, H - 68, 200, 46); }
- (NSRect)quitRect { return NSMakeRect(W - 60 - 200 - 20 - 150, H - 68, 150, 46); }
/* the Graphics Options panel over the window, its rows, and each row's arrows and value */
- (NSRect)panelRect { CGFloat h = 86 + 60 * (CGFloat)self.rows.count; return NSMakeRect(130, MAX(40, 492 - h), 700, h); }
- (NSRect)rowRect:(NSInteger)k { NSRect p = self.panelRect; return NSMakeRect(p.origin.x + 30, p.origin.y + 72 + 60 * k, p.size.width - 60, 52); }
- (NSRect)rowArrow:(NSInteger)k right:(BOOL)right { NSRect r = [self rowRect:k]; return NSMakeRect(right ? NSMaxX(r) - 34 : NSMaxX(r) - 330, r.origin.y + 3, 30, 30); }
- (NSRect)rowValue:(NSInteger)k { NSRect r = [self rowRect:k]; return NSMakeRect(NSMaxX(r) - 300, r.origin.y + 4, 262, 28); }
- (void)openPanel { self.open = YES; self.rowFocus = 0; self.hover = HIT_NONE; self.needsDisplay = YES; }
- (void)closePanel { self.open = NO; self.hover = HIT_NONE; self.needsDisplay = YES; }
- (NSInteger)hit:(NSPoint)p
{
    if (NSPointInRect(p, self.playRect)) return HIT_PLAY;
    if (NSPointInRect(p, self.quitRect)) return self.open ? HIT_BACK : HIT_QUIT;
    if (self.open) {
        for (NSInteger k = 0; k < (NSInteger)self.rows.count; k++) {
            if (NSPointInRect(p, [self rowArrow:k right:NO])) return HIT_ROW + 10 * k;
            if (NSPointInRect(p, [self rowArrow:k right:YES])) return HIT_ROW + 10 * k + 2;
            if (NSPointInRect(p, [self rowRect:k])) return HIT_ROW + 10 * k + 1;
        }
        return NSPointInRect(p, self.panelRect) ? HIT_NONE : HIT_BACK;    /* a click outside the panel closes it */
    }
    for (NSInteger i = 0; i < 2; i++) if (NSPointInRect(p, [self cardRect:i])) return HIT_CARD + i;
    for (NSInteger k = 0; k < (NSInteger)self.tiles.count; k++) {
        if (!NSPointInRect(p, [self tileRect:k])) continue;
        if (self.tiles[k].activate) return HIT_TILE + 10 * k + 1;
        if (NSPointInRect(p, [self arrow:k right:NO])) return HIT_TILE + 10 * k;
        if (NSPointInRect(p, [self arrow:k right:YES])) return HIT_TILE + 10 * k + 2;
        return HIT_TILE + 10 * k + 1;
    }
    return HIT_NONE;
}
- (void)updateTrackingAreas
{
    for (NSTrackingArea *t in self.trackingAreas) [self removeTrackingArea:t];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:self userInfo:nil]];
}
- (void)mouseMoved:(NSEvent *)e { NSInteger h = [self hit:[self convertPoint:e.locationInWindow fromView:nil]]; if (h != self.hover) { self.hover = h; self.needsDisplay = YES; } }
- (void)mouseExited:(NSEvent *)e { self.hover = HIT_NONE; self.needsDisplay = YES; }
- (void)mouseDown:(NSEvent *)e
{
    NSInteger h = [self hit:[self convertPoint:e.locationInWindow fromView:nil]];
    if (h >= HIT_CARD && h < HIT_CARD + 2 && e.clickCount == 2) { self.game = h - HIT_CARD; if (self.onPlay) self.onPlay(); return; }   /* double-click: play that one */
    self.pressed = h; self.needsDisplay = YES;
}
- (void)mouseUp:(NSEvent *)e
{
    NSInteger h = [self hit:[self convertPoint:e.locationInWindow fromView:nil]], p = self.pressed; self.pressed = HIT_NONE; self.needsDisplay = YES;
    if (h != p || h == HIT_NONE) return;
    if (h == HIT_PLAY) { if (self.onPlay) self.onPlay(); return; }
    if (h == HIT_QUIT) { if (self.onQuit) self.onQuit(); return; }
    if (h == HIT_BACK) { [self closePanel]; return; }
    if (h >= HIT_ROW) { NSInteger k = (h - HIT_ROW) / 10; self.rowFocus = k; [self stepRow:k by:(h - HIT_ROW) % 10 == 0 ? -1 : 1]; return; }
    if (h >= HIT_CARD && h < HIT_CARD + 2) { self.game = h - HIT_CARD; self.focus = -1; if (self.onChange) self.onChange(); return; }
    NSInteger k = (h - HIT_TILE) / 10, part = (h - HIT_TILE) % 10; self.focus = k;
    [self step:k by:part == 0 ? -1 : 1];
}
- (void)step:(NSInteger)k by:(NSInteger)dir
{
    if (k < 0) { self.game = (self.game + 2 + dir) % 2; self.needsDisplay = YES; if (self.onChange) self.onChange(); return; }
    HWTile *t = self.tiles[k];
    if (t.activate) { t.activate(); self.needsDisplay = YES; return; }
    NSInteger n = (NSInteger)t.choices.count; t.index = (t.index + dir + n) % n;
    self.needsDisplay = YES; if (self.onChange) self.onChange();
}
- (void)stepRow:(NSInteger)k by:(NSInteger)dir
{
    HWTile *t = self.rows[k]; NSInteger n = (NSInteger)t.choices.count; t.index = (t.index + dir + n) % n;
    self.needsDisplay = YES; if (self.onChange) self.onChange();
}
- (void)keyDown:(NSEvent *)e
{
    if (self.open) {
        NSInteger rn = (NSInteger)self.rows.count;
        switch (e.keyCode) {
        case 0x24: case 0x4c: case 0x35: [self closePanel]; return;                          /* Return, Escape: back */
        case 0x7b: [self stepRow:self.rowFocus by:-1]; return;
        case 0x7c: [self stepRow:self.rowFocus by:1]; return;
        case 0x7e: self.rowFocus = (self.rowFocus + rn - 1) % rn; self.needsDisplay = YES; return;
        case 0x7d: case 0x30: self.rowFocus = (self.rowFocus + 1) % rn; self.needsDisplay = YES; return;
        }
        [super keyDown:e]; return;
    }
    NSInteger n = (NSInteger)self.tiles.count;
    switch (e.keyCode) {
    case 0x24: case 0x4c: if (self.focus >= 0 && self.tiles[self.focus].activate) { [self step:self.focus by:1]; return; }   /* Return */
                          if (self.onPlay) self.onPlay(); return;
    case 0x35: if (self.onQuit) self.onQuit(); return;                                       /* Escape */
    case 0x7b: [self step:self.focus by:-1]; return;                                         /* left / right: the focused row */
    case 0x7c: [self step:self.focus by:1]; return;
    case 0x7e: self.focus = self.focus < 0 ? n - 1 : self.focus - 1; self.needsDisplay = YES; return;   /* up / down: cards, then each tile */
    case 0x7d: case 0x30: self.focus = self.focus + 1 >= n ? -1 : self.focus + 1; self.needsDisplay = YES; return;   /* also Tab */
    }
    [super keyDown:e];
}

static void draw_text(NSString *s, NSFont *f, NSColor *c, NSRect r, NSTextAlignment al, NSShadow *sh, CGFloat kern)
{
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.alignment = al; ps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSMutableDictionary *a = [@{NSFontAttributeName: f, NSForegroundColorAttributeName: c, NSParagraphStyleAttributeName: ps, NSKernAttributeName: @(kern)} mutableCopy];
    if (sh) a[NSShadowAttributeName] = sh;
    [s drawInRect:r withAttributes:a];
}
static void brackets(NSRect r, CGFloat len, CGFloat w, NSColor *c)     /* corner marks, like the game's selection brackets */
{
    [c setStroke]; NSBezierPath *p = [NSBezierPath bezierPath]; p.lineWidth = w; p.lineCapStyle = NSLineCapStyleSquare;
    CGFloat x0 = r.origin.x, y0 = r.origin.y, x1 = NSMaxX(r), y1 = NSMaxY(r);
    [p moveToPoint:NSMakePoint(x0, y0 + len)]; [p lineToPoint:NSMakePoint(x0, y0)]; [p lineToPoint:NSMakePoint(x0 + len, y0)];
    [p moveToPoint:NSMakePoint(x1 - len, y0)]; [p lineToPoint:NSMakePoint(x1, y0)]; [p lineToPoint:NSMakePoint(x1, y0 + len)];
    [p moveToPoint:NSMakePoint(x1, y1 - len)]; [p lineToPoint:NSMakePoint(x1, y1)]; [p lineToPoint:NSMakePoint(x1 - len, y1)];
    [p moveToPoint:NSMakePoint(x0 + len, y1)]; [p lineToPoint:NSMakePoint(x0, y1)]; [p lineToPoint:NSMakePoint(x0, y1 - len)];
    [p stroke];
}
static void fill_over(NSRect r) { NSRectFillUsingOperation(r, NSCompositingOperationSourceOver); }
/* an image filling a rectangle, aspect kept (cropped); `keep` = the share of the image's height to use, from the top */
static void draw_cover(NSImage *img, NSRect r, CGFloat fraction, CGFloat keep, CGFloat focusX)
{
    NSSize is = img.size; is.height *= keep;
    CGFloat sc = MAX(r.size.width / is.width, r.size.height / is.height), w = r.size.width / sc, h = r.size.height / sc;
    NSRect from = NSMakeRect((img.size.width - w) * focusX, img.size.height - h - (is.height - h) / 2, w, h);   /* NSImage rows: bottom-up */
    [img drawInRect:r fromRect:from operation:NSCompositingOperationSourceOver fraction:fraction respectFlipped:YES hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
}
static void draw_fit(NSImage *img, NSRect r, CGFloat fraction)           /* aspect kept, inside r, centred */
{
    CGFloat sc = MIN(r.size.width / img.size.width, r.size.height / img.size.height), w = img.size.width * sc, h = img.size.height * sc;
    [img drawInRect:NSMakeRect(NSMidX(r) - w / 2, NSMidY(r) - h / 2, w, h) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:fraction respectFlipped:YES hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
}
- (void)drawButton:(NSRect)r title:(NSString *)title code:(NSInteger)code primary:(BOOL)primary
{
    BOOL hot = self.hover == code, down = self.pressed == code;
    if (primary) {
        NSGradient *g = [[NSGradient alloc] initWithStartingColor:[accent() colorWithAlphaComponent:down ? 0.75 : hot ? 0.62 : 0.48] endingColor:[accent_dim() colorWithAlphaComponent:down ? 0.8 : 0.6]];
        [g drawInRect:r angle:90];
        NSShadow *glow = [NSShadow new]; glow.shadowColor = [accent() colorWithAlphaComponent:hot ? 0.9 : 0.55]; glow.shadowBlurRadius = hot ? 18 : 12; glow.shadowOffset = NSZeroSize;
        [NSGraphicsContext saveGraphicsState]; [glow set]; [accent() setStroke]; NSBezierPath *b = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 0.5, 0.5)]; b.lineWidth = 1.2; [b stroke]; [NSGraphicsContext restoreGraphicsState];
    } else {
        [[NSColor colorWithSRGBRed:0.02 green:0.05 blue:0.09 alpha:down ? 0.95 : 0.75] setFill]; fill_over(r);
        [(hot ? accent() : accent_dim()) setStroke]; NSBezierPath *b = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 0.5, 0.5)]; b.lineWidth = 1; [b stroke];
    }
    brackets(NSInsetRect(r, -4, -4), 9, 2, primary || hot ? accent() : accent_dim());
    NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.8]; sh.shadowOffset = NSMakeSize(0, -1); sh.shadowBlurRadius = 2;
    NSFont *f = hwfont(primary ? 19 : 16);
    NSDictionary *at = @{NSFontAttributeName: f, NSForegroundColorAttributeName: primary || hot ? NSColor.whiteColor : text_c(), NSShadowAttributeName: sh, NSKernAttributeName: @5};
    NSSize sz = [title sizeWithAttributes:at];
    [title drawAtPoint:NSMakePoint(NSMidX(r) - sz.width / 2 + 2.5, round(NSMidY(r) - f.capHeight / 2 - (f.ascender - f.capHeight))) withAttributes:at];
}
- (void)drawCard:(NSInteger)i shadow:(NSShadow *)sh
{
    NSRect r = [self cardRect:i]; BOOL sel = self.game == i, hot = self.hover == HIT_CARD + i, foc = sel && self.focus == -1;
    if (sel) {                                                           /* the chosen game: a glow behind the card */
        NSShadow *glow = [NSShadow new]; glow.shadowColor = [accent() colorWithAlphaComponent:0.55]; glow.shadowBlurRadius = 26; glow.shadowOffset = NSZeroSize;
        [NSGraphicsContext saveGraphicsState]; [glow set]; [NSColor.blackColor setFill]; NSRectFill(r); [NSGraphicsContext restoreGraphicsState];
    }
    [NSColor.blackColor setFill]; NSRectFill(r);
    NSImage *shot = i < (NSInteger)self.shots.count ? self.shots[i] : nil;
    if (shot && shot.size.width > 16) draw_cover(shot, r, sel ? 1 : hot ? 0.85 : 0.62, 0.86, 0.5);   /* the Steam caption at the foot: left off */
    NSGradient *foot = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithWhite:0 alpha:0], 0.0, [NSColor colorWithWhite:0 alpha:0.35], 0.45, [NSColor colorWithSRGBRed:0 green:0.02 blue:0.05 alpha:0.92], 1.0, nil];
    [foot drawInRect:r angle:90];
    if (!sel) { [[NSColor colorWithSRGBRed:0.01 green:0.03 blue:0.07 alpha:hot ? 0.18 : 0.34] setFill]; fill_over(r); }
    NSImage *logo = (sel || hot) && i < (NSInteger)self.logosLit.count ? self.logosLit[i] : i < (NSInteger)self.logos.count ? self.logos[i] : nil;
    NSRect lr = NSMakeRect(r.origin.x + 18, NSMaxY(r) - 82, 330, 66);
    if (logo.size.width > 16) draw_fit(logo, lr, sel || hot ? 1 : 0.75);
    else draw_text(i ? @"HOMEWORLD 2 REMASTERED" : @"HOMEWORLD REMASTERED", hwfont(20), text_c(), NSMakeRect(lr.origin.x, lr.origin.y + 20, lr.size.width, 30), NSTextAlignmentLeft, sh, 3);
    [(sel ? accent() : hot ? [accent() colorWithAlphaComponent:0.7] : [accent_dim() colorWithAlphaComponent:0.7]) setStroke];
    NSBezierPath *b = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 0.5, 0.5)]; b.lineWidth = sel ? 1.5 : 1; [b stroke];
    if (sel) {
        brackets(NSInsetRect(r, -6, -6), foc ? 22 : 16, foc ? 3 : 2.5, accent());
        NSRect tag = NSMakeRect(NSMaxX(r) - 104, r.origin.y + 12, 92, 22);    /* "SELECTED" tag, top right */
        [[accent() colorWithAlphaComponent:0.9] setFill]; fill_over(tag);
        draw_text(@"SELECTED", hwfont(10.5), [NSColor colorWithSRGBRed:0.01 green:0.06 blue:0.12 alpha:1], NSMakeRect(tag.origin.x, tag.origin.y + 4, tag.size.width, 16), NSTextAlignmentCenter, nil, 2.5);
    }
}
- (void)drawArrow:(NSRect)r right:(BOOL)right lit:(BOOL)lit
{
    NSBezierPath *p = [NSBezierPath bezierPath]; CGFloat x0 = NSMidX(r) - 4, x1 = NSMidX(r) + 4, y0 = NSMidY(r) - 7, y1 = NSMidY(r) + 7, ym = NSMidY(r);
    if (right) { [p moveToPoint:NSMakePoint(x0, y0)]; [p lineToPoint:NSMakePoint(x1, ym)]; [p lineToPoint:NSMakePoint(x0, y1)]; }
    else { [p moveToPoint:NSMakePoint(x1, y0)]; [p lineToPoint:NSMakePoint(x0, ym)]; [p lineToPoint:NSMakePoint(x1, y1)]; }
    p.lineWidth = 2; p.lineCapStyle = NSLineCapStyleRound; p.lineJoinStyle = NSLineJoinStyleRound;
    [(lit ? NSColor.whiteColor : accent()) setStroke]; [p stroke];
}
- (void)drawTile:(NSInteger)k shadow:(NSShadow *)sh
{
    HWTile *t = self.tiles[k]; NSRect r = [self tileRect:k]; NSDictionary *c = t.choices[t.index];
    BOOL hot = self.hover >= HIT_TILE + 10 * k && self.hover <= HIT_TILE + 10 * k + 2, foc = self.focus == k, special = [c[@"accent"] boolValue];
    [[NSColor colorWithSRGBRed:0.02 green:0.05 blue:0.09 alpha:hot ? 0.88 : 0.72] setFill]; fill_over(r);
    [((hot || foc) ? accent() : [accent_dim() colorWithAlphaComponent:0.8]) setStroke];
    NSBezierPath *b = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 0.5, 0.5)]; b.lineWidth = 1; [b stroke];
    [((hot || foc) ? accent() : accent_dim()) setFill]; NSRectFill(NSMakeRect(r.origin.x, r.origin.y, r.size.width, 2));   /* a lit top edge */
    if (foc) brackets(NSInsetRect(r, -4, -4), 8, 2, accent());
    CGFloat ty = r.size.height < 60 ? 8 : 11;
    draw_text(t.title.uppercaseString, hwfont(10.5), (hot || foc) ? accent() : dim(), NSMakeRect(r.origin.x, r.origin.y + ty, r.size.width, 16), NSTextAlignmentCenter, nil, 3);
    NSColor *vc = special ? accent() : self.hover == HIT_TILE + 10 * k + 1 ? NSColor.whiteColor : text_c();
    draw_text(t.summary ? t.summary() : c[@"label"], body(15, YES), vc, NSMakeRect(r.origin.x + 17, r.origin.y + ty + 23, r.size.width - 34, 24), NSTextAlignmentCenter, sh, 0.2);
    if (!t.activate) {
        [self drawArrow:[self arrow:k right:NO] right:NO lit:self.hover == HIT_TILE + 10 * k];
        [self drawArrow:[self arrow:k right:YES] right:YES lit:self.hover == HIT_TILE + 10 * k + 2];
    }
}
- (void)drawPanel:(NSShadow *)sh                        /* Graphics Options: a panel over the window, a row per setting */
{
    [[NSColor colorWithSRGBRed:0 green:0.01 blue:0.03 alpha:0.62] setFill]; fill_over(NSMakeRect(0, 0, W, H - 92));
    NSRect p = self.panelRect;
    NSShadow *glow = [NSShadow new]; glow.shadowColor = [accent() colorWithAlphaComponent:0.35]; glow.shadowBlurRadius = 30; glow.shadowOffset = NSZeroSize;
    [NSGraphicsContext saveGraphicsState]; [glow set]; [[NSColor colorWithSRGBRed:0.02 green:0.04 blue:0.08 alpha:0.97] setFill]; NSRectFill(p); [NSGraphicsContext restoreGraphicsState];
    [accent_dim() setStroke]; NSBezierPath *bp = [NSBezierPath bezierPathWithRect:NSInsetRect(p, 0.5, 0.5)]; bp.lineWidth = 1; [bp stroke];
    brackets(NSInsetRect(p, -6, -6), 22, 3, accent());
    draw_text(@"GRAPHICS OPTIONS", hwfont(20), accent(), NSMakeRect(p.origin.x, p.origin.y + 22, p.size.width, 28), NSTextAlignmentCenter, sh, 6);
    [[accent_dim() colorWithAlphaComponent:0.6] setFill]; fill_over(NSMakeRect(p.origin.x + 30, p.origin.y + 60, p.size.width - 60, 1));
    for (NSInteger k = 0; k < (NSInteger)self.rows.count; k++) {
        HWTile *t = self.rows[k]; NSRect r = [self rowRect:k]; NSDictionary *c = t.choices[t.index];
        BOOL hot = self.hover >= HIT_ROW + 10 * k && self.hover <= HIT_ROW + 10 * k + 2, foc = self.rowFocus == k;
        if (foc || hot) { [[accent() colorWithAlphaComponent:foc ? 0.12 : 0.07] setFill]; fill_over(NSInsetRect(r, -10, -2));
                          if (foc) { [accent() setFill]; NSRectFill(NSMakeRect(r.origin.x - 10, r.origin.y - 2, 3, r.size.height + 4)); } }
        draw_text(t.title.uppercaseString, hwfont(16), foc || hot ? accent() : text_c(), NSMakeRect(r.origin.x + 6, r.origin.y + 8, 260, 24), NSTextAlignmentLeft, sh, 3);
        [self drawArrow:[self rowArrow:k right:NO] right:NO lit:self.hover == HIT_ROW + 10 * k];
        [self drawArrow:[self rowArrow:k right:YES] right:YES lit:self.hover == HIT_ROW + 10 * k + 2];
        draw_text(c[@"label"], body(18, YES), self.hover == HIT_ROW + 10 * k + 1 ? NSColor.whiteColor : text_c(), [self rowValue:k], NSTextAlignmentCenter, sh, 0.3);
        draw_text(c[@"note"] ?: @"", body(12, NO), dim(), NSMakeRect(r.origin.x + 6, r.origin.y + 32, r.size.width - 12, 18), NSTextAlignmentRight, nil, 0);
        if (k + 1 < (NSInteger)self.rows.count) { [[accent_dim() colorWithAlphaComponent:0.3] setFill]; fill_over(NSMakeRect(r.origin.x, NSMaxY(r) + 4, r.size.width, 1)); }
    }
}
- (NSString *)currentHint
{
    if (self.open) return @"←/→ change  ·  ↑/↓ move  ·  Return or Esc goes back";
    NSInteger k = -1;
    if (self.hover >= HIT_TILE && self.hover < HIT_ROW) k = (self.hover - HIT_TILE) / 10; else if (self.hover == HIT_NONE && self.focus >= 0) k = self.focus;
    if (k >= 0 && k < (NSInteger)self.tiles.count) { HWTile *t = self.tiles[k]; return t.choices[t.index][@"note"]; }
    if (self.hover >= HIT_CARD && self.hover < HIT_CARD + 2)
        return self.hover == HIT_CARD ? @"The Kushan exile, remastered: the original campaign. Double-click to play." : @"The Hiigaran war against the Vaygr, remastered. Double-click to play.";
    return self.hint;
}
- (void)drawRect:(NSRect)dirty
{
    NSRect b = self.bounds;
    NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.85]; sh.shadowOffset = NSMakeSize(0, -1); sh.shadowBlurRadius = 3;
    [[NSColor colorWithSRGBRed:0.01 green:0.02 blue:0.05 alpha:1] setFill]; NSRectFill(b);
    if (self.backdrop) draw_cover(self.backdrop, NSMakeRect(0, 0, W, 660), 1, 1, 0.5);   /* Aspyr's starfield, its horizon line above the cards */
    NSGradient *vig = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithWhite:0 alpha:0], 0.0, [NSColor colorWithWhite:0 alpha:0.55], 1.0, nil];
    [vig drawInRect:b relativeCenterPosition:NSMakePoint(0, 0.2)];
    /* the collection's logo, the subtitle under it */
    if (self.collection.size.width > 16) {
        NSShadow *glow = [NSShadow new]; glow.shadowColor = [accent() colorWithAlphaComponent:0.45]; glow.shadowOffset = NSZeroSize; glow.shadowBlurRadius = 22;
        [NSGraphicsContext saveGraphicsState]; [glow set]; draw_fit(self.collection, NSMakeRect(180, 54, 600, 100), 1); [NSGraphicsContext restoreGraphicsState];
    } else draw_text(@"HOMEWORLD", hwfont(64), text_c(), NSMakeRect(0, 60, W, 80), NSTextAlignmentCenter, sh, 10);
    draw_text(@"NATIVE FOR APPLE SILICON  ·  METAL", hwfont(11.5), accent(), NSMakeRect(0, 168, W, 18), NSTextAlignmentCenter, sh, 5);
    draw_text(@"CHOOSE YOUR CAMPAIGN", hwfont(10.5), dim(), NSMakeRect(60, 222, 400, 16), NSTextAlignmentLeft, sh, 3.5);
    for (NSInteger i = 0; i < 2; i++) [self drawCard:i shadow:sh];
    NSInteger hov = self.hover, foc = self.focus; if (self.open) self.hover = self.focus = HIT_NONE;   /* under the panel: nothing lit */
    for (NSInteger k = 0; k < (NSInteger)self.tiles.count; k++) [self drawTile:k shadow:sh];
    self.hover = hov; self.focus = foc;
    [[accent_dim() colorWithAlphaComponent:0.5] setFill]; fill_over(NSMakeRect(60, H - 92, 840, 1));
    if (self.open) [self drawPanel:sh];
    NSString *hint = [self currentHint];
    if (hint.length) draw_text(hint, body(12.5, NO), dim(), NSMakeRect(60, H - 56, 400, 36), NSTextAlignmentLeft, sh, 0.2);
    [self drawButton:self.quitRect title:self.open ? @"BACK" : @"QUIT" code:self.open ? HIT_BACK : HIT_QUIT primary:NO];
    [self drawButton:self.playRect title:@"PLAY" code:HIT_PLAY primary:YES];
}
@end

// ---------------------------------------------------------------- updates (the Dungeon Siege ports' updater)
enum { UPD_NONE, UPD_CHECKING, UPD_CURRENT, UPD_AVAILABLE, UPD_ERROR };
static NSString *short_commit(NSString *c) { return c.length > 7 ? [c substringToIndex:7] : (c ?: @""); }
static NSString *update_check(NSString *updater)     /* the newest commit of the branch on GitHub; nil if it can't be reached */
{
    NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/bin/bash"]; t.arguments = @[updater, @"--check"];
    NSPipe *p = [NSPipe pipe]; t.standardOutput = p; t.standardError = [NSFileHandle fileHandleWithNullDevice];
    if (![t launchAndReturnError:nil]) return nil;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{ if (t.running) [t terminate]; });
    NSData *d = [p.fileHandleForReading readDataToEndOfFile]; [t waitUntilExit];
    NSString *out = [[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return t.terminationStatus == 0 && out.length >= 40 ? [out substringToIndex:40] : nil;
}
static NSArray<NSDictionary *> *update_choice(int state, NSString *branch, NSString *built, NSString *latest)
{
    NSString *label = @"Unavailable", *note = @"This copy wasn't installed from the GitHub repository"; BOOL lit = NO;
    switch (state) {
    case UPD_CHECKING: label = @"Checking…"; note = [NSString stringWithFormat:@"Asking GitHub for the newest %@", branch]; break;
    case UPD_CURRENT: label = @"Up to Date"; note = [NSString stringWithFormat:@"%@ %@ · click to check again", branch, short_commit(built)]; break;
    case UPD_AVAILABLE: label = @"Update Ready"; lit = YES; note = [NSString stringWithFormat:@"%@ → %@: click to install", short_commit(built), short_commit(latest)]; break;
    case UPD_ERROR: label = @"Can’t Check"; note = @"GitHub unreachable or not signed in: click to retry"; break;
    }
    return @[@{@"value": @"", @"label": label, @"note": note, @"accent": @(lit)}];
}
static NSString *update_phase(NSString *line, NSString *branch)
{
    if ([line hasPrefix:@"== fetching"]) return @"Downloading the update from GitHub";
    if ([line hasPrefix:[NSString stringWithFormat:@"== %@:", branch]]) return [@"Got " stringByAppendingString:[line substringFromIndex:branch.length + 4]];
    if ([line hasPrefix:@"== recompiling"]) return @"Rebuilding from your game: translating it to native code (1 of 2)";
    if ([line hasPrefix:@"== building"]) return @"Installing the new app (2 of 2)";
    return nil;
}
@interface HWBlockTarget : NSObject
@property (copy) void (^fire)(void);
- (void)clicked:(id)b;
@end
@implementation HWBlockTarget
- (void)clicked:(id)b { (void)b; if (self.fire) self.fire(); }
@end
static BOOL update_run(NSWindow *parent, NSString *updater, NSString *branch, NSString *logPath)   /* modal progress; YES when rebuilt */
{
    NSPanel *pn = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 520, 170) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    pn.title = @"Updating";
    NSTextField *title = [NSTextField labelWithString:@"Updating to the newest version"]; title.font = [NSFont boldSystemFontOfSize:14]; title.frame = NSMakeRect(20, 128, 480, 22);
    NSTextField *status = [NSTextField wrappingLabelWithString:@"Starting…"]; status.frame = NSMakeRect(20, 64, 480, 56);
    NSProgressIndicator *spin = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 44, 480, 12)]; spin.indeterminate = YES; [spin startAnimation:nil];
    NSTextField *time = [NSTextField labelWithString:@"This takes a few minutes. Your profiles, saves and settings are kept."]; time.font = [NSFont systemFontOfSize:11]; time.textColor = NSColor.secondaryLabelColor; time.frame = NSMakeRect(20, 14, 370, 18);
    NSButton *btn = [NSButton buttonWithTitle:@"Close" target:nil action:nil]; btn.frame = NSMakeRect(400, 8, 100, 30); btn.hidden = YES;
    for (NSView *x in @[title, status, spin, time, btn]) [pn.contentView addSubview:x];
    if (parent) [pn setFrameOrigin:NSMakePoint(NSMidX(parent.frame) - 260, NSMidY(parent.frame) - 85)]; else [pn center];
    [[NSFileManager defaultManager] createFileAtPath:logPath contents:nil attributes:nil];
    NSFileHandle *log = [NSFileHandle fileHandleForWritingAtPath:logPath];
    NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/bin/bash"]; t.arguments = @[updater];
    NSPipe *pipe = [NSPipe pipe]; t.standardOutput = pipe; t.standardError = pipe;
    NSMutableString *pending = [NSMutableString new], *tail = [NSMutableString new];
    pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
        NSData *d = h.availableData; if (!d.length) return;
        [log writeData:d];
        NSString *chunk = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"";
        @synchronized (pending) {
            [pending appendString:chunk]; NSRange nl;
            while ((nl = [pending rangeOfString:@"\n"]).location != NSNotFound) {
                NSString *line = [pending substringToIndex:nl.location]; [pending deleteCharactersInRange:NSMakeRange(0, nl.location + 1)];
                [tail appendFormat:@"%@\n", line]; if (tail.length > 600) [tail deleteCharactersInRange:NSMakeRange(0, tail.length - 600)];
                NSString *ph = update_phase(line, branch); if (ph) on_main(^{ status.stringValue = ph; });
            }
        }
    };
    __block BOOL ok = NO;
    __attribute__((objc_precise_lifetime)) HWBlockTarget *bt = [HWBlockTarget new];   /* a button's target is weak: kept here */
    bt.fire = ^{ [NSApp stopModal]; }; btn.target = bt; btn.action = @selector(clicked:);
    t.terminationHandler = ^(NSTask *task) {
        int code = task.terminationStatus;
        on_main(^{
            pipe.fileHandleForReading.readabilityHandler = nil; [log closeFile];
            [spin stopAnimation:nil]; spin.hidden = YES; btn.hidden = NO;
            if (code == 0) { ok = YES; title.stringValue = @"Updated"; status.stringValue = @"The new version is installed. The app restarts when you press Restart."; btn.title = @"Restart"; }
            else {
                NSString *last; @synchronized (pending) { last = [[tail componentsSeparatedByString:@"\n"] componentsJoinedByString:@" "]; }
                title.stringValue = @"The update didn't finish";
                status.stringValue = [NSString stringWithFormat:@"Nothing was changed if it stopped before installing. Details: %@  (%@)", logPath, last.length > 160 ? [last substringFromIndex:last.length - 160] : last];
                time.stringValue = @"";
            }
        });
    };
    if (![t launchAndReturnError:nil]) { [log closeFile]; return NO; }
    [pn makeKeyAndOrderFront:nil]; [NSApp runModalForWindow:pn]; [pn orderOut:nil];
    return ok;
}

// ---------------------------------------------------------------- the window
static NSInteger index_of(NSArray<NSDictionary *> *c, id v, NSInteger dflt)
{ for (NSUInteger k = 0; k < c.count; k++) if ([c[k][@"value"] isEqual:v]) return (NSInteger)k; return dflt; }
static NSImage *image_at(NSArray<NSString *> *dirs, NSString *name)
{
    for (NSString *d in dirs) { NSImage *i = [[NSImage alloc] initWithContentsOfFile:[d stringByAppendingPathComponent:name]]; if (i.size.width > 16) return i; }
    return nil;
}

@interface Launcher : NSObject <NSApplicationDelegate>
@property (strong) NSWindow *window; @property (strong) HWLaunchView *view;
@property (strong) HWTile *display, *notch, *resolution, *vsync, *renderer, *movies, *saves, *updates;
@property NSString *gamePath, *dataPath;
@end

@implementation Launcher
- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    [d registerDefaults:@{@"game": @0, @"fullscreen": @NO, @"skipIntro": @NO, @"appleGL": @NO, @"notch": @"hide", @"retina": @NO, @"vsync": @YES}];
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    self.gamePath = info[@"HWGamePath"] ?: @"";
    self.dataPath = info[@"HWDataPath"] ?: [NSHomeDirectory() stringByAppendingPathComponent:@"Games/HomeworldNative"];
    NSString *res = [self.gamePath stringByAppendingPathComponent:@"Contents/Resources"];
    NSArray *artDirs = @[[self.dataPath stringByAppendingPathComponent:@"art"], [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"art"] ?: @"", res];

    HWLaunchView *v = [[HWLaunchView alloc] initWithFrame:NSMakeRect(0, 0, W, H)]; self.view = v;
    v.hover = v.pressed = HIT_NONE; v.focus = -1; v.game = [d integerForKey:@"game"] == 1 ? 1 : 0;
    v.backdrop = image_at(@[res], @"HW-GG3-Background-Main.jpg");
    v.collection = image_at(@[res], @"HWRM_Logo_Large.png");
    NSImage *none = [NSImage new];
    v.shots = @[image_at(artDirs, @"shot_hw1.jpg") ?: image_at(@[res], @"HW-GG3-Background-Popup.jpg") ?: none,
                image_at(artDirs, @"shot_hw2.jpg") ?: image_at(@[res], @"HW-GG3-Background-Offline.jpg") ?: none];
    v.logos = @[image_at(@[res], @"HW1RM_Logo_Default.png") ?: none, image_at(@[res], @"HW2RM_Logo_Default.png") ?: none];
    v.logosLit = @[image_at(@[res], @"HW1RM_Logo_Hover.png") ?: none, image_at(@[res], @"HW2RM_Logo_Hover.png") ?: none];
    v.hint = @"←/→ choose · ↑/↓ settings · Return plays · Esc quits";

    self.display = [HWTile new]; self.display.title = @"Display";
    self.display.choices = @[@{@"value": @NO, @"label": @"Window", @"note": @"A window on the desktop; ⌘-Tab and the Dock work as usual"},
                             @{@"value": @YES, @"label": @"Full Screen", @"note": @"The whole display, the Dock and menu bar out of the way: turns on Game Mode"}];
    self.display.index = index_of(self.display.choices, @([d boolForKey:@"fullscreen"]), 0);
    /* a display with a notch: full screen below the camera housing, or over the whole display around it (HW_NOTCH) */
    CGFloat notchH = 0; if (@available(macOS 12.0, *)) notchH = NSScreen.mainScreen.safeAreaInsets.top;
    self.notch = [HWTile new]; self.notch.title = @"Notch";
    self.notch.choices = @[@{@"value": @"hide", @"label": @"Below It", @"note": @"Full screen below the camera, a black band beside it"},
                           @{@"value": @"around", @"label": @"Around It", @"note": @"Full screen on the whole display, the notch over the picture's top"}];
    self.notch.index = index_of(self.notch.choices, [d stringForKey:@"notch"], 0);
    /* Retina: the game draws every pixel of the display (sharper; about 2.5x the GPU time, still within a 120 Hz frame on
     * an M1 Pro); its HUD is drawn at a fixed pixel size, so it comes out smaller. Standard: the size macOS gives in points. */
    CGFloat scale = NSScreen.mainScreen.backingScaleFactor; NSSize pts = NSScreen.mainScreen.frame.size;
    self.resolution = [HWTile new]; self.resolution.title = @"Resolution";
    self.resolution.choices = @[@{@"value": @NO, @"label": @"Standard", @"note": [NSString stringWithFormat:@"The display's size in points (%.0f × %.0f full screen), scaled up by macOS; HUD at its designed size", pts.width, pts.height]},
                                @{@"value": @YES, @"label": @"Retina", @"note": [NSString stringWithFormat:@"Every pixel (%.0f × %.0f full screen): sharpest; the HUD comes out smaller", pts.width * scale, pts.height * scale]}];
    self.resolution.index = index_of(self.resolution.choices, @([d boolForKey:@"retina"]), 0);
    /* VSync: frames paced to the display (no tearing), or as fast as the Mac draws them */
    NSInteger hz = NSScreen.mainScreen.maximumFramesPerSecond; if (hz <= 0) hz = 60;
    self.vsync = [HWTile new]; self.vsync.title = @"VSync";
    self.vsync.choices = @[@{@"value": @YES, @"label": @"On", @"short": @"VSync", @"note": [NSString stringWithFormat:@"Frames paced to the display's %ld Hz: smooth, no tearing", (long)hz]},
                           @{@"value": @NO, @"label": @"Off", @"short": @"Uncapped", @"note": @"As many frames as the Mac can draw: lowest latency, may tear, more power"}];
    self.vsync.index = index_of(self.vsync.choices, @([d boolForKey:@"vsync"]), 0);
    self.renderer = [HWTile new]; self.renderer.title = @"Renderer";
    self.renderer.choices = @[@{@"value": @NO, @"label": @"Metal", @"note": @"The port's own renderer: the game's OpenGL translated to Metal (recommended)"},
                              @{@"value": @YES, @"label": @"Apple GL", @"note": @"macOS's own OpenGL: a fallback if something looks wrong on Metal"}];
    self.renderer.index = index_of(self.renderer.choices, @([d boolForKey:@"appleGL"]), 0);
    self.movies = [HWTile new]; self.movies.title = @"Movies";
    self.movies.choices = @[@{@"value": @NO, @"label": @"Play All", @"note": @"The intros and the campaign's cinematics, as the game shipped"},
                            @{@"value": @YES, @"label": @"Skip All", @"note": @"Straight to the menu; also skips the mission cinematics"}];
    self.movies.index = index_of(self.movies.choices, @([d boolForKey:@"skipIntro"]), 0);
    NSString *userdata = [self.dataPath stringByAppendingPathComponent:@"userdata"];
    self.saves = [HWTile new]; self.saves.title = @"Saves";
    self.saves.choices = @[@{@"value": @"", @"label": @"Open", @"note": [@"Profiles, saves, settings and the game's log: " stringByAppendingString:[userdata stringByAbbreviatingWithTildeInPath]]}];
    self.saves.activate = ^{ [[NSFileManager defaultManager] createDirectoryAtPath:userdata withIntermediateDirectories:YES attributes:nil error:nil];
                             [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:userdata isDirectory:YES]]; };
    [self setupUpdates];
    v.rows = notchH > 0 ? @[self.display, self.notch, self.resolution, self.vsync, self.renderer] : @[self.display, self.resolution, self.vsync, self.renderer];
    HWTile *gfx = [HWTile new]; gfx.title = @"Graphics Options"; gfx.wide = YES;
    gfx.choices = @[@{@"value": @"", @"label": @"", @"note": notchH > 0 ? @"Display, notch, resolution, VSync and renderer" : @"Display, resolution, VSync and renderer"}];
    __weak Launcher *wl = self;
    gfx.summary = ^NSString * {
        Launcher *l = wl; NSMutableArray *a = [NSMutableArray array];
        for (HWTile *t in @[l.display, l.resolution, l.vsync, l.renderer]) [a addObject:t.choices[t.index][@"short"] ?: t.choices[t.index][@"label"]];
        return [a componentsJoinedByString:@"  ·  "];
    };
    __weak HWLaunchView *wv = v; gfx.activate = ^{ [wv openPanel]; };
    v.tiles = @[gfx, self.movies, self.saves, self.updates];
    __weak Launcher *ws = self;
    v.onPlay = ^{ [ws play]; };
    v.onQuit = ^{ [NSApp terminate:nil]; };
    v.onChange = ^{ [ws save]; };

    const char *shot = getenv("HW_LAUNCHER_SHOT");
    if (shot) {                                                          /* development: the window as an image */
        if (getenv("HW_LAUNCHER_HOVER")) v.hover = atoi(getenv("HW_LAUNCHER_HOVER"));
        if (getenv("HW_LAUNCHER_GAME")) v.game = atoi(getenv("HW_LAUNCHER_GAME"));
        if (getenv("HW_LAUNCHER_OPEN")) v.open = YES;
        NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds]; [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(shot) atomically:YES];
        exit(0);
    }
    NSWindow *w = [[NSWindow alloc] initWithContentRect:v.frame styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskFullSizeContentView
                                                backing:NSBackingStoreBuffered defer:NO];
    w.releasedWhenClosed = NO; w.titlebarAppearsTransparent = YES; w.titleVisibility = NSWindowTitleHidden; w.movableByWindowBackground = YES;
    w.title = @"Homeworld Remastered Collection"; w.backgroundColor = NSColor.blackColor; w.contentView = v;
    self.window = w; [w center]; [w makeKeyAndOrderFront:nil]; [w makeFirstResponder:v];
    [NSApp activateIgnoringOtherApps:YES];
    if (getenv("HW_LAUNCHER_PLAY")) dispatch_async(dispatch_get_main_queue(), ^{ [ws play]; });   /* development: Play at once */
}
- (void)setupUpdates
{
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    NSString *updater = [NSBundle.mainBundle.executablePath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"update"];
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:updater]) updater = nil;
    NSString *branch = info[@"HWBranch"] ?: @"main", *built = info[@"HWBuilt"] ?: @"";
    HWTile *u = [HWTile new]; u.title = @"Updates"; self.updates = u;
    __block int state = updater ? UPD_CHECKING : UPD_NONE; __block NSString *latest = nil;
    u.choices = update_choice(state, branch, built, nil);
    __weak HWTile *wu = u; __weak Launcher *ws = self;
    void (^check)(void) = ^{
        if (!updater) return;
        state = UPD_CHECKING; wu.choices = update_choice(state, branch, built, nil); ws.view.needsDisplay = YES;
        void (^done)(NSString *) = ^(NSString *l) {
            latest = l; state = !l ? UPD_ERROR : [l isEqualToString:built] ? UPD_CURRENT : UPD_AVAILABLE;
            wu.choices = update_choice(state, branch, built, l); ws.view.needsDisplay = YES;
        };
        if (getenv("HW_LAUNCHER_SHOT")) { done(update_check(updater)); return; }
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ NSString *l = update_check(updater); on_main(^{ done(l); }); });
    };
    check();
    u.activate = ^{
        if (state == UPD_CHECKING || state == UPD_NONE) return;
        if (state != UPD_AVAILABLE) { check(); return; }
        NSAlert *a = [NSAlert new];
        a.messageText = [NSString stringWithFormat:@"Update to the newest %@?", [branch isEqualToString:@"nightly"] ? @"Nightly" : @"version"];
        a.informativeText = [NSString stringWithFormat:@"The %@ branch (%@) is downloaded from GitHub and the app is rebuilt from your own copy of the game, "
                             "which takes a few minutes. Your profiles, saves and settings are kept. The app restarts when it is done.", branch, short_commit(latest)];
        [a addButtonWithTitle:@"Update"]; [a addButtonWithTitle:@"Cancel"];
        if ([a runModal] != NSAlertFirstButtonReturn) return;
        if (update_run(ws.window, updater, branch, [ws.dataPath stringByAppendingPathComponent:@"update.log"])) {
            NSString *app = NSBundle.mainBundle.bundlePath;                 /* the rebuilt app is at the same path */
            NSTask *o = [NSTask new]; o.executableURL = [NSURL fileURLWithPath:@"/usr/bin/open"]; o.arguments = @[@"-n", app];
            [o launchAndReturnError:nil]; [o waitUntilExit];
            exit(0);
        }
        check();
    };
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }
- (void)save
{
    NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
    [d setInteger:self.view.game forKey:@"game"];
    [d setBool:[self.display.choices[self.display.index][@"value"] boolValue] forKey:@"fullscreen"];
    [d setBool:[self.renderer.choices[self.renderer.index][@"value"] boolValue] forKey:@"appleGL"];
    [d setBool:[self.movies.choices[self.movies.index][@"value"] boolValue] forKey:@"skipIntro"];
    [d setObject:self.notch.choices[self.notch.index][@"value"] forKey:@"notch"];
    [d setBool:[self.resolution.choices[self.resolution.index][@"value"] boolValue] forKey:@"retina"];
    [d setBool:[self.vsync.choices[self.vsync.index][@"value"] boolValue] forKey:@"vsync"];
    [d synchronize];
}
- (void)play
{
    [self save];
    NSInteger g = self.view.game;
    if (![[NSFileManager defaultManager] fileExistsAtPath:self.gamePath]) {
        NSAlert *a = [NSAlert new]; a.messageText = @"The game files are missing";
        a.informativeText = [NSString stringWithFormat:@"Expected %@. Run install.sh again.", self.gamePath];
        [a runModal]; return;
    }
    NSString *exe = [NSBundle.mainBundle.executablePath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"hw2hd"];
    NSArray *args = g == 0 ? @[@"-dlccampaign", @"HW1Campaign.big", @"-campaign", @"HomeworldClassic", @"-moviepath", @"DataHW1Campaign"]
                           : @[@"-dlccampaign", @"HW2Campaign.big", @"-campaign", @"Ascension", @"-moviepath", @"DataHW2Campaign"];
    setenv("HW_FULLSCREEN", [self.display.choices[self.display.index][@"value"] boolValue] ? "1" : "0", 1);
    setenv("HW_VSYNC", [self.vsync.choices[self.vsync.index][@"value"] boolValue] ? "1" : "0", 1);
    setenv("HW_RETINA", [self.resolution.choices[self.resolution.index][@"value"] boolValue] ? "1" : "0", 1);
    setenv("HW_NOTCH", [self.notch.choices[self.notch.index][@"value"] UTF8String], 1);
    setenv("HW_GL", [self.renderer.choices[self.renderer.index][@"value"] boolValue] ? "apple" : "metal", 1);
    [self.window orderOut:nil];
    const char **argv = calloc(args.count + 4, sizeof *argv);
    argv[0] = exe.fileSystemRepresentation; argv[1] = self.gamePath.fileSystemRepresentation;
    for (NSUInteger k = 0; k < args.count; k++) argv[k + 2] = [args[k] UTF8String];
    if ([self.movies.choices[self.movies.index][@"value"] boolValue]) argv[args.count + 2] = "-noMovies";
    execv(argv[0], (char *const *)argv);
    NSAlert *a = [NSAlert new]; a.messageText = @"The game did not start"; [a runModal];
}
@end

int main(int argc, const char **argv)
{
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        Launcher *l = [Launcher new]; app.delegate = l;
        [app run];
    }
    return 0;
}
