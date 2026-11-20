/* mgl: OpenGL 4.1 core (the subset Homeworld Remastered calls) on Metal. See mgl.h for the coordinate conventions.
 *
 * Objects live in one GL namespace (the game shares everything between its contexts). Work is recorded into one
 * command buffer per frame; a render pass is open while draws go to the same framebuffer, and is closed by a
 * framebuffer change, a blit or the swap. Up to three frames are in flight; buffers and textures the GPU may still
 * be reading are versioned (buffers) or waited for (textures) when the game rewrites them. */
#define GL_SILENCE_DEPRECATION
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_metal.h>
#include "mgl.h"
#include "shader.h"

void hw_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) hw_log("mgl: " __VA_ARGS__)
#define FRAMES 3
#define VB_BASE 15                                     /* vertex attribute L uses Metal buffer index VB_BASE + L */
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT 0x8C4C
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT 0x8C4E
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE

/* ---------------------------------------------------------------- objects */
@interface MBuffer : NSObject { @public id<MTLBuffer> ver[6]; uint64_t verFrame[6]; int cur, nver; NSUInteger size; } @end
@implementation MBuffer @end
@interface MTex : NSObject {
@public id<MTLTexture> tex; GLenum target, internal; MTLPixelFormat fmt; int w, h, levels;
    GLint minF, magF, wrapS, wrapT, wrapR, compareMode, compareFunc, maxLevel, baseLevel; float aniso, lodBias;
    id<MTLSamplerState> sampler; uint64_t usedFrame; BOOL expandRGB, compressed;
    GLint swz[4]; id<MTLTexture> view;              /* GL_TEXTURE_SWIZZLE_*: a swizzled view for sampling */
} @end
@implementation MTex @end
typedef struct Attr { BOOL on; GLint size; GLenum type; BOOL norm; GLsizei stride; uint32_t offset, buffer; } Attr;
@interface MVao : NSObject { @public Attr a[16]; uint32_t element; } @end
@implementation MVao @end
@interface MFbo : NSObject { @public uint32_t color[8]; GLint colorLevel[8]; GLenum colorFace[8]; uint32_t depth, stencil; GLint depthLevel; GLenum drawBuf, readBuf; } @end
@implementation MFbo @end
@interface MShader : NSObject { @public GLenum type; char *src; BOOL ok; char log[4096]; MglShaderOut *out; id<MTLFunction> fn; } @end
@implementation MShader - (void)dealloc { free(src); if (out) { free(out->msl); free(out); } } @end
typedef struct Uni { char name[64]; int loc, count, basetype, columns, vecsize; int off[2], stride[2]; int sampler; } Uni;   /* off: -1 = not in that stage */
typedef struct Smp { int loc, unit, slot[2], dim, depth; } Smp;                                                                   /* slot: texture(n) per stage, -1 */
@interface MProgram : NSObject {
@public MShader *vs, *fs; BOOL linked; char log[4096];
    Uni *uni; int nuni, nloc; Smp smp[MGL_MAX_SAMPLERS * 2]; int nsmp;
    uint8_t *block[2]; unsigned blockSize[2]; int blockSlot[2];
    MglInput inputs[MGL_MAX_INPUTS]; int ninputs;
    id<MTLFunction> ffn; MglShaderOut *fout;            /* the fragment stage as linked */
} @end
@implementation MProgram - (void)dealloc { free(uni); free(block[0]); free(block[1]); if (fout) { free(fout->msl); free(fout); } } @end

static NSMutableArray *buffers, *textures, *vaos, *fbos, *shaders;   /* index = GL name; NSNull = free */
static NSMutableArray *programs;                                      /* shaders and programs share one name space */
static id get(NSMutableArray *a, uint32_t n) { return n < a.count && a[n] != (id)[NSNull null] ? a[n] : nil; }
static uint32_t put(NSMutableArray *a, id o)
{
    for (NSUInteger k = 1; k < a.count; k++) if (a[k] == (id)[NSNull null]) { a[k] = o; return (uint32_t)k; }
    if (!a.count) [a addObject:[NSNull null]];
    [a addObject:o]; return (uint32_t)(a.count - 1);
}
static void drop(NSMutableArray *a, uint32_t n) { if (n && n < a.count) a[n] = [NSNull null]; }

/* ---------------------------------------------------------------- state */
typedef struct Unit { uint32_t tex2d, cube; } Unit;
static struct {
    id<MTLDevice> dev; id<MTLCommandQueue> queue; id<MTLCommandBuffer> cb; id<MTLRenderCommandEncoder> enc;
    CAMetalLayer *layer; SDL_MetalView view; SDL_Window *win;
    dispatch_semaphore_t inflight; uint64_t frame; volatile uint64_t done;
    id<MTLTexture> defColor, defDepth; int defW, defH;
    /* GL state */
    GLenum error; uint32_t prog, vao, arrayBuf, unpackBuf, packBuf, drawFbo, readFbo; int activeUnit; Unit units[32];
    BOOL blend, depthTest, cull, scissorTest, polyOffset, srgb;
    GLenum bsrc, bdst, bsrcA, bdstA, beq, beqA; float bcolor[4];
    GLenum depthFunc; BOOL depthMask; BOOL cmask[4]; GLenum cullFace, frontFace, polyMode;
    float offFactor, offUnits; GLint vp[4], sc[4]; float clearC[4]; double clearD; int clearS;
    GLint unpackAlign, unpackRow, packAlign;
    /* pass */
    uint32_t passFbo; BOOL passOpen; int pendingClear; uint32_t passKey[12];
    NSMutableDictionary *pipelines, *dsStates, *samplers; id<MTLRenderPipelineState> flipPipe;
    id<MTLBuffer> ring[FRAMES]; NSUInteger ringOff;
    MVao *defaultVao; id<MTLBuffer> zeroAttr;
    uint32_t rendererID; NSMutableArray<NSString *> *extensions; char version[64], renderer[128];
    id<MTLCommandBuffer> frameCB[FRAMES];
} S;
static void err(GLenum e) { if (!S.error) S.error = e; }
static volatile double gpu_ms; static volatile int gpu_n; static int stalls;      /* HW_LOG_FPS statistics */

/* ---------------------------------------------------------------- frames */
static void begin_frame(void)
{
    dispatch_semaphore_wait(S.inflight, DISPATCH_TIME_FOREVER);
    S.cb = [S.queue commandBuffer];
    uint64_t f = ++S.frame;
    S.frameCB[f % FRAMES] = S.cb;
    [S.cb addCompletedHandler:^(id<MTLCommandBuffer> b) { gpu_ms += (b.GPUEndTime - b.GPUStartTime) * 1000; gpu_n++; S.done = f; dispatch_semaphore_signal(S.inflight); }];
    S.ringOff = 0;
}
static void end_pass(void) { if (S.enc) { [S.enc endEncoding]; S.enc = nil; } S.passOpen = NO; }
static void wait_frame(uint64_t f)                     /* until frame f has finished on the GPU */
{
    if (f > S.done) stalls++;
    if (f > S.done && f == S.frame) {                  /* the current frame: submit it and start a new one */
        end_pass(); [S.cb commit]; [S.cb waitUntilCompleted]; begin_frame();
    }
    while (S.done < f) usleep(100);
}
static id<MTLBuffer> ring_alloc(NSUInteger n, NSUInteger *off)
{
    id<MTLBuffer> r = S.ring[S.frame % FRAMES];
    NSUInteger o = (S.ringOff + 255) & ~(NSUInteger)255;
    if (!r || o + n > r.length) {
        NSUInteger len = r ? r.length * 2 : (8u << 20); while (len < n) len *= 2;
        if (r && o + n > r.length && S.ringOff) { /* grow: the old one stays alive in the command buffer */ }
        r = [S.dev newBufferWithLength:len options:MTLResourceStorageModeShared];
        S.ring[S.frame % FRAMES] = r; o = 0;
    }
    S.ringOff = o + n; *off = o; return r;
}

/* ---------------------------------------------------------------- formats */
static MTLPixelFormat tex_format(GLenum internal, GLenum format, GLenum type, BOOL *expand)
{
    *expand = NO;
    switch (internal) {
    case GL_RGBA8: case GL_RGBA: return format == GL_BGRA ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
    case GL_SRGB8_ALPHA8: case GL_SRGB_ALPHA: return MTLPixelFormatRGBA8Unorm_sRGB;
    case GL_RGB8: case GL_RGB: *expand = YES; return MTLPixelFormatRGBA8Unorm;
    case GL_SRGB8: case GL_SRGB: *expand = YES; return MTLPixelFormatRGBA8Unorm_sRGB;
    case GL_R8: case GL_RED: return MTLPixelFormatR8Unorm;
    case GL_RG8: case GL_RG: return MTLPixelFormatRG8Unorm;
    case GL_R16F: return MTLPixelFormatR16Float;
    case GL_RG16F: return MTLPixelFormatRG16Float;
    case GL_RGBA16F: return MTLPixelFormatRGBA16Float;
    case GL_RGB16F: *expand = YES; return MTLPixelFormatRGBA16Float;
    case GL_R32F: return MTLPixelFormatR32Float;
    case GL_RG32F: return MTLPixelFormatRG32Float;
    case GL_RGBA32F: return MTLPixelFormatRGBA32Float;
    case GL_R11F_G11F_B10F: return MTLPixelFormatRG11B10Float;
    case GL_RGB10_A2: return MTLPixelFormatRGB10A2Unorm;
    case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32: case GL_DEPTH_COMPONENT32F:
        return MTLPixelFormatDepth32Float;
    case GL_DEPTH24_STENCIL8: case GL_DEPTH_STENCIL: case GL_DEPTH32F_STENCIL8: return MTLPixelFormatDepth32Float_Stencil8;
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: return MTLPixelFormatBC1_RGBA;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: return MTLPixelFormatBC2_RGBA;
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: return MTLPixelFormatBC3_RGBA;
    case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT: case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT: return MTLPixelFormatBC1_RGBA_sRGB;
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT: return MTLPixelFormatBC2_RGBA_sRGB;
    case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT: return MTLPixelFormatBC3_RGBA_sRGB;
    }
    LOG("texture format %04x/%04x/%04x: using RGBA8", internal, format, type);
    return MTLPixelFormatRGBA8Unorm;
}
static int fmt_bpp(GLenum format, GLenum type)
{
    int ch = format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_RGB || format == GL_BGR ? 3 : format == GL_RG ? 2 : 1;
    int sz = type == GL_FLOAT ? 4 : type == GL_HALF_FLOAT ? 2 : type == GL_UNSIGNED_SHORT ? 2 : 1;
    if (type == GL_UNSIGNED_INT_8_8_8_8_REV || type == GL_UNSIGNED_INT_8_8_8_8) return 4;
    if (format == GL_DEPTH_COMPONENT) return 4;
    return ch * sz;
}
static BOOL is_depth(MTLPixelFormat f) { return f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth32Float_Stencil8; }

/* ---------------------------------------------------------------- textures */
static MTex *bound_tex(GLenum target)
{
    Unit *u = &S.units[S.activeUnit];
    return get(textures, target == GL_TEXTURE_CUBE_MAP || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) ? u->cube : u->tex2d);
}
static void tex_alloc(MTex *t, int w, int h, MTLPixelFormat f, BOOL cube)
{
    int lv = 1; for (int m = w > h ? w : h; m > 1; m >>= 1) lv++;
    MTLTextureDescriptor *d = cube ? [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:f size:(NSUInteger)w mipmapped:YES]
                                   : [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:(NSUInteger)w height:(NSUInteger)h mipmapped:YES];
    d.usage = MTLTextureUsageShaderRead | (f >= MTLPixelFormatBC1_RGBA && f <= MTLPixelFormatBC7_RGBAUnorm_sRGB ? 0 : MTLTextureUsageRenderTarget);
    d.storageMode = is_depth(f) ? MTLStorageModePrivate : MTLStorageModeShared;
    if (is_depth(f)) d.mipmapLevelCount = 1, lv = 1;
    t->tex = [S.dev newTextureWithDescriptor:d]; t->view = nil; t->fmt = f; t->w = w; t->h = h; t->levels = lv;
}
static void tex_upload(MTex *t, GLenum target, int level, int x, int y, int w, int h, const void *px, GLenum format, GLenum type, BOOL compressed, size_t csize)
{
    if (S.unpackBuf) {                                  /* a pixel unpack buffer: px is an offset into it */
        MBuffer *b = get(buffers, S.unpackBuf); if (!b || !b->nver) return;
        px = (const uint8_t *)b->ver[b->cur].contents + (uintptr_t)px;
    }
    if (!t->tex || !px || w <= 0 || h <= 0) return;
    if (t->usedFrame > S.done) wait_frame(t->usedFrame);
    NSUInteger slice = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;
    if (level >= t->levels || t->tex.storageMode == MTLStorageModePrivate) return;
    if (compressed) {
        int bw = (w + 3) / 4, bytes = t->fmt == MTLPixelFormatBC1_RGBA || t->fmt == MTLPixelFormatBC1_RGBA_sRGB ? 8 : 16;
        (void)csize;
        [t->tex replaceRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h) mipmapLevel:(NSUInteger)level slice:slice
                    withBytes:px bytesPerRow:(NSUInteger)(bw * bytes) bytesPerImage:0];
        return;
    }
    int bpp = fmt_bpp(format, type), rowpx = S.unpackRow ? S.unpackRow : w;
    size_t rb = (size_t)rowpx * bpp; rb = (rb + (size_t)S.unpackAlign - 1) & ~((size_t)S.unpackAlign - 1);
    if (t->expandRGB && (format == GL_RGB || format == GL_BGR)) {                 /* 3 channels -> 4 */
        int cs = type == GL_HALF_FLOAT ? 2 : type == GL_FLOAT ? 4 : 1;
        uint8_t *o = malloc((size_t)w * h * 4 * cs);
        for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) {
            const uint8_t *s = (const uint8_t *)px + (size_t)r * rb + (size_t)c * 3 * cs; uint8_t *d = o + ((size_t)r * w + c) * 4 * cs;
            int bgr = format == GL_BGR;
            memcpy(d, s + (bgr ? 2 : 0) * cs, cs); memcpy(d + cs, s + cs, cs); memcpy(d + 2 * cs, s + (bgr ? 0 : 2) * cs, cs);
            if (cs == 1) d[3] = 255; else if (cs == 2) { uint16_t one = 0x3c00; memcpy(d + 6, &one, 2); } else { float one = 1; memcpy(d + 12, &one, 4); }
        }
        [t->tex replaceRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h) mipmapLevel:(NSUInteger)level slice:slice
                    withBytes:o bytesPerRow:(NSUInteger)w * 4 * cs bytesPerImage:0];
        free(o); return;
    }
    if (format == GL_BGRA && t->fmt == MTLPixelFormatRGBA8Unorm) {                   /* BGRA data into an RGBA texture: swizzle */
        uint8_t *o = malloc((size_t)w * h * 4);
        for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) { const uint8_t *s = (const uint8_t *)px + (size_t)r * rb + (size_t)c * 4; uint8_t *d = o + ((size_t)r * w + c) * 4; d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
        [t->tex replaceRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h) mipmapLevel:(NSUInteger)level slice:slice withBytes:o bytesPerRow:(NSUInteger)w * 4 bytesPerImage:0];
        free(o); return;
    }
    [t->tex replaceRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h) mipmapLevel:(NSUInteger)level slice:slice
                withBytes:px bytesPerRow:rb bytesPerImage:0];
}
static void mgl_glGenTextures(GLsizei n, GLuint *out) { for (int k = 0; k < n; k++) { MTex *t = [MTex new]; t->minF = GL_NEAREST_MIPMAP_LINEAR; t->magF = GL_LINEAR; t->wrapS = t->wrapT = t->wrapR = GL_REPEAT; t->maxLevel = 1000; t->compareFunc = GL_LEQUAL; t->aniso = 1; out[k] = put(textures, t); } }
static void mgl_glDeleteTextures(GLsizei n, const GLuint *t) { for (int k = 0; k < n; k++) { for (int u = 0; u < 32; u++) { if (S.units[u].tex2d == t[k]) S.units[u].tex2d = 0; if (S.units[u].cube == t[k]) S.units[u].cube = 0; } drop(textures, t[k]); } }
static void mgl_glActiveTexture(GLenum u) { S.activeUnit = (int)(u - GL_TEXTURE0) & 31; }
static void mgl_glBindTexture(GLenum target, GLuint t)
{
    if (t && !get(textures, t)) { while (textures.count <= t) [textures addObject:[NSNull null]]; MTex *x = [MTex new]; x->minF = GL_NEAREST_MIPMAP_LINEAR; x->magF = GL_LINEAR; x->wrapS = x->wrapT = x->wrapR = GL_REPEAT; x->maxLevel = 1000; x->aniso = 1; textures[t] = x; }
    MTex *x = get(textures, t); if (x && !x->target) x->target = target;
    if (target == GL_TEXTURE_CUBE_MAP) S.units[S.activeUnit].cube = t; else S.units[S.activeUnit].tex2d = t;
}
static void mgl_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *px)
{
    (void)border; MTex *t = bound_tex(target); if (!t) { err(GL_INVALID_OPERATION); return; }
    BOOL cube = target != GL_TEXTURE_2D, expand;
    MTLPixelFormat f = tex_format((GLenum)internal, format, type, &expand);
    if (level == 0 && (!t->tex || t->w != w || t->h != h || t->fmt != f)) {
        if (t->tex && t->usedFrame > S.done) wait_frame(t->usedFrame);
        tex_alloc(t, w, h, f, cube); t->sampler = nil;
    }
    t->internal = (GLenum)internal; t->expandRGB = expand;
    tex_upload(t, target, level, 0, 0, w, h, px, format, type, NO, 0);
}
static void mgl_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *px)
{ MTex *t = bound_tex(target); if (t) tex_upload(t, target, level, x, y, w, h, px, format, type, NO, 0); }
static void mgl_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border, GLsizei size, const void *data)
{
    (void)border; MTex *t = bound_tex(target); if (!t) return;
    BOOL e; MTLPixelFormat f = tex_format(internal, 0, 0, &e);
    if (level == 0 && (!t->tex || t->w != w || t->h != h || t->fmt != f)) { if (t->tex && t->usedFrame > S.done) wait_frame(t->usedFrame); tex_alloc(t, w, h, f, target != GL_TEXTURE_2D); t->sampler = nil; }
    t->internal = internal; t->compressed = YES;
    tex_upload(t, target, level, 0, 0, w, h, data, 0, 0, YES, (size_t)size);
}
static MTLTextureSwizzle swz(GLint c) { return c == GL_RED ? MTLTextureSwizzleRed : c == GL_GREEN ? MTLTextureSwizzleGreen : c == GL_BLUE ? MTLTextureSwizzleBlue : c == GL_ALPHA ? MTLTextureSwizzleAlpha : c == GL_ZERO ? MTLTextureSwizzleZero : MTLTextureSwizzleOne; }
static id<MTLTexture> sample_view(MTex *t)              /* the texture as shaders see it (with the swizzle applied) */
{
    if (!t->swz[0] || (t->swz[0] == GL_RED && t->swz[1] == GL_GREEN && t->swz[2] == GL_BLUE && t->swz[3] == GL_ALPHA)) return t->tex;
    if (!t->view || t->view.parentTexture != t->tex) {
        MTLTextureSwizzleChannels c = MTLTextureSwizzleChannelsMake(swz(t->swz[0]), swz(t->swz[1]), swz(t->swz[2]), swz(t->swz[3]));
        t->view = [t->tex newTextureViewWithPixelFormat:t->tex.pixelFormat textureType:t->tex.textureType levels:NSMakeRange(0, t->tex.mipmapLevelCount)
                                                 slices:NSMakeRange(0, t->tex.textureType == MTLTextureTypeCube ? 6 : 1) swizzle:c];
    }
    return t->view;
}
static void tex_param(GLenum target, GLenum p, float v)
{
    MTex *t = bound_tex(target); if (!t) return;
    int i = (int)v;
    if (p >= GL_TEXTURE_SWIZZLE_R && p <= GL_TEXTURE_SWIZZLE_A) {
        if (!t->swz[0]) { t->swz[0] = GL_RED; t->swz[1] = GL_GREEN; t->swz[2] = GL_BLUE; t->swz[3] = GL_ALPHA; }
        t->swz[p - GL_TEXTURE_SWIZZLE_R] = i; t->view = nil; return;
    }
    switch (p) {
    case GL_TEXTURE_MIN_FILTER: t->minF = i; break; case GL_TEXTURE_MAG_FILTER: t->magF = i; break;
    case GL_TEXTURE_WRAP_S: t->wrapS = i; break; case GL_TEXTURE_WRAP_T: t->wrapT = i; break; case GL_TEXTURE_WRAP_R: t->wrapR = i; break;
    case GL_TEXTURE_COMPARE_MODE: t->compareMode = i; break; case GL_TEXTURE_COMPARE_FUNC: t->compareFunc = i; break;
    case GL_TEXTURE_MAX_LEVEL: t->maxLevel = i; break; case GL_TEXTURE_BASE_LEVEL: t->baseLevel = i; break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: t->aniso = v; break; case GL_TEXTURE_LOD_BIAS: t->lodBias = v; break;
    default: return;
    }
    t->sampler = nil;
}
static void mgl_glTexParameteri(GLenum target, GLenum p, GLint v) { tex_param(target, p, (float)v); }
static void mgl_glTexParameterf(GLenum target, GLenum p, GLfloat v) { tex_param(target, p, v); }
static void mgl_glTexParameteriv(GLenum target, GLenum p, const GLint *v)
{
    if (p == GL_TEXTURE_SWIZZLE_RGBA) { for (int k = 0; k < 4; k++) tex_param(target, GL_TEXTURE_SWIZZLE_R + (GLenum)k, (float)v[k]); return; }
    tex_param(target, p, (float)v[0]);
}
static void mgl_glTexParameterfv(GLenum target, GLenum p, const GLfloat *v) { tex_param(target, p, v[0]); }
static void mgl_glGetTexParameteriv(GLenum target, GLenum p, GLint *v)
{
    MTex *t = bound_tex(target); if (!t) { *v = 0; return; }
    switch (p) {
    case GL_TEXTURE_MIN_FILTER: *v = t->minF; break; case GL_TEXTURE_MAG_FILTER: *v = t->magF; break;
    case GL_TEXTURE_WRAP_S: *v = t->wrapS; break; case GL_TEXTURE_WRAP_T: *v = t->wrapT; break;
    case GL_TEXTURE_MAX_LEVEL: *v = t->maxLevel; break; case GL_TEXTURE_BASE_LEVEL: *v = t->baseLevel; break;
    default: *v = 0;
    }
}
static void mgl_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum p, GLint *v)
{
    MTex *t = bound_tex(target); if (!t) { *v = 0; return; }
    if (p == GL_TEXTURE_WIDTH) *v = t->w >> level; else if (p == GL_TEXTURE_HEIGHT) *v = t->h >> level; else if (p == GL_TEXTURE_INTERNAL_FORMAT) *v = (GLint)t->internal; else *v = 0;
}
static void mgl_glGenerateMipmap(GLenum target)
{
    MTex *t = bound_tex(target); if (!t || !t->tex || t->levels < 2) return;
    end_pass(); id<MTLBlitCommandEncoder> b = [S.cb blitCommandEncoder]; [b generateMipmapsForTexture:t->tex]; [b endEncoding];
    t->usedFrame = S.frame;
}
static void mgl_glPixelStorei(GLenum p, GLint v)
{ if (p == GL_UNPACK_ALIGNMENT) S.unpackAlign = v; else if (p == GL_UNPACK_ROW_LENGTH) S.unpackRow = v; else if (p == GL_PACK_ALIGNMENT) S.packAlign = v; }
static MTLSamplerAddressMode wrap(GLint w)
{ return w == GL_CLAMP_TO_EDGE ? MTLSamplerAddressModeClampToEdge : w == GL_MIRRORED_REPEAT ? MTLSamplerAddressModeMirrorRepeat : w == GL_CLAMP_TO_BORDER ? MTLSamplerAddressModeClampToBorderColor : MTLSamplerAddressModeRepeat; }
static MTLCompareFunction cmpf(GLenum f)
{
    switch (f) { case GL_NEVER: return MTLCompareFunctionNever; case GL_LESS: return MTLCompareFunctionLess; case GL_EQUAL: return MTLCompareFunctionEqual;
    case GL_LEQUAL: return MTLCompareFunctionLessEqual; case GL_GREATER: return MTLCompareFunctionGreater; case GL_NOTEQUAL: return MTLCompareFunctionNotEqual;
    case GL_GEQUAL: return MTLCompareFunctionGreaterEqual; } return MTLCompareFunctionAlways;
}
static id<MTLSamplerState> tex_sampler(MTex *t)
{
    if (t->sampler) return t->sampler;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.magFilter = t->magF == GL_NEAREST ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    BOOL mipmapped = t->minF != GL_NEAREST && t->minF != GL_LINEAR && t->levels > 1;
    d.minFilter = (t->minF == GL_NEAREST || t->minF == GL_NEAREST_MIPMAP_NEAREST || t->minF == GL_NEAREST_MIPMAP_LINEAR) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    d.mipFilter = !mipmapped ? MTLSamplerMipFilterNotMipmapped : (t->minF == GL_NEAREST_MIPMAP_NEAREST || t->minF == GL_LINEAR_MIPMAP_NEAREST) ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    d.sAddressMode = wrap(t->wrapS); d.tAddressMode = wrap(t->wrapT); d.rAddressMode = wrap(t->wrapR);
    d.maxAnisotropy = t->aniso > 1 && mipmapped ? (NSUInteger)(t->aniso > 16 ? 16 : t->aniso) : 1;
    d.lodMinClamp = t->baseLevel; d.lodMaxClamp = t->maxLevel < 1000 ? t->maxLevel : FLT_MAX;
    if (t->compareMode == GL_COMPARE_REF_TO_TEXTURE) d.compareFunction = cmpf((GLenum)t->compareFunc);
    NSString *key = [NSString stringWithFormat:@"%d%d%d%d%d%d%d%lu%g%g%d", (int)d.magFilter, (int)d.minFilter, (int)d.mipFilter, (int)d.sAddressMode, (int)d.tAddressMode, (int)d.rAddressMode, t->compareMode, (unsigned long)d.maxAnisotropy, d.lodMinClamp, d.lodMaxClamp, t->compareFunc];
    id<MTLSamplerState> s = S.samplers[key];
    if (!s) { s = [S.dev newSamplerStateWithDescriptor:d]; S.samplers[key] = s; }
    return t->sampler = s;
}

/* ---------------------------------------------------------------- buffers and vertex arrays */
static MVao *cur_vao(void) { MVao *v = get(vaos, S.vao); return v ? v : S.defaultVao; }
static void mgl_glGenBuffers(GLsizei n, GLuint *out) { for (int k = 0; k < n; k++) out[k] = put(buffers, [MBuffer new]); }
static void mgl_glDeleteBuffers(GLsizei n, const GLuint *b) { for (int k = 0; k < n; k++) { if (S.arrayBuf == b[k]) S.arrayBuf = 0; drop(buffers, b[k]); } }
static void mgl_glBindBuffer(GLenum target, GLuint b)
{
    if (b && !get(buffers, b)) { while (buffers.count <= b) [buffers addObject:[NSNull null]]; buffers[b] = [MBuffer new]; }
    switch (target) {
    case GL_ARRAY_BUFFER: S.arrayBuf = b; break;
    case GL_ELEMENT_ARRAY_BUFFER: cur_vao()->element = b; break;
    case GL_PIXEL_UNPACK_BUFFER: S.unpackBuf = b; break;
    case GL_PIXEL_PACK_BUFFER: S.packBuf = b; break;
    }
}
static uint32_t target_buf(GLenum t) { return t == GL_ARRAY_BUFFER ? S.arrayBuf : t == GL_ELEMENT_ARRAY_BUFFER ? cur_vao()->element : t == GL_PIXEL_UNPACK_BUFFER ? S.unpackBuf : S.packBuf; }
static void mgl_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    (void)usage; MBuffer *b = get(buffers, target_buf(target)); if (!b) { err(GL_INVALID_OPERATION); return; }
    NSUInteger n = size > 0 ? (NSUInteger)size : 4;
    int pick = -1;                                         /* a version the GPU is done with */
    for (int k = 0; k < b->nver; k++) if (b->verFrame[k] <= S.done && b->ver[k].length >= n) { pick = k; break; }
    if (pick < 0 && b->nver < 6) { pick = b->nver++; b->ver[pick] = [S.dev newBufferWithLength:(n + 255) & ~(NSUInteger)255 options:MTLResourceStorageModeShared]; b->verFrame[pick] = 0; }
    if (pick < 0) {                                        /* all busy: wait for the oldest */
        uint64_t oldest = UINT64_MAX; for (int k = 0; k < b->nver; k++) if (b->verFrame[k] < oldest) { oldest = b->verFrame[k]; pick = k; }
        wait_frame(oldest);
        if (b->ver[pick].length < n) b->ver[pick] = [S.dev newBufferWithLength:(n + 255) & ~(NSUInteger)255 options:MTLResourceStorageModeShared];
    }
    if (data) memcpy(b->ver[pick].contents, data, (size_t)size);
    b->cur = pick; b->size = (NSUInteger)size;
}
static void mgl_glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data)
{
    MBuffer *b = get(buffers, target_buf(target)); if (!b || !b->nver) return;
    if (b->verFrame[b->cur] > S.done) {                   /* in use: copy into a fresh version first */
        id<MTLBuffer> old = b->ver[b->cur]; NSUInteger len = b->size;
        mgl_glBufferData(target, (GLsizeiptr)len, old.contents, GL_DYNAMIC_DRAW);
    }
    memcpy((uint8_t *)b->ver[b->cur].contents + off, data, (size_t)size);
}
static GLboolean mgl_glUnmapBuffer(GLenum target) { (void)target; return GL_TRUE; }
static void *mgl_glMapBufferRange(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access)
{
    MBuffer *b = get(buffers, target_buf(target)); if (!b || !b->nver) { err(GL_INVALID_OPERATION); return 0; }
    (void)len;
    if (b->verFrame[b->cur] > S.done && !(access & GL_MAP_UNSYNCHRONIZED_BIT)) {    /* the GPU may still read it: a fresh version */
        NSUInteger size = b->size;
        if (access & GL_MAP_INVALIDATE_BUFFER_BIT) mgl_glBufferData(target, (GLsizeiptr)size, 0, 0);
        else { void *copy = malloc(size); memcpy(copy, b->ver[b->cur].contents, size); mgl_glBufferData(target, (GLsizeiptr)size, copy, 0); free(copy); }
    }
    return (uint8_t *)b->ver[b->cur].contents + off;
}
static void *mgl_glMapBuffer(GLenum target, GLenum access) { MBuffer *b = get(buffers, target_buf(target)); return b ? mgl_glMapBufferRange(target, 0, (GLsizeiptr)b->size, access == GL_READ_ONLY ? GL_MAP_READ_BIT : GL_MAP_WRITE_BIT) : 0; }
static void mgl_glFlushMappedBufferRange(GLenum target, GLintptr off, GLsizeiptr len) { (void)target; (void)off; (void)len; }
static void mgl_glGetBufferParameteriv(GLenum target, GLenum p, GLint *v) { MBuffer *b = get(buffers, target_buf(target)); *v = p == GL_BUFFER_SIZE && b ? (GLint)b->size : 0; }
static void mgl_glGenVertexArrays(GLsizei n, GLuint *out) { for (int k = 0; k < n; k++) out[k] = put(vaos, [MVao new]); }
static void mgl_glDeleteVertexArrays(GLsizei n, const GLuint *v) { for (int k = 0; k < n; k++) { if (S.vao == v[k]) S.vao = 0; drop(vaos, v[k]); } }
static void mgl_glBindVertexArray(GLuint v) { S.vao = v; }
static void mgl_glEnableVertexAttribArray(GLuint i) { if (i < 16) cur_vao()->a[i].on = YES; }
static void mgl_glDisableVertexAttribArray(GLuint i) { if (i < 16) cur_vao()->a[i].on = NO; }
static void mgl_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr)
{
    if (i >= 16) return;
    Attr *a = &cur_vao()->a[i]; a->size = size; a->type = type; a->norm = norm; a->stride = stride; a->offset = (uint32_t)(uintptr_t)ptr; a->buffer = S.arrayBuf;
    if (!S.arrayBuf) LOG("client-side vertex arrays are not supported (attribute %u)", i);
}
static MTLVertexFormat vfmt(const Attr *a)
{
    int n = a->size == GL_BGRA ? 4 : a->size;
    switch (a->type) {
    case GL_FLOAT: return n == 1 ? MTLVertexFormatFloat : n == 2 ? MTLVertexFormatFloat2 : n == 3 ? MTLVertexFormatFloat3 : MTLVertexFormatFloat4;
    case GL_HALF_FLOAT: return n == 1 ? MTLVertexFormatHalf : n == 2 ? MTLVertexFormatHalf2 : n == 3 ? MTLVertexFormatHalf3 : MTLVertexFormatHalf4;
    case GL_UNSIGNED_BYTE:
        if (a->size == GL_BGRA) return MTLVertexFormatUChar4Normalized_BGRA;
        return a->norm ? (n == 1 ? MTLVertexFormatUCharNormalized : n == 2 ? MTLVertexFormatUChar2Normalized : n == 3 ? MTLVertexFormatUChar3Normalized : MTLVertexFormatUChar4Normalized)
                       : (n == 1 ? MTLVertexFormatUChar : n == 2 ? MTLVertexFormatUChar2 : n == 3 ? MTLVertexFormatUChar3 : MTLVertexFormatUChar4);
    case GL_BYTE:
        return a->norm ? (n == 1 ? MTLVertexFormatCharNormalized : n == 2 ? MTLVertexFormatChar2Normalized : n == 3 ? MTLVertexFormatChar3Normalized : MTLVertexFormatChar4Normalized)
                       : (n == 1 ? MTLVertexFormatChar : n == 2 ? MTLVertexFormatChar2 : n == 3 ? MTLVertexFormatChar3 : MTLVertexFormatChar4);
    case GL_UNSIGNED_SHORT:
        return a->norm ? (n == 1 ? MTLVertexFormatUShortNormalized : n == 2 ? MTLVertexFormatUShort2Normalized : n == 3 ? MTLVertexFormatUShort3Normalized : MTLVertexFormatUShort4Normalized)
                       : (n == 1 ? MTLVertexFormatUShort : n == 2 ? MTLVertexFormatUShort2 : n == 3 ? MTLVertexFormatUShort3 : MTLVertexFormatUShort4);
    case GL_SHORT:
        return a->norm ? (n == 1 ? MTLVertexFormatShortNormalized : n == 2 ? MTLVertexFormatShort2Normalized : n == 3 ? MTLVertexFormatShort3Normalized : MTLVertexFormatShort4Normalized)
                       : (n == 1 ? MTLVertexFormatShort : n == 2 ? MTLVertexFormatShort2 : n == 3 ? MTLVertexFormatShort3 : MTLVertexFormatShort4);
    case GL_INT: return n == 1 ? MTLVertexFormatInt : n == 2 ? MTLVertexFormatInt2 : n == 3 ? MTLVertexFormatInt3 : MTLVertexFormatInt4;
    case GL_UNSIGNED_INT: return n == 1 ? MTLVertexFormatUInt : n == 2 ? MTLVertexFormatUInt2 : n == 3 ? MTLVertexFormatUInt3 : MTLVertexFormatUInt4;
    case GL_UNSIGNED_INT_2_10_10_10_REV: return a->norm ? MTLVertexFormatUInt1010102Normalized : MTLVertexFormatUInt1010102Normalized;
    case GL_INT_2_10_10_10_REV: return MTLVertexFormatInt1010102Normalized;
    }
    return MTLVertexFormatFloat4;
}
static int type_size(GLenum t) { return t == GL_FLOAT || t == GL_INT || t == GL_UNSIGNED_INT ? 4 : t == GL_SHORT || t == GL_UNSIGNED_SHORT || t == GL_HALF_FLOAT ? 2 : t == GL_UNSIGNED_INT_2_10_10_10_REV || t == GL_INT_2_10_10_10_REV ? 4 : 1; }

/* ---------------------------------------------------------------- shaders and programs */
static uint32_t gl_names_put(id o) { return put(shaders, o); }
static GLuint mgl_glCreateShader(GLenum type) { MShader *s = [MShader new]; s->type = type; return gl_names_put(s); }
static void mgl_glShaderSource(GLuint sh, GLsizei n, const GLchar *const *str, const GLint *len)
{
    MShader *s = get(shaders, sh); if (![s isKindOfClass:[MShader class]]) return;
    size_t tot = 0; for (int k = 0; k < n; k++) tot += len && len[k] >= 0 ? (size_t)len[k] : strlen(str[k]);
    free(s->src); s->src = malloc(tot + 1); size_t o = 0;
    for (int k = 0; k < n; k++) { size_t l = len && len[k] >= 0 ? (size_t)len[k] : strlen(str[k]); memcpy(s->src + o, str[k], l); o += l; }
    s->src[o] = 0;
}
static NSMutableDictionary *fn_cache;                    /* MSL source hash -> MTLFunction */
static void mgl_glCompileShader(GLuint sh)
{
    MShader *s = get(shaders, sh); if (![s isKindOfClass:[MShader class]] || !s->src) return;
    if (!s->out) s->out = calloc(1, sizeof *s->out);
    int stage = s->type == GL_VERTEX_SHADER ? 0 : s->type == GL_FRAGMENT_SHADER ? 1 : 2;
    s->ok = mgl_translate(stage, s->src, s->out) != 0;
    if (!s->ok) { snprintf(s->log, sizeof s->log, "%s", s->out->log); LOG("shader %u did not translate: %.300s", sh, s->log); return; }
    NSString *msl = @(s->out->msl);
    id<MTLFunction> f = fn_cache[msl];
    if (!f) {
        NSError *e = nil; id<MTLLibrary> lib = [S.dev newLibraryWithSource:msl options:nil error:&e];
        if (!lib) { s->ok = NO; snprintf(s->log, sizeof s->log, "%s", e.localizedDescription.UTF8String); LOG("shader %u did not compile: %.300s", sh, s->log); return; }
        f = [lib newFunctionWithName:@"main0"]; fn_cache[msl] = f;
    }
    s->fn = f; s->log[0] = 0;
}
static void mgl_glGetShaderiv(GLuint sh, GLenum p, GLint *v)
{
    MShader *s = get(shaders, sh);
    if (p == GL_COMPILE_STATUS) *v = s && s->ok; else if (p == GL_INFO_LOG_LENGTH) *v = s ? (GLint)strlen(s->log) + 1 : 0;
    else if (p == GL_SHADER_TYPE) *v = s ? (GLint)s->type : 0; else *v = 0;
}
static void mgl_glGetShaderInfoLog(GLuint sh, GLsizei max, GLsizei *len, GLchar *out)
{ MShader *s = get(shaders, sh); const char *l = s ? s->log : ""; if (max > 0) { snprintf(out, (size_t)max, "%s", l); } if (len) *len = (GLsizei)strlen(l); }
static void mgl_glDeleteShader(GLuint sh) { (void)sh; }   /* kept while programs refer to them (the game deletes after linking) */
static GLuint mgl_glCreateProgram(void) { return gl_names_put([MProgram new]); }
static void mgl_glDeleteProgram(GLuint p) { if (S.prog == p) S.prog = 0; drop(shaders, p); }
static void mgl_glAttachShader(GLuint p, GLuint sh)
{
    MProgram *pr = get(shaders, p); MShader *s = get(shaders, sh); if (![pr isKindOfClass:[MProgram class]] || ![s isKindOfClass:[MShader class]]) return;
    if (s->type == GL_VERTEX_SHADER) pr->vs = s; else if (s->type == GL_FRAGMENT_SHADER) pr->fs = s;
}
static int uniform_size(const MglUniform *u) { int b = u->basetype == 13 || u->basetype == 7 || u->basetype == 8 || u->basetype == 1 ? 4 : 4; return b * (int)u->vecsize * (int)(u->columns ? u->columns : 1); }
static void mgl_glLinkProgram(GLuint p)
{
    MProgram *pr = get(shaders, p); if (![pr isKindOfClass:[MProgram class]]) return;
    pr->linked = pr->vs && pr->fs && pr->vs->ok && pr->fs->ok;
    if (!pr->linked) { snprintf(pr->log, sizeof pr->log, "a stage did not compile"); return; }
    /* the fragment stage again, its inputs at the vertex outputs' locations (by name) */
    MglShaderOut *lo = calloc(1, sizeof *lo);
    if (mgl_translate_linked(1, pr->fs->src, lo, pr->vs->out->outputs, pr->vs->out->noutputs)) {
        NSString *msl = @(lo->msl); id<MTLFunction> f = fn_cache[msl];
        if (!f) { NSError *e = nil; id<MTLLibrary> lib = [S.dev newLibraryWithSource:msl options:nil error:&e]; if (lib) { f = [lib newFunctionWithName:@"main0"]; fn_cache[msl] = f; } else LOG("linked fragment shader: %.300s", e.localizedDescription.UTF8String); }
        if (f) { pr->ffn = f; pr->fout = lo; lo = 0; }
    }
    if (lo) { free(lo->msl); free(lo); }
    free(pr->uni); pr->uni = calloc(MGL_MAX_UNIFORMS * 2, sizeof *pr->uni); pr->nuni = 0; pr->nloc = 0; pr->nsmp = 0;
    MShader *st[2] = {pr->vs, pr->fs};
    for (int s = 0; s < 2; s++) {
        MglShaderOut *o = s == 1 && pr->fout ? pr->fout : st[s]->out;
        free(pr->block[s]); pr->blockSize[s] = o->block_size; pr->block[s] = calloc(1, o->block_size + 16); pr->blockSlot[s] = (int)o->uniform_buffer;
        for (unsigned k = 0; k < o->nuniforms; k++) {
            const MglUniform *u = &o->uniforms[k]; Uni *x = 0;
            for (int j = 0; j < pr->nuni; j++) if (!strcmp(pr->uni[j].name, u->name)) x = &pr->uni[j];
            if (!x) {
                x = &pr->uni[pr->nuni++]; memset(x, 0, sizeof *x); snprintf(x->name, sizeof x->name, "%s", u->name);
                x->count = (int)u->array; x->basetype = u->basetype; x->columns = (int)u->columns; x->vecsize = (int)u->vecsize;
                x->loc = pr->nloc; pr->nloc += x->count; x->off[0] = x->off[1] = -1; x->sampler = -1;
            }
            x->off[s] = (int)u->offset; x->stride[s] = u->stride ? (int)u->stride : uniform_size(u);
        }
        for (unsigned k = 0; k < o->nsamplers; k++) {
            const MglSampler *sm = &o->samplers[k]; Uni *x = 0;
            for (int j = 0; j < pr->nuni; j++) if (!strcmp(pr->uni[j].name, sm->name)) x = &pr->uni[j];
            if (!x) {
                x = &pr->uni[pr->nuni++]; memset(x, 0, sizeof *x); snprintf(x->name, sizeof x->name, "%s", sm->name);
                x->count = 1; x->loc = pr->nloc++; x->off[0] = x->off[1] = -1; x->sampler = pr->nsmp;
                Smp *m = &pr->smp[pr->nsmp++]; m->loc = x->loc; m->unit = 0; m->slot[0] = m->slot[1] = -1; m->dim = sm->dim; m->depth = sm->depth;
            }
            pr->smp[x->sampler].slot[s] = (int)sm->texture;
        }
    }
    pr->ninputs = (int)pr->vs->out->ninputs; memcpy(pr->inputs, pr->vs->out->inputs, sizeof pr->inputs);
    pr->log[0] = 0;
}
static void mgl_glGetProgramiv(GLuint p, GLenum q, GLint *v)
{
    MProgram *pr = get(shaders, p);
    if (q == GL_LINK_STATUS || q == GL_VALIDATE_STATUS) *v = pr && pr->linked; else if (q == GL_INFO_LOG_LENGTH) *v = pr ? (GLint)strlen(pr->log) + 1 : 0;
    else if (q == GL_ACTIVE_UNIFORMS) *v = pr ? pr->nuni : 0; else *v = 0;
}
static void mgl_glGetProgramInfoLog(GLuint p, GLsizei max, GLsizei *len, GLchar *out)
{ MProgram *pr = get(shaders, p); const char *l = pr ? pr->log : ""; if (max > 0) snprintf(out, (size_t)max, "%s", l); if (len) *len = (GLsizei)strlen(l); }
static void mgl_glUseProgram(GLuint p) { S.prog = p; }
static GLint mgl_glGetUniformLocation(GLuint p, const GLchar *name)
{
    MProgram *pr = get(shaders, p); if (!pr || !pr->linked) return -1;
    char base[64]; snprintf(base, sizeof base, "%s", name); int idx = 0;
    char *br = strchr(base, '['); if (br) { idx = atoi(br + 1); *br = 0; }
    for (int k = 0; k < pr->nuni; k++) if (!strcmp(pr->uni[k].name, base)) return idx < pr->uni[k].count ? pr->uni[k].loc + idx : -1;
    return -1;
}
static GLint mgl_glGetAttribLocation(GLuint p, const GLchar *name)
{ MProgram *pr = get(shaders, p); if (!pr) return -1; for (int k = 0; k < pr->ninputs; k++) if (!strcmp(pr->inputs[k].name, name)) return (GLint)pr->inputs[k].location; return -1; }
static void mgl_glBindAttribLocation(GLuint p, GLuint i, const GLchar *n) { (void)p; (void)i; (void)n; }
static Uni *uni_at(MProgram *pr, GLint loc, int *elem)
{
    for (int k = 0; k < pr->nuni; k++) if (loc >= pr->uni[k].loc && loc < pr->uni[k].loc + pr->uni[k].count) { *elem = loc - pr->uni[k].loc; return &pr->uni[k]; }
    return 0;
}
static void set_uniform(GLint loc, GLsizei count, const void *data, int bytes_per)   /* bytes_per: one element's data */
{
    MProgram *pr = get(shaders, S.prog); if (!pr || loc < 0) return;
    int e; Uni *u = uni_at(pr, loc, &e); if (!u) return;
    if (u->sampler >= 0) { pr->smp[u->sampler].unit = *(const GLint *)data; return; }
    for (int s = 0; s < 2; s++) {
        if (u->off[s] < 0) continue;
        for (int k = 0; k < count && e + k < u->count; k++) {
            unsigned at = (unsigned)u->off[s] + (unsigned)(u->stride[s] * (e + k));
            int n = bytes_per;
            if (u->columns > 1 && bytes_per == u->columns * u->vecsize * 4) {        /* matrices: columns are 16 bytes apart */
                for (int c = 0; c < u->columns; c++) if (at + (unsigned)c * 16 + (unsigned)u->vecsize * 4 <= pr->blockSize[s])
                    memcpy(pr->block[s] + at + c * 16, (const uint8_t *)data + (size_t)k * n + (size_t)c * u->vecsize * 4, (size_t)u->vecsize * 4);
            } else if (at + (unsigned)n <= pr->blockSize[s] + 4) memcpy(pr->block[s] + at, (const uint8_t *)data + (size_t)k * n, (size_t)n);
        }
    }
}
static void mgl_glUniform1i(GLint l, GLint a) { set_uniform(l, 1, &a, 4); }
static void mgl_glUniform1f(GLint l, GLfloat a) { set_uniform(l, 1, &a, 4); }
static void mgl_glUniform2i(GLint l, GLint a, GLint b) { GLint v[2] = {a, b}; set_uniform(l, 1, v, 8); }
static void mgl_glUniform2f(GLint l, GLfloat a, GLfloat b) { GLfloat v[2] = {a, b}; set_uniform(l, 1, v, 8); }
static void mgl_glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { GLfloat v[3] = {a, b, c}; set_uniform(l, 1, v, 12); }
static void mgl_glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { GLfloat v[4] = {a, b, c, d}; set_uniform(l, 1, v, 16); }
static void mgl_glUniform1iv(GLint l, GLsizei n, const GLint *v) { set_uniform(l, n, v, 4); }
static void mgl_glUniform1fv(GLint l, GLsizei n, const GLfloat *v) { set_uniform(l, n, v, 4); }
static void mgl_glUniform2fv(GLint l, GLsizei n, const GLfloat *v) { set_uniform(l, n, v, 8); }
static void mgl_glUniform3fv(GLint l, GLsizei n, const GLfloat *v) { set_uniform(l, n, v, 12); }
static void mgl_glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { set_uniform(l, n, v, 16); }
static void mgl_glUniformMatrix3fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v)
{
    if (t) { GLfloat *m = malloc((size_t)n * 36); for (int k = 0; k < n; k++) for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m[k * 9 + i * 3 + j] = v[k * 9 + j * 3 + i]; set_uniform(l, n, m, 36); free(m); }
    else set_uniform(l, n, v, 36);
}
static void mgl_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v)
{
    if (t) { GLfloat *m = malloc((size_t)n * 64); for (int k = 0; k < n; k++) for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) m[k * 16 + i * 4 + j] = v[k * 16 + j * 4 + i]; set_uniform(l, n, m, 64); free(m); }
    else set_uniform(l, n, v, 64);
}

/* ---------------------------------------------------------------- framebuffers */
static void mgl_glGenFramebuffers(GLsizei n, GLuint *out) { for (int k = 0; k < n; k++) { MFbo *f = [MFbo new]; f->drawBuf = GL_COLOR_ATTACHMENT0; f->readBuf = GL_COLOR_ATTACHMENT0; out[k] = put(fbos, f); } }
static void mgl_glDeleteFramebuffers(GLsizei n, const GLuint *f) { for (int k = 0; k < n; k++) { if (S.drawFbo == f[k]) S.drawFbo = 0; if (S.readFbo == f[k]) S.readFbo = 0; drop(fbos, f[k]); } }
static void mgl_glBindFramebuffer(GLenum target, GLuint f)
{
    if (f && !get(fbos, f)) { while (fbos.count <= f) [fbos addObject:[NSNull null]]; MFbo *x = [MFbo new]; x->drawBuf = x->readBuf = GL_COLOR_ATTACHMENT0; fbos[f] = x; }
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) S.drawFbo = f;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) S.readFbo = f;
}
static void mgl_glFramebufferTexture2D(GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level)
{
    MFbo *f = get(fbos, target == GL_READ_FRAMEBUFFER ? S.readFbo : S.drawFbo); if (!f) return;
    if (att >= GL_COLOR_ATTACHMENT0 && att < GL_COLOR_ATTACHMENT0 + 8) { int k = (int)(att - GL_COLOR_ATTACHMENT0); f->color[k] = tex; f->colorLevel[k] = level; f->colorFace[k] = textarget; }
    else if (att == GL_DEPTH_ATTACHMENT) { f->depth = tex; f->depthLevel = level; }
    else if (att == GL_STENCIL_ATTACHMENT) f->stencil = tex;
    else if (att == GL_DEPTH_STENCIL_ATTACHMENT) { f->depth = f->stencil = tex; f->depthLevel = level; }
    if (S.passOpen && S.passFbo == (target == GL_READ_FRAMEBUFFER ? S.readFbo : S.drawFbo)) end_pass();
}
static GLenum mgl_glCheckFramebufferStatus(GLenum t) { (void)t; return GL_FRAMEBUFFER_COMPLETE; }
static void mgl_glBindRenderbuffer(GLenum t, GLuint r) { (void)t; if (r) LOG("renderbuffers are not supported"); }
static void mgl_glDrawBuffer(GLenum b) { MFbo *f = get(fbos, S.drawFbo); if (f) { f->drawBuf = b; if (S.passOpen) end_pass(); } }
static void mgl_glDrawBuffers(GLsizei n, const GLenum *b) { MFbo *f = get(fbos, S.drawFbo); if (f) { f->drawBuf = n > 0 ? b[0] : GL_NONE; if (S.passOpen) end_pass(); } }
static void mgl_glReadBuffer(GLenum b) { MFbo *f = get(fbos, S.readFbo); if (f) f->readBuf = b; }

static void ensure_default(void)
{
    CGSize sz = S.layer.drawableSize; int w = (int)sz.width, h = (int)sz.height; if (w < 1) w = 1; if (h < 1) h = 1;
    if (S.defColor && S.defW == w && S.defH == h) return;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    S.defColor = [S.dev newTextureWithDescriptor:d];
    d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;
    S.defDepth = [S.dev newTextureWithDescriptor:d];
    S.defW = w; S.defH = h;
}
/* the attachments of the current draw framebuffer */
typedef struct Targets { id<MTLTexture> color[8]; int level[8], slice[8]; int ncolor; id<MTLTexture> depth; BOOL stencil; int w, h; } Targets;
static void draw_targets(Targets *t)
{
    memset(t, 0, sizeof *t);
    MFbo *f = get(fbos, S.drawFbo);
    if (!f) { ensure_default(); t->color[0] = S.defColor; t->ncolor = 1; t->depth = S.defDepth; t->stencil = YES; t->w = S.defW; t->h = S.defH; return; }
    if (f->drawBuf != GL_NONE) {
        int k = f->drawBuf >= GL_COLOR_ATTACHMENT0 && f->drawBuf < GL_COLOR_ATTACHMENT0 + 8 ? (int)(f->drawBuf - GL_COLOR_ATTACHMENT0) : 0;
        MTex *x = get(textures, f->color[k]);
        if (x && x->tex) { t->color[0] = x->tex; t->level[0] = f->colorLevel[k]; t->slice[0] = f->colorFace[k] >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && f->colorFace[k] <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (int)(f->colorFace[k] - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
                           t->ncolor = 1; t->w = x->w >> f->colorLevel[k]; t->h = x->h >> f->colorLevel[k]; x->usedFrame = S.frame; }
    }
    MTex *dx = get(textures, f->depth);
    if (dx && dx->tex) { t->depth = dx->tex; t->stencil = dx->fmt == MTLPixelFormatDepth32Float_Stencil8 && f->stencil; if (!t->w) { t->w = dx->w; t->h = dx->h; } dx->usedFrame = S.frame; }
    if (t->w < 1) t->w = 1; if (t->h < 1) t->h = 1;
}
static void begin_pass(void)
{
    if (S.passOpen && S.passFbo == S.drawFbo && !S.pendingClear) return;
    end_pass();
    Targets t; draw_targets(&t);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    for (int k = 0; k < t.ncolor; k++) {
        p.colorAttachments[k].texture = t.color[k]; p.colorAttachments[k].level = (NSUInteger)t.level[k]; p.colorAttachments[k].slice = (NSUInteger)t.slice[k];
        p.colorAttachments[k].loadAction = S.pendingClear & GL_COLOR_BUFFER_BIT ? MTLLoadActionClear : MTLLoadActionLoad;
        p.colorAttachments[k].clearColor = MTLClearColorMake(S.clearC[0], S.clearC[1], S.clearC[2], S.clearC[3]);
        p.colorAttachments[k].storeAction = MTLStoreActionStore;
    }
    if (t.depth) {
        p.depthAttachment.texture = t.depth; p.depthAttachment.loadAction = S.pendingClear & GL_DEPTH_BUFFER_BIT ? MTLLoadActionClear : MTLLoadActionLoad;
        p.depthAttachment.clearDepth = S.clearD; p.depthAttachment.storeAction = MTLStoreActionStore;
        if (t.depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
            p.stencilAttachment.texture = t.depth; p.stencilAttachment.loadAction = S.pendingClear & GL_STENCIL_BUFFER_BIT ? MTLLoadActionClear : MTLLoadActionLoad;
            p.stencilAttachment.clearStencil = (uint32_t)S.clearS; p.stencilAttachment.storeAction = MTLStoreActionStore;
        }
    }
    if (!t.ncolor && !t.depth) { p.renderTargetWidth = (NSUInteger)t.w; p.renderTargetHeight = (NSUInteger)t.h; p.defaultRasterSampleCount = 1; }
    S.enc = [S.cb renderCommandEncoderWithDescriptor:p];
    S.passOpen = YES; S.passFbo = S.drawFbo; S.pendingClear = 0;
    S.passKey[0] = (uint32_t)t.ncolor; S.passKey[1] = t.ncolor ? (uint32_t)t.color[0].pixelFormat : 0; S.passKey[2] = t.depth ? (uint32_t)t.depth.pixelFormat : 0;
    S.passKey[3] = (uint32_t)t.w; S.passKey[4] = (uint32_t)t.h;
}
static void mgl_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.clearC[0] = r; S.clearC[1] = g; S.clearC[2] = b; S.clearC[3] = a; }
static void mgl_glClearDepth(GLdouble d) { S.clearD = d; }
static void mgl_glClearStencil(GLint s) { S.clearS = s; }
static void mgl_glClear(GLbitfield m)
{
    /* a clear starts the pass (load action); scissored or masked clears are not partial here */
    if (S.passOpen && S.passFbo == S.drawFbo) end_pass();
    S.pendingClear |= (int)(m & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
    begin_pass();
}

/* ---------------------------------------------------------------- state */
static void cap(GLenum c, BOOL on)
{
    switch (c) {
    case GL_BLEND: S.blend = on; break; case GL_DEPTH_TEST: S.depthTest = on; break; case GL_CULL_FACE: S.cull = on; break;
    case GL_SCISSOR_TEST: S.scissorTest = on; break; case GL_POLYGON_OFFSET_FILL: S.polyOffset = on; break;
    case GL_FRAMEBUFFER_SRGB: S.srgb = on; break;
    case GL_TEXTURE_CUBE_MAP_SEAMLESS: case GL_PROGRAM_POINT_SIZE: case GL_MULTISAMPLE: case GL_DITHER: case GL_LINE_SMOOTH: case GL_POLYGON_SMOOTH: break;
    default: { static int seen[64]; int k = (int)(c & 63); if (!seen[k]) { seen[k] = 1; LOG("glEnable/Disable(%04x) ignored", c); } }
    }
}
static void mgl_glEnable(GLenum c) { cap(c, YES); }
static void mgl_glDisable(GLenum c) { cap(c, NO); }
static GLboolean mgl_glIsEnabled(GLenum c) { return c == GL_BLEND ? S.blend : c == GL_DEPTH_TEST ? S.depthTest : c == GL_CULL_FACE ? S.cull : c == GL_SCISSOR_TEST ? S.scissorTest : 0; }
static void mgl_glBlendFunc(GLenum s, GLenum d) { S.bsrc = S.bsrcA = s; S.bdst = S.bdstA = d; }
static void mgl_glBlendFuncSeparate(GLenum s, GLenum d, GLenum sa, GLenum da) { S.bsrc = s; S.bdst = d; S.bsrcA = sa; S.bdstA = da; }
static void mgl_glBlendEquation(GLenum e) { S.beq = S.beqA = e; }
static void mgl_glBlendEquationSeparate(GLenum e, GLenum ea) { S.beq = e; S.beqA = ea; }
static void mgl_glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.bcolor[0] = r; S.bcolor[1] = g; S.bcolor[2] = b; S.bcolor[3] = a; }
static void mgl_glDepthFunc(GLenum f) { S.depthFunc = f; }
static void mgl_glDepthMask(GLboolean m) { S.depthMask = m; }
static void mgl_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { S.cmask[0] = r; S.cmask[1] = g; S.cmask[2] = b; S.cmask[3] = a; }
static void mgl_glCullFace(GLenum f) { S.cullFace = f; }
static void mgl_glFrontFace(GLenum f) { S.frontFace = f; }
static void mgl_glPolygonMode(GLenum face, GLenum m) { (void)face; S.polyMode = m; }
static void mgl_glPolygonOffset(GLfloat f, GLfloat u) { S.offFactor = f; S.offUnits = u; }
static void mgl_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h; }
static void mgl_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { S.sc[0] = x; S.sc[1] = y; S.sc[2] = w; S.sc[3] = h; }
static void mgl_glPointSize(GLfloat s) { (void)s; }
static void mgl_glLineWidth(GLfloat s) { (void)s; }
static void mgl_glFogf(GLenum p, GLfloat v) { (void)p; (void)v; }
static void mgl_glFlush(void) { }
static void mgl_glFinish(void) { }
static GLenum mgl_glGetError(void) { GLenum e = S.error; S.error = 0; return e; }

static MTLBlendFactor bf(GLenum f)
{
    switch (f) {
    case GL_ZERO: return MTLBlendFactorZero; case GL_ONE: return MTLBlendFactorOne;
    case GL_SRC_COLOR: return MTLBlendFactorSourceColor; case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case GL_DST_COLOR: return MTLBlendFactorDestinationColor; case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha; case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case GL_DST_ALPHA: return MTLBlendFactorDestinationAlpha; case GL_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case GL_CONSTANT_COLOR: return MTLBlendFactorBlendColor; case GL_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
    case GL_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha; case GL_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
    case GL_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
    }
    return MTLBlendFactorOne;
}
static MTLBlendOperation bop(GLenum e)
{ return e == GL_FUNC_SUBTRACT ? MTLBlendOperationSubtract : e == GL_FUNC_REVERSE_SUBTRACT ? MTLBlendOperationReverseSubtract : e == GL_MIN ? MTLBlendOperationMin : e == GL_MAX ? MTLBlendOperationMax : MTLBlendOperationAdd; }

/* ---------------------------------------------------------------- drawing */
static id<MTLRenderPipelineState> pipeline(MProgram *pr, MVao *va)
{
    /* key: program, attachment formats, blend, write mask, vertex layout */
    uint32_t key[64]; int n = 0;
    key[n++] = (uint32_t)(uintptr_t)(__bridge void *)pr; key[n++] = (uint32_t)((uintptr_t)(__bridge void *)pr >> 32);
    key[n++] = S.passKey[0]; key[n++] = S.passKey[1]; key[n++] = S.passKey[2];
    key[n++] = S.blend ? (S.bsrc | S.bdst << 16) : 0; key[n++] = S.blend ? (S.bsrcA | S.bdstA << 16) : 0; key[n++] = S.blend ? (S.beq | S.beqA << 16) : 0;
    key[n++] = (uint32_t)(S.cmask[0] | S.cmask[1] << 1 | S.cmask[2] << 2 | S.cmask[3] << 3);
    for (int k = 0; k < pr->ninputs && n < 62; k++) {
        unsigned L = pr->inputs[k].location; const Attr *a = L < 16 ? &va->a[L] : 0;
        key[n++] = a && a->on ? (uint32_t)vfmt(a) | (uint32_t)(a->stride ? a->stride : a->size * type_size(a->type)) << 8 : 0xffffffffu;
    }
    NSData *kd = [NSData dataWithBytes:key length:(NSUInteger)n * 4];
    id<MTLRenderPipelineState> ps = S.pipelines[kd];
    if (ps == (id)[NSNull null]) return nil;
    if (ps) return ps;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = pr->vs->fn; d.fragmentFunction = pr->ffn ? pr->ffn : pr->fs->fn;
    if (S.passKey[0]) {
        d.colorAttachments[0].pixelFormat = (MTLPixelFormat)S.passKey[1];
        d.colorAttachments[0].blendingEnabled = S.blend;
        d.colorAttachments[0].sourceRGBBlendFactor = bf(S.bsrc); d.colorAttachments[0].destinationRGBBlendFactor = bf(S.bdst);
        d.colorAttachments[0].sourceAlphaBlendFactor = bf(S.bsrcA); d.colorAttachments[0].destinationAlphaBlendFactor = bf(S.bdstA);
        d.colorAttachments[0].rgbBlendOperation = bop(S.beq); d.colorAttachments[0].alphaBlendOperation = bop(S.beqA);
        d.colorAttachments[0].writeMask = (S.cmask[0] ? MTLColorWriteMaskRed : 0) | (S.cmask[1] ? MTLColorWriteMaskGreen : 0) | (S.cmask[2] ? MTLColorWriteMaskBlue : 0) | (S.cmask[3] ? MTLColorWriteMaskAlpha : 0);
    }
    if (S.passKey[2]) { d.depthAttachmentPixelFormat = (MTLPixelFormat)S.passKey[2]; if (S.passKey[2] == MTLPixelFormatDepth32Float_Stencil8) d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8; }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (int k = 0; k < pr->ninputs; k++) {
        unsigned L = pr->inputs[k].location; const Attr *a = L < 16 ? &va->a[L] : 0; NSUInteger slot = VB_BASE + L;
        if (a && a->on) {
            vd.attributes[L].format = vfmt(a); vd.attributes[L].offset = 0; vd.attributes[L].bufferIndex = slot;
            vd.layouts[slot].stride = (NSUInteger)(a->stride ? a->stride : a->size * type_size(a->type));
            vd.layouts[slot].stepFunction = MTLVertexStepFunctionPerVertex;
        } else {                                       /* disabled: the generic value (0, 0, 0, 1) */
            int vs = (int)pr->inputs[k].vecsize; BOOL in = pr->inputs[k].basetype == 7 || pr->inputs[k].basetype == 8;
            vd.attributes[L].format = in ? (vs == 1 ? MTLVertexFormatInt : vs == 2 ? MTLVertexFormatInt2 : vs == 3 ? MTLVertexFormatInt3 : MTLVertexFormatInt4)
                                         : (vs == 1 ? MTLVertexFormatFloat : vs == 2 ? MTLVertexFormatFloat2 : vs == 3 ? MTLVertexFormatFloat3 : MTLVertexFormatFloat4);
            vd.attributes[L].offset = 0; vd.attributes[L].bufferIndex = slot;
            vd.layouts[slot].stride = 16; vd.layouts[slot].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[slot].stepRate = 0;
        }
    }
    d.vertexDescriptor = vd;
    NSError *e = nil; ps = [S.dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) {
        static int dumped; const char *dd = getenv("HW_MGL_DUMP");
        if (dd && dumped < 3) { char path[512]; snprintf(path, sizeof path, "%s/fail%d_vs.metal", dd, dumped); FILE *f = fopen(path, "w"); if (f) { fputs(pr->vs->out->msl, f); fclose(f); }
                                snprintf(path, sizeof path, "%s/fail%d_fs.metal", dd, dumped); f = fopen(path, "w"); if (f) { fputs(pr->fs->out->msl, f); fclose(f); } dumped++; }
        static int logged; if (logged++ < 20) LOG("pipeline: %s", e.localizedDescription.UTF8String);
        S.pipelines[kd] = (id)[NSNull null]; return nil;
    }
    S.pipelines[kd] = ps; return ps;
}
static id<MTLDepthStencilState> ds_state(void)
{
    uint32_t k = (S.depthTest ? 1u : 0u) | (S.depthMask ? 2u : 0u) | (uint32_t)S.depthFunc << 4;
    NSNumber *kn = @(k); id<MTLDepthStencilState> s = S.dsStates[kn];
    if (!s) {
        MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = S.depthTest ? cmpf(S.depthFunc) : MTLCompareFunctionAlways;
        d.depthWriteEnabled = S.depthTest && S.depthMask;
        s = [S.dev newDepthStencilStateWithDescriptor:d]; S.dsStates[kn] = s;
    }
    return s;
}
static BOOL prepare_draw(void)
{
    MProgram *pr = get(shaders, S.prog);
    if (![pr isKindOfClass:[MProgram class]] || !pr->linked) return NO;
    begin_pass();
    MVao *va = cur_vao();
    id<MTLRenderPipelineState> ps = pipeline(pr, va); if (!ps) return NO;
    id<MTLRenderCommandEncoder> e = S.enc;
    [e setRenderPipelineState:ps];
    [e setDepthStencilState:ds_state()];
    [e setCullMode:S.cull ? (S.cullFace == GL_FRONT ? MTLCullModeFront : S.cullFace == GL_BACK ? MTLCullModeBack : MTLCullModeNone) : MTLCullModeNone];
    [e setFrontFacingWinding:S.frontFace == GL_CW ? MTLWindingCounterClockwise : MTLWindingClockwise];   /* Y is flipped */
    [e setTriangleFillMode:S.polyMode == GL_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    if (S.polyOffset) [e setDepthBias:S.offUnits slopeScale:S.offFactor clamp:0]; else [e setDepthBias:0 slopeScale:0 clamp:0];
    int tw = (int)S.passKey[3], th = (int)S.passKey[4];
    [e setViewport:(MTLViewport){S.vp[0], S.vp[1], S.vp[2], S.vp[3], 0, 1}];
    MTLScissorRect sr = {0, 0, (NSUInteger)tw, (NSUInteger)th};
    if (S.scissorTest) {
        int x0 = S.sc[0] < 0 ? 0 : S.sc[0], y0 = S.sc[1] < 0 ? 0 : S.sc[1], x1 = S.sc[0] + S.sc[2], y1 = S.sc[1] + S.sc[3];
        if (x1 > tw) x1 = tw; if (y1 > th) y1 = th; if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
        sr = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
        if (!sr.width || !sr.height) return NO;
    }
    [e setScissorRect:sr];
    [e setBlendColorRed:S.bcolor[0] green:S.bcolor[1] blue:S.bcolor[2] alpha:S.bcolor[3]];
    for (int k = 0; k < pr->ninputs; k++) {
        unsigned L = pr->inputs[k].location; const Attr *a = L < 16 ? &va->a[L] : 0;
        if (a && a->on) {
            MBuffer *b = get(buffers, a->buffer);
            if (!b || !b->nver) return NO;
            [e setVertexBuffer:b->ver[b->cur] offset:a->offset atIndex:VB_BASE + L]; b->verFrame[b->cur] = S.frame;
        } else [e setVertexBuffer:S.zeroAttr offset:0 atIndex:VB_BASE + L];
    }
    for (int s = 0; s < 2; s++) {
        if (!pr->blockSize[s]) continue;
        if (pr->blockSize[s] <= 4096) {
            if (s == 0) [e setVertexBytes:pr->block[0] length:pr->blockSize[0] atIndex:(NSUInteger)pr->blockSlot[0]];
            else [e setFragmentBytes:pr->block[1] length:pr->blockSize[1] atIndex:(NSUInteger)pr->blockSlot[1]];
        } else {
            NSUInteger off; id<MTLBuffer> r = ring_alloc(pr->blockSize[s], &off); memcpy((uint8_t *)r.contents + off, pr->block[s], pr->blockSize[s]);
            if (s == 0) [e setVertexBuffer:r offset:off atIndex:(NSUInteger)pr->blockSlot[0]]; else [e setFragmentBuffer:r offset:off atIndex:(NSUInteger)pr->blockSlot[1]];
        }
    }
    for (int k = 0; k < pr->nsmp; k++) {
        Smp *m = &pr->smp[k]; Unit *u = &S.units[m->unit & 31];
        MTex *t = get(textures, m->dim == 3 ? u->cube : u->tex2d);
        if (!t || !t->tex) continue;
        id<MTLSamplerState> ss = tex_sampler(t); t->usedFrame = S.frame;
        id<MTLTexture> view = sample_view(t);
        if (m->slot[0] >= 0) { [e setVertexTexture:view atIndex:(NSUInteger)m->slot[0]]; [e setVertexSamplerState:ss atIndex:(NSUInteger)m->slot[0]]; }
        if (m->slot[1] >= 0) { [e setFragmentTexture:view atIndex:(NSUInteger)m->slot[1]]; [e setFragmentSamplerState:ss atIndex:(NSUInteger)m->slot[1]]; }
    }
    return YES;
}
static MTLPrimitiveType prim(GLenum m)
{ return m == GL_POINTS ? MTLPrimitiveTypePoint : m == GL_LINES ? MTLPrimitiveTypeLine : m == GL_LINE_STRIP ? MTLPrimitiveTypeLineStrip : m == GL_TRIANGLE_STRIP ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle; }
static void draw_indexed(GLenum mode, GLsizei count, GLenum type, uintptr_t offset, GLint base, GLsizei instances)
{
    if (count <= 0 || !prepare_draw()) return;
    MBuffer *ib = get(buffers, cur_vao()->element); if (!ib || !ib->nver) return;
    id<MTLBuffer> buf = ib->ver[ib->cur]; ib->verFrame[ib->cur] = S.frame;
    NSUInteger off = offset; MTLIndexType it = type == GL_UNSIGNED_INT ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
    if (type == GL_UNSIGNED_BYTE || mode == GL_TRIANGLE_FAN || mode == GL_LINE_LOOP) {   /* convert to 16/32-bit triangle or line lists */
        const uint8_t *src = (const uint8_t *)buf.contents + offset; int sz = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
        uint32_t *tmp = malloc((size_t)count * 3 * 4 + 16); int n = 0;
#define IDX(i) (sz == 4 ? ((const uint32_t *)src)[i] : sz == 2 ? ((const uint16_t *)src)[i] : src[i])
        if (mode == GL_TRIANGLE_FAN) { for (int i = 1; i + 1 < count; i++) { tmp[n++] = IDX(0); tmp[n++] = IDX(i); tmp[n++] = IDX(i + 1); } mode = GL_TRIANGLES; }
        else if (mode == GL_LINE_LOOP) { for (int i = 0; i < count; i++) { tmp[n++] = IDX(i); tmp[n++] = IDX((i + 1) % count); } mode = GL_LINES; }
        else for (int i = 0; i < count; i++) tmp[n++] = IDX(i);
#undef IDX
        id<MTLBuffer> r = ring_alloc((NSUInteger)n * 4, &off); memcpy((uint8_t *)r.contents + off, tmp, (size_t)n * 4); free(tmp);
        buf = r; it = MTLIndexTypeUInt32; count = n;
    }
    [S.enc drawIndexedPrimitives:prim(mode) indexCount:(NSUInteger)count indexType:it indexBuffer:buf indexBufferOffset:off
                   instanceCount:(NSUInteger)(instances > 0 ? instances : 1) baseVertex:base baseInstance:0];
}
static void mgl_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) { draw_indexed(mode, count, type, (uintptr_t)idx, 0, 1); }
static void mgl_glDrawRangeElements(GLenum mode, GLuint s, GLuint e, GLsizei count, GLenum type, const void *idx) { (void)s; (void)e; draw_indexed(mode, count, type, (uintptr_t)idx, 0, 1); }
static void mgl_glDrawRangeElementsBaseVertex(GLenum mode, GLuint s, GLuint e, GLsizei count, GLenum type, const void *idx, GLint base) { (void)s; (void)e; draw_indexed(mode, count, type, (uintptr_t)idx, base, 1); }
static void mgl_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *idx, GLint base) { draw_indexed(mode, count, type, (uintptr_t)idx, base, 1); }
static void mgl_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *idx, GLsizei n) { draw_indexed(mode, count, type, (uintptr_t)idx, 0, n); }
static void mgl_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (count <= 0 || !prepare_draw()) return;
    if (mode == GL_TRIANGLE_FAN || mode == GL_LINE_LOOP) {
        uint32_t *tmp = malloc((size_t)count * 3 * 4 + 16); int n = 0;
        if (mode == GL_TRIANGLE_FAN) for (int i = 1; i + 1 < count; i++) { tmp[n++] = (uint32_t)first; tmp[n++] = (uint32_t)(first + i); tmp[n++] = (uint32_t)(first + i + 1); }
        else for (int i = 0; i < count; i++) { tmp[n++] = (uint32_t)(first + i); tmp[n++] = (uint32_t)(first + (i + 1) % count); }
        NSUInteger off; id<MTLBuffer> r = ring_alloc((NSUInteger)n * 4, &off); memcpy((uint8_t *)r.contents + off, tmp, (size_t)n * 4); free(tmp);
        [S.enc drawIndexedPrimitives:mode == GL_TRIANGLE_FAN ? MTLPrimitiveTypeTriangle : MTLPrimitiveTypeLine indexCount:(NSUInteger)n indexType:MTLIndexTypeUInt32 indexBuffer:r indexBufferOffset:off];
        return;
    }
    [S.enc drawPrimitives:prim(mode) vertexStart:(NSUInteger)first vertexCount:(NSUInteger)count];
}

/* ---------------------------------------------------------------- blits and presenting */
static NSString *const kFlipSrc = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; float2 uv; };\n"
    "vertex V vs(uint i [[vertex_id]], constant float4 &r [[buffer(0)]]) { float2 q = float2(i & 1, i >> 1); V o; o.p = float4(q * 2 - 1, 0, 1); o.uv = float2(mix(r.x, r.z, q.x), mix(r.y, r.w, q.y)); return o; }\n"
    "fragment float4 fs(V v [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, v.uv); }\n";
static id<MTLRenderPipelineState> blit_pipe(MTLPixelFormat f)
{
    static NSMutableDictionary *cache; static id<MTLLibrary> lib;
    if (!cache) { cache = [NSMutableDictionary new]; NSError *e; lib = [S.dev newLibraryWithSource:kFlipSrc options:nil error:&e]; }
    id<MTLRenderPipelineState> p = cache[@(f)];
    if (!p) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vs"]; d.fragmentFunction = [lib newFunctionWithName:@"fs"]; d.colorAttachments[0].pixelFormat = f;
        p = [S.dev newRenderPipelineStateWithDescriptor:d error:nil]; cache[@(f)] = p;
    }
    return p;
}
/* draw src (texture coordinates rect) into dst (pixel rect) with a quad; y up in both (GL layout) */
static void quad_blit(id<MTLTexture> src, float sx0, float sy0, float sx1, float sy1, id<MTLTexture> dst, int dlevel, int dx0, int dy0, int dx1, int dy1, BOOL linear, BOOL flip)
{
    end_pass();
    { static int seen; if (!seen++) LOG("scaled blit %dx%d -> %dx%d", (int)(sx1 - sx0), (int)(sy1 - sy0), dx1 - dx0, dy1 - dy0); }
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = dst; p.colorAttachments[0].level = (NSUInteger)dlevel; p.colorAttachments[0].loadAction = MTLLoadActionLoad; p.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [S.cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:blit_pipe(dst.pixelFormat)];
    float tw = (float)src.width, th = (float)src.height;
    /* the quad's q.y = 0 is the top row of the viewport in Metal; GL layout keeps row 0 = bottom, so map q.y=0 -> sy0 unless presenting */
    float r[4] = {sx0 / tw, (flip ? sy1 : sy0) / th, sx1 / tw, (flip ? sy0 : sy1) / th};
    [e setVertexBytes:r length:16 atIndex:0];
    [e setViewport:(MTLViewport){dx0, dy0, dx1 - dx0, dy1 - dy0, 0, 1}];
    static id<MTLSamplerState> lin, near;
    if (!lin) { MTLSamplerDescriptor *d = [MTLSamplerDescriptor new]; d.minFilter = d.magFilter = MTLSamplerMinMagFilterLinear; lin = [S.dev newSamplerStateWithDescriptor:d];
                d.minFilter = d.magFilter = MTLSamplerMinMagFilterNearest; near = [S.dev newSamplerStateWithDescriptor:d]; }
    [e setFragmentTexture:src atIndex:0]; [e setFragmentSamplerState:linear ? lin : near atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
}
static id<MTLTexture> read_color(int *w, int *h)
{
    MFbo *f = get(fbos, S.readFbo);
    if (!f) { ensure_default(); *w = S.defW; *h = S.defH; return S.defColor; }
    int k = f->readBuf >= GL_COLOR_ATTACHMENT0 && f->readBuf < GL_COLOR_ATTACHMENT0 + 8 ? (int)(f->readBuf - GL_COLOR_ATTACHMENT0) : 0;
    MTex *t = get(textures, f->color[k]); if (!t) return nil;
    *w = t->w; *h = t->h; t->usedFrame = S.frame; return t->tex;
}
static void mgl_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;          /* depth blits: not used by the game */
    int sw, sh; id<MTLTexture> src = read_color(&sw, &sh); if (!src) return;
    Targets t; draw_targets(&t); if (!t.ncolor) return;
    if (src == t.color[0]) return;
    BOOL same = sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0 && sx0 >= 0 && sy0 >= 0 && src.pixelFormat == t.color[0].pixelFormat && dx0 >= 0 && dy0 >= 0;
    end_pass();
    if (same) {
        id<MTLBlitCommandEncoder> b = [S.cb blitCommandEncoder];
        int w = sx1 - sx0, h = sy1 - sy0;
        if (sx0 + w > (int)src.width) w = (int)src.width - sx0; if (sy0 + h > (int)src.height) h = (int)src.height - sy0;
        if (dx0 + w > (int)t.color[0].width) w = (int)t.color[0].width - dx0; if (dy0 + h > (int)t.color[0].height) h = (int)t.color[0].height - dy0;
        if (w > 0 && h > 0)
            [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((NSUInteger)sx0, (NSUInteger)sy0, 0) sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
                     toTexture:t.color[0] destinationSlice:(NSUInteger)t.slice[0] destinationLevel:(NSUInteger)t.level[0] destinationOrigin:MTLOriginMake((NSUInteger)dx0, (NSUInteger)dy0, 0)];
        [b endEncoding];
    } else quad_blit(src, (float)sx0, (float)sy0, (float)sx1, (float)sy1, t.color[0], t.level[0], dx0, dy0, dx1, dy1, filter == GL_LINEAR, NO);
}

/* ---------------------------------------------------------------- queries */
static GLint apple_int(GLenum p, GLint fallback);
static void mgl_glGetIntegerv(GLenum p, GLint *v)
{
    switch (p) {
    case GL_VIEWPORT: memcpy(v, S.vp, 16); return;
    case GL_SCISSOR_BOX: memcpy(v, S.sc, 16); return;
    case GL_FRAMEBUFFER_BINDING: *v = (GLint)S.drawFbo; return;
    case GL_READ_FRAMEBUFFER_BINDING: *v = (GLint)S.readFbo; return;
    case GL_CURRENT_PROGRAM: *v = (GLint)S.prog; return;
    case GL_VERTEX_ARRAY_BINDING: *v = (GLint)S.vao; return;
    case GL_ARRAY_BUFFER_BINDING: *v = (GLint)S.arrayBuf; return;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: *v = (GLint)cur_vao()->element; return;
    case GL_TEXTURE_BINDING_2D: *v = (GLint)S.units[S.activeUnit].tex2d; return;
    case GL_TEXTURE_BINDING_CUBE_MAP: *v = (GLint)S.units[S.activeUnit].cube; return;
    case GL_ACTIVE_TEXTURE: *v = GL_TEXTURE0 + S.activeUnit; return;
    case GL_NUM_EXTENSIONS: *v = (GLint)S.extensions.count; return;
    case GL_MAJOR_VERSION: *v = 4; return;
    case GL_MINOR_VERSION: *v = 1; return;
    case GL_DRAW_BUFFER: { MFbo *f = get(fbos, S.drawFbo); *v = f ? (GLint)f->drawBuf : GL_BACK; return; }
    case GL_UNPACK_ALIGNMENT: *v = S.unpackAlign; return;
    }
    *v = apple_int(p, 0);
}
static void mgl_glGetFloatv(GLenum p, GLfloat *v)
{
    if (p == GL_COLOR_CLEAR_VALUE) { memcpy(v, S.clearC, 16); return; }
    if (p == GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT) { *v = 16; return; }
    GLint i[4] = {0}; mgl_glGetIntegerv(p, i); *v = (GLfloat)i[0];
}
static void mgl_glGetBooleanv(GLenum p, GLboolean *v) { GLint i = 0; mgl_glGetIntegerv(p, &i); *v = i != 0; }
static const GLubyte *mgl_glGetString(GLenum n)
{
    switch (n) {
    case GL_VENDOR: return (const GLubyte *)"Apple";
    case GL_RENDERER: return (const GLubyte *)S.renderer;
    case GL_VERSION: return (const GLubyte *)S.version;
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"4.10";
    }
    return (const GLubyte *)"";
}
static const GLubyte *mgl_glGetStringi(GLenum n, GLuint i)
{ return n == GL_EXTENSIONS && i < S.extensions.count ? (const GLubyte *)S.extensions[i].UTF8String : 0; }
static void mgl_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *px)
{
    (void)type; int sw, sh; id<MTLTexture> src = read_color(&sw, &sh); if (!src || !px) return;
    end_pass();
    id<MTLBuffer> tmp = [S.dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> b = [S.cb blitCommandEncoder];
    [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)y, 0) sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1)
              toBuffer:tmp destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
    [b endEncoding];
    uint64_t f = S.frame; wait_frame(f);
    const uint8_t *s = tmp.contents; uint8_t *d = px; BOOL bgra = src.pixelFormat == MTLPixelFormatBGRA8Unorm || src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    for (int i = 0; i < w * h; i++) {
        uint8_t c0 = s[i * 4], c1 = s[i * 4 + 1], c2 = s[i * 4 + 2], c3 = s[i * 4 + 3];
        if (bgra != (format == GL_BGRA)) { uint8_t t = c0; c0 = c2; c2 = t; }
        d[i * 4] = c0; d[i * 4 + 1] = c1; d[i * 4 + 2] = c2; d[i * 4 + 3] = c3;
    }
}

/* ---------------------------------------------------------------- contexts */
static CGLContextObj apple_ctx;                          /* a hidden Apple GL context for implementation limits and extensions */
static GLint apple_int(GLenum p, GLint fallback)
{
    if (!apple_ctx) return fallback;
    CGLContextObj prev = CGLGetCurrentContext(); CGLSetCurrentContext(apple_ctx);
    GLint v[4] = {fallback}; glGetIntegerv(p, v); CGLSetCurrentContext(prev);
    return v[0];
}
static void apple_info(void)
{
    CGLPixelFormatAttribute a[] = {kCGLPFAAccelerated, kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core, 0};
    CGLPixelFormatObj pf; GLint n;
    S.extensions = [NSMutableArray new];
    if (CGLChoosePixelFormat(a, &pf, &n) || !pf || CGLCreateContext(pf, 0, &apple_ctx)) { apple_ctx = 0; return; }
    CGLDestroyPixelFormat(pf);
    CGLContextObj prev = CGLGetCurrentContext(); CGLSetCurrentContext(apple_ctx);
    GLint ne = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &ne);
    for (GLint k = 0; k < ne; k++) [S.extensions addObject:@((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)k))];
    GLint rid = 0; CGLGetParameter(apple_ctx, kCGLCPCurrentRendererID, &rid); S.rendererID = (uint32_t)rid;
    CGLSetCurrentContext(prev);
}
uint32_t mgl_renderer_id(void) { return S.rendererID; }
static void init_device(void)
{
    if (S.dev) return;
    S.dev = MTLCreateSystemDefaultDevice(); S.queue = [S.dev newCommandQueueWithMaxCommandBufferCount:64];
    S.inflight = dispatch_semaphore_create(FRAMES);
    buffers = [NSMutableArray new]; textures = [NSMutableArray new]; vaos = [NSMutableArray new]; fbos = [NSMutableArray new]; shaders = [NSMutableArray new];
    fn_cache = [NSMutableDictionary new]; S.pipelines = [NSMutableDictionary new]; S.dsStates = [NSMutableDictionary new]; S.samplers = [NSMutableDictionary new];
    S.defaultVao = [MVao new];
    float generic[4] = {0, 0, 0, 1}; S.zeroAttr = [S.dev newBufferWithBytes:generic length:64 options:MTLResourceStorageModeShared];
    S.bsrc = S.bsrcA = GL_ONE; S.bdst = S.bdstA = GL_ZERO; S.beq = S.beqA = GL_FUNC_ADD; S.depthFunc = GL_LESS; S.depthMask = YES;
    S.cmask[0] = S.cmask[1] = S.cmask[2] = S.cmask[3] = YES; S.cullFace = GL_BACK; S.frontFace = GL_CCW; S.polyMode = GL_FILL;
    S.clearD = 1; S.unpackAlign = S.packAlign = 4;
    apple_info();
    snprintf(S.renderer, sizeof S.renderer, "%s", S.dev.name.UTF8String);
    snprintf(S.version, sizeof S.version, "4.1 Metal (native port)");
    begin_frame();
    LOG("Metal backend on %s", S.renderer);
}
static void init_device(void);
/* The window the game draws into gets the Metal view and layer. AppKit views are made on the main thread only (the
 * game makes its windows there; it draws on a render thread): sdl.c calls mgl_window_created as each window is made,
 * and attach_layer, on the render thread, only checks that the current window has it. */
static void attach_layer(SDL_Window *w);
static int on_main_thread;
void mgl_window_created(SDL_Window *w)
{
    if (!w || (SDL_GetWindowFlags(w) & SDL_WINDOW_HIDDEN)) return;
    init_device(); on_main_thread = 1; attach_layer(w); on_main_thread = 0;
}
static void attach_layer(SDL_Window *w)
{
    if (!w || w == S.win || (SDL_GetWindowFlags(w) & SDL_WINDOW_HIDDEN)) return;
    if (!on_main_thread && ![NSThread isMainThread]) { static int warned; if (!warned++) LOG("a window without a Metal view (not made on the main thread)"); return; }
    if (S.view) { SDL_Metal_DestroyView(S.view); S.view = 0; S.layer = nil; }
    {
        S.view = SDL_Metal_CreateView(w); S.layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(S.view);
        S.layer.device = S.dev; S.layer.pixelFormat = MTLPixelFormatBGRA8Unorm; S.layer.framebufferOnly = NO;
        S.layer.maximumDrawableCount = 3; S.win = w;
        int dw, dh; SDL_Metal_GetDrawableSize(w, &dw, &dh); S.layer.drawableSize = CGSizeMake(dw, dh);
        S.vp[2] = S.sc[2] = dw; S.vp[3] = S.sc[3] = dh;
        mgl_set_swap_interval(1);
        LOG("drawing into the window (%dx%d drawable)", dw, dh);
    }
}
void *mgl_create_context(SDL_Window *w)
{
    init_device(); attach_layer(w);
    static int ctx; return &ctx;
}
int mgl_make_current(SDL_Window *w, void *c) { (void)c; attach_layer(w); return 0; }
void mgl_delete_context(void *c) { (void)c; }
void mgl_set_swap_interval(int i)
{
    const char *v = getenv("HW_VSYNC"); if (v) i = atoi(v);              /* HW_VSYNC=0: uncapped (benchmarks) */
    if (S.layer) S.layer.displaySyncEnabled = i != 0;
    if (getenv("HW_LOG_FPS")) LOG("vsync %s (layer %p)", i ? "on" : "off", (__bridge void *)S.layer);
}
void mgl_drawable_size(SDL_Window *w, int *dw, int *dh) { SDL_Metal_GetDrawableSize(w, dw, dh); if (S.layer && w == S.win) S.layer.drawableSize = CGSizeMake(*dw, *dh); }
extern void *objc_autoreleasePoolPush(void); extern void objc_autoreleasePoolPop(void *);
void mgl_swap(SDL_Window *w)
{
    /* the game's render thread has no autorelease pool of its own: the objects Metal and Foundation hand back
     * autoreleased (command buffers, descriptors, cache keys) are released frame by frame here */
    static __thread void *pool;
    if (pool) objc_autoreleasePoolPop(pool);
    pool = objc_autoreleasePoolPush();
    if (!S.dev) return;
    attach_layer(w);
    end_pass();
    if (S.layer && S.defColor) {
        static double waited, total, last; static int frames_;
        double t0 = CACurrentMediaTime();
        id<CAMetalDrawable> d = [S.layer nextDrawable];
        double t1 = CACurrentMediaTime(); waited += t1 - t0; frames_++;
        if (last) total += t1 - last; last = t1;
        if (getenv("HW_LOG_FPS") && total > 2) { LOG("frame %.2f ms (%.2f ms waiting for a drawable), GPU %.2f ms, %d CPU waits for the GPU", total * 1000 / frames_, waited * 1000 / frames_, gpu_n ? gpu_ms / gpu_n : 0, stalls); waited = total = 0; frames_ = 0; gpu_ms = 0; gpu_n = 0; stalls = 0; }
        if (d) {
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = d.texture; p.colorAttachments[0].loadAction = MTLLoadActionDontCare; p.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [S.cb renderCommandEncoderWithDescriptor:p];
            [e setRenderPipelineState:blit_pipe(d.texture.pixelFormat)];
            float r[4] = {0, 0, 1, 1};                  /* GL layout (row 0 = bottom): clip-space y up puts row 0 at the drawable's foot */
            [e setVertexBytes:r length:16 atIndex:0];
            static id<MTLSamplerState> lin; if (!lin) { MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; lin = [S.dev newSamplerStateWithDescriptor:sd]; }
            [e setFragmentTexture:S.defColor atIndex:0]; [e setFragmentSamplerState:lin atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [e endEncoding];
            [S.cb presentDrawable:d];
        }
    }
    [S.cb commit];
    begin_frame();
}
int mgl_read_back(int x, int y, int w, int h, void *rgba)
{
    uint32_t keep = S.readFbo; S.readFbo = 0;
    mgl_glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    S.readFbo = keep; return 0;
}

/* ---------------------------------------------------------------- the entry point table */
#define E(n) {#n, (void *)mgl_##n}
const mgl_entry mgl_entries[] = {
    E(glActiveTexture), E(glAttachShader), E(glBindAttribLocation), E(glBindBuffer), E(glBindFramebuffer), E(glBindRenderbuffer),
    E(glBindTexture), E(glBindVertexArray), E(glBlendColor), E(glBlendEquation), E(glBlendEquationSeparate), E(glBlendFunc),
    E(glBlendFuncSeparate), E(glBlitFramebuffer), E(glBufferData), E(glBufferSubData), E(glCheckFramebufferStatus), E(glClear),
    E(glClearColor), E(glClearDepth), E(glClearStencil), E(glColorMask), E(glCompileShader), E(glCompressedTexImage2D),
    E(glCreateProgram), E(glCreateShader), E(glCullFace), E(glDeleteBuffers), E(glDeleteFramebuffers), E(glDeleteProgram),
    E(glDeleteShader), E(glDeleteTextures), E(glDeleteVertexArrays), E(glDepthFunc), E(glDepthMask), E(glDisable),
    E(glDisableVertexAttribArray), E(glDrawArrays), E(glDrawBuffer), E(glDrawBuffers), E(glDrawElements), E(glDrawElementsBaseVertex),
    E(glDrawElementsInstanced), E(glDrawRangeElements), E(glDrawRangeElementsBaseVertex), E(glEnable), E(glEnableVertexAttribArray),
    E(glFinish), E(glFlush), E(glFlushMappedBufferRange), E(glGetBufferParameteriv), E(glMapBuffer), E(glMapBufferRange), E(glFogf), E(glFramebufferTexture2D), E(glFrontFace), E(glGenBuffers), E(glGenerateMipmap),
    E(glGenFramebuffers), E(glGenTextures), E(glGenVertexArrays), E(glGetAttribLocation), E(glGetBooleanv), E(glGetError),
    E(glGetFloatv), E(glGetIntegerv), E(glGetProgramInfoLog), E(glGetProgramiv), E(glGetShaderInfoLog), E(glGetShaderiv),
    E(glGetString), E(glGetStringi), E(glGetTexLevelParameteriv), E(glGetTexParameteriv), E(glGetUniformLocation), E(glIsEnabled),
    E(glLineWidth), E(glLinkProgram), E(glPixelStorei), E(glPointSize), E(glPolygonMode), E(glPolygonOffset), E(glReadBuffer),
    E(glReadPixels), E(glScissor), E(glShaderSource), E(glTexImage2D), E(glTexParameterf), E(glTexParameterfv), E(glTexParameteri),
    E(glTexParameteriv), E(glTexSubImage2D), E(glUniform1f), E(glUniform1fv), E(glUniform1i), E(glUniform1iv), E(glUniform2f),
    E(glUniform2fv), E(glUniform2i), E(glUniform3f), E(glUniform3fv), E(glUniform4f), E(glUniform4fv), E(glUniformMatrix3fv),
    E(glUniformMatrix4fv), E(glUnmapBuffer), E(glUseProgram), E(glVertexAttribPointer), E(glViewport),
};
const unsigned mgl_nentries = sizeof mgl_entries / sizeof mgl_entries[0];
void *mgl_lookup(const char *name)
{
    for (unsigned k = 0; k < mgl_nentries; k++) if (!strcmp(mgl_entries[k].name, name)) return mgl_entries[k].fn;
    return 0;
}
