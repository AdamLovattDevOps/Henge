#include "pvg.h"

#include <sys/resource.h>
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <ParavirtualizedGraphics/ParavirtualizedGraphics.h>
#include <stdatomic.h>
#include <stdio.h>

/* Following QEMU's hw/display/apple-gfx.m and apple-gfx-mmio.m, adapted to
   the framework as it is in macOS 27: guest memory is no longer reached through
   callbacks (createTask/mapMemory/readMemory are obsolete there) but described
   up front as physical ranges with host addresses, and the iosurface mapper's
   MMIO is 64 bits wide. The iosurface mapper and the memory map are private
   interfaces; these declarations match what the runtime reports for them. */
typedef struct PGGuestPhysicalRange_s {
    uint64_t physicalAddress;
    uint64_t physicalLength;
    void *virtualAddress;
} PGGuestPhysicalRange_t;

@interface PGMemoryMapDescriptor : NSObject
- (void)addRange:(PGGuestPhysicalRange_t)range;
@end

@interface PGDeviceDescriptor (Private)
@property (readwrite, nonatomic) bool usingIOSurfaceMapper;
@property (readwrite, nonatomic, retain) PGMemoryMapDescriptor *memoryMapDescriptor;
@end

typedef bool (^IOSFCRaiseInterrupt)(uint32_t vector);

@interface PGIOSurfaceHostDeviceDescriptor : NSObject
@property (readwrite, nonatomic) size_t mmioLength;
@property (readwrite, nonatomic, copy, nullable) IOSFCRaiseInterrupt raiseInterrupt;
@property (readwrite, nonatomic, retain) PGMemoryMapDescriptor *memoryMapDescriptor;
@end

@interface PGIOSurfaceHostDevice : NSObject
- (instancetype)initWithDescriptor:(PGIOSurfaceHostDeviceDescriptor *)desc;
- (uint64_t)mmioReadAtOffset:(size_t)offset;
- (void)mmioWriteAtOffset:(size_t)offset value:(uint64_t)value;
@end

static HvVmapple *board;
static id<PGDevice> pgdev;
static id<PGDisplay> pgdisp;
static PGIOSurfaceHostDevice *iosfc;
static id<MTLDevice> mtl;
static id<MTLCommandQueue> mtl_queue;
static atomic_uint pending_gfx, pending_iosfc;
static atomic_ulong frames;
static uint32_t width = 1920, height = 1080;
static const char *shot_path;
static double last_shot;
static double shot_every = 5; /* seconds between named screenshots (HV_SHOT_SECS, for timing a boot) */

/* ---- guest memory for the framework: every RAM region, by physical address ---- */

static PGMemoryMapDescriptor *memory_map(void) {
    PGMemoryMapDescriptor *map = [PGMemoryMapDescriptor new];
    for (int i = 0; i < board->m->nram; i++) {
        HvRam *r = &board->m->ram[i];
        [map addRange:(PGGuestPhysicalRange_t){r->pa, r->size, r->host}];
    }
    return map;
}

/* ---- frames: rendered offscreen, saved as a PNG now and then ---- */

static double attach_time;

static void save_png(id<MTLTexture> tex) {
    size_t row = (size_t)width * 4;
    NSMutableData *px = [NSMutableData dataWithLength:row * height];
    [tex getBytes:px.mutableBytes bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.mutableBytes, width, height, 8, row, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    /* The latest frame, and one per 5 s named by seconds since start, to see when things happened. */
    static long last_minute = -1;
    long minute = (long)(([NSDate timeIntervalSinceReferenceDate] - attach_time) / shot_every);
    NSString *latest = [NSString stringWithUTF8String:shot_path];
    NSMutableArray *paths = [NSMutableArray arrayWithObject:latest];
    if (minute != last_minute) {
        last_minute = minute;
        [paths addObject:[[latest stringByDeletingPathExtension] stringByAppendingFormat:@"-%04lds.png", (long)(minute * shot_every)]];
        /* The CPU time spent so far, for timing a boot by work done rather than by the clock. */
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        fprintf(stderr, "shot %04lds cpu %.2f s\n", (long)(minute * shot_every),
                ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
    }
    for (NSString *path in paths) {
        CGImageDestinationRef dst =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL);
        if (dst) {
            CGImageDestinationAddImage(dst, img, NULL);
            CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
    }
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
}

static CAMetalLayer *window_layer; /* --window: where frames go */

/* The pointer comes separately (glyph image, hotspot, position), so the window
   draws the screen and then the glyph on top. */
static int cursor_shown = 1;
static PGDisplayCoord_t glyph_hot;
static id<MTLTexture> screen_tex, glyph_tex;
static id<MTLRenderPipelineState> quad_pipe;
static void present(void);

/* A new pointer image: premultiplied BGRA into a texture of its own size. */
static void set_glyph(CGImageRef img, PGDisplayCoord_t hot) {
    if (!img) return;
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    if (!w || !h || w > 1024 || h > 1024) return;
    NSMutableData *px = [NSMutableData dataWithLength:w * h * 4];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.mutableBytes, w, h, 8, w * 4, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [mtl newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px.bytes bytesPerRow:w * 4];
    glyph_tex = t;
    glyph_hot = hot;
    if (window_layer) present();
}

static id<MTLTexture> new_texture(uint32_t w, uint32_t h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    return [mtl newTextureWithDescriptor:td];
}

static id<MTLRenderPipelineState> make_pipe(void) {
    NSString *src = @"#include <metal_stdlib>\n"
                     "using namespace metal;\n"
                     "struct V { float4 pos [[position]]; float2 uv; };\n"
                     "vertex V vs(uint vid [[vertex_id]], constant float4 &r [[buffer(0)]]) {\n"
                     "  float2 c = float2(vid & 1, vid >> 1);\n"
                     "  V o; o.pos = float4(r.xy + c * r.zw, 0, 1); o.uv = float2(c.x, 1 - c.y); return o; }\n"
                     "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
                     "  constexpr sampler s(filter::linear); return t.sample(s, in.uv); }\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [mtl newLibraryWithSource:src options:nil error:&err];
    if (!lib) return nil;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.colorAttachments[0].blendingEnabled = YES; /* the glyph is premultiplied */
    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    return [mtl newRenderPipelineStateWithDescriptor:pd error:&err];
}

/* Draw the screen, then the pointer, into the window. */
static void present(void) {
    if (!screen_tex || !quad_pipe) return;
    if (!CGSizeEqualToSize(window_layer.drawableSize, CGSizeMake(width, height)))
        window_layer.drawableSize = CGSizeMake(width, height);
    id<CAMetalDrawable> drawable = [window_layer nextDrawable];
    if (!drawable) return;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [mtl_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:quad_pipe];
    float full[4] = {-1, -1, 2, 2};
    [enc setVertexBytes:full length:sizeof full atIndex:0];
    [enc setFragmentTexture:screen_tex atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    PGDisplayCoord_t at = pgdisp.cursorPosition;
    if (glyph_tex && cursor_shown && at.x != 0xffff) {
        float w = 2.0f * glyph_tex.width / width, h = 2.0f * glyph_tex.height / height;
        float x = 2.0f * ((float)at.x - glyph_hot.x) / width - 1, top = 1 - 2.0f * ((float)at.y - glyph_hot.y) / height;
        float r[4] = {x, top - h, w, h};
        [enc setVertexBytes:r length:sizeof r atIndex:0];
        [enc setFragmentTexture:glyph_tex atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [enc endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
}

/* A new frame from the guest: copy the screen, then draw it with the pointer. */
static void show_frame(void) {
    if (!quad_pipe) quad_pipe = make_pipe();
    if (!screen_tex || screen_tex.width != width || screen_tex.height != height) screen_tex = new_texture(width, height);
    id<MTLCommandBuffer> cb = [mtl_queue commandBuffer];
    if (![pgdisp encodeCurrentFrameToCommandBuffer:cb texture:screen_tex region:MTLRegionMake2D(0, 0, width, height)]) return;
    [cb commit];
    present();
}

static void new_frame(void) {
    atomic_fetch_add(&frames, 1);
    if (window_layer) show_frame();
    double now = [NSDate timeIntervalSinceReferenceDate];
    if (!shot_path || now - last_shot < (shot_every < 3.0 ? shot_every : 3.0)) return;
    last_shot = now;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:width
                                                                                 height:height
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [mtl newTextureWithDescriptor:td];
    id<MTLCommandBuffer> cb = [mtl_queue commandBuffer];
    if (![pgdisp encodeCurrentFrameToCommandBuffer:cb texture:tex region:MTLRegionMake2D(0, 0, width, height)]) return;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        save_png(tex);
    }];
    [cb commit];
}

/* ---- MMIO, from the CPU thread; the framework is 32-bit only ---- */

/* HV_PVG_TRACE=1: every register access, with the time, for learning the
   protocol an iOS host would have to speak (no PVG there). */
static int pvg_trace = -1;
static double trace_t0;

static void trace(const char *dev, int write, uint64_t off, uint64_t v, unsigned size) {
    if (pvg_trace < 0) {
        pvg_trace = getenv("HV_PVG_TRACE") ? atoi(getenv("HV_PVG_TRACE")) : 0;
        trace_t0 = [NSDate timeIntervalSinceReferenceDate];
    }
    if (!pvg_trace) return;
    fprintf(stderr, "pvgtrace %.4f %s %s +%04llx = %llx (%u) pc %llx\n", [NSDate timeIntervalSinceReferenceDate] - trace_t0,
            dev, write == 2 ? "IRQ" : write ? "W" : "R", (unsigned long long)off, (unsigned long long)v, size,
            (unsigned long long)board->m->access_pc);
}

/* HV_PVG_TRACE=2 also dumps what the guest puts in the FIFO and what the
   host writes back to the shared page (diffs at each interrupt): the data a
   PVG stand-in for iOS would have to reproduce. */
static uint64_t fifo_pa, shared_pa;
static uint32_t fifo_size, fifo_wp;
static uint8_t shared_prev[0x4000];

static void dump_hex(const char *what, uint64_t pa, const uint8_t *p, uint32_t n) {
    fprintf(stderr, "pvgdata %s pa %llx len %u:", what, (unsigned long long)pa, n);
    for (uint32_t i = 0; i < n; i++) fprintf(stderr, "%s%02x", i % 4 ? "" : " ", p[i]);
    fprintf(stderr, "\n");
}

static void trace_data(int write, uint64_t off, uint64_t v) {
    if (pvg_trace < 2) return;
    if (write && off == 0x101c) fifo_pa = v << 14;
    if (write && off == 0x1030) shared_pa = v << 14;
    if (write && off == 0x1004) fifo_size = (uint32_t)v;
    if (write && off == 0x1008 && fifo_pa && fifo_size) {
        uint32_t from = fifo_wp, to = (uint32_t)v;
        for (uint32_t i = from; i != to && i - from < fifo_size; ) {
            uint32_t at = i % fifo_size, n = to - i < fifo_size - at ? to - i : fifo_size - at;
            uint8_t *p = hv_phys_ptr(board->m, fifo_pa + at, n);
            if (p) dump_hex("fifo", fifo_pa + at, p, n);
            i += n;
        }
        fifo_wp = to;
    }
    if (write == 2 && shared_pa) { /* an interrupt: what changed in the shared page */
        uint8_t *p = hv_phys_ptr(board->m, shared_pa, sizeof shared_prev);
        if (!p) return;
        for (uint32_t i = 0; i < sizeof shared_prev; i += 4)
            if (memcmp(p + i, shared_prev + i, 4)) {
                uint32_t nv;
                memcpy(&nv, p + i, 4);
                fprintf(stderr, "pvgdata shared +%04x = %08x\n", i, nv);
            }
        memcpy(shared_prev, p, sizeof shared_prev);
    }
}

static uint64_t gfx_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque;
    uint64_t v = [pgdev mmioReadAtOffset:off];
    if (size == 8) v |= (uint64_t)[pgdev mmioReadAtOffset:off + 4] << 32;
    trace("gfx", 0, off, v, size);
    return v;
}

static void gfx_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)opaque;
    trace("gfx", 1, off, v, size);
    trace_data(1, off, v);
    [pgdev mmioWriteAtOffset:off value:(uint32_t)v];
    if (size == 8) [pgdev mmioWriteAtOffset:off + 4 value:(uint32_t)(v >> 32)];
}

static uint64_t iosfc_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque;
    uint64_t v = [iosfc mmioReadAtOffset:off];
    v = size == 8 ? v : v & ((1ull << (size * 8)) - 1);
    trace("iosfc", 0, off, v, size);
    return v;
}

static void iosfc_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)opaque;
    trace("iosfc", 1, off, v, size);
    [iosfc mmioWriteAtOffset:off value:v];
}

void hv_pvg_poll(void) {
    /* Interrupts arrive on the framework's threads; QEMU pulses the line. */
    if (atomic_exchange(&pending_gfx, 0)) hv_gicv3_pend(board->gic, 32 + 0x11);
    if (atomic_exchange(&pending_iosfc, 0)) hv_gicv3_pend(board->gic, 32 + 0x10);
}

static void swap_handlers(const char *name, uint64_t (*rd)(void *, uint64_t, unsigned),
                          void (*wr)(void *, uint64_t, uint64_t, unsigned)) {
    for (int i = 0; i < board->m->nmmio; i++)
        if (!strcmp(board->m->mmio[i].name, name)) {
            board->m->mmio[i].read = rd;
            board->m->mmio[i].write = wr;
        }
}

int hv_pvg_attach(HvVmapple *b, const char *screenshot) {
    board = b;
    shot_path = screenshot;
    if (getenv("HV_SHOT_SECS")) shot_every = atof(getenv("HV_SHOT_SECS"));
    attach_time = [NSDate timeIntervalSinceReferenceDate];
    @autoreleasepool {
        mtl = MTLCreateSystemDefaultDevice();
        mtl_queue = [mtl newCommandQueue];

        PGIOSurfaceHostDeviceDescriptor *idesc = [PGIOSurfaceHostDeviceDescriptor new];
        idesc.mmioLength = 0x10000;
        idesc.memoryMapDescriptor = memory_map();
        idesc.raiseInterrupt = ^bool(uint32_t vector) {
            (void)vector;
            atomic_store(&pending_iosfc, 1);
            trace("iosfc", 2, vector, 0, 0);
            hv_machine_kick_all(board->m);
            return true;
        };
        iosfc = [[PGIOSurfaceHostDevice alloc] initWithDescriptor:idesc];
        if (!iosfc) return -1;

        PGDeviceDescriptor *desc = [PGDeviceDescriptor new];
        desc.device = mtl;
        desc.usingIOSurfaceMapper = true;
        desc.memoryMapDescriptor = memory_map();
        desc.raiseInterrupt = ^(uint32_t vector) {
            (void)vector;
            atomic_store(&pending_gfx, 1);
            trace("gfx", 2, vector, 0, 0);
            trace_data(2, 0, 0);
            hv_machine_kick_all(board->m);
        };
        pgdev = PGCreateDeviceWithDescriptor(desc);
        if (!pgdev) return -1;

        PGDisplayDescriptor *dd = [PGDisplayDescriptor new];
        dd.name = @"HengeVM display";
        dd.sizeInMillimeters = NSMakeSize(400., 300.);
        dd.queue = dispatch_get_main_queue();
        dd.newFrameEventHandler = ^{
            new_frame();
        };
        dd.modeChangeHandler = ^(PGDisplayCoord_t size, OSType format) {
            (void)format;
            if (board->m->log) fprintf(stderr, "hengevm: pvg: display mode %ux%u\n", size.x, size.y);
            if (size.x < 320 || size.y < 240) return; /* a pointer glyph passed off as a mode: not the screen */
            width = size.x;
            height = size.y;
        };
        /* The pointer is not drawn into frames: it moves separately, so redraw the window when it does. */
        /* macOS 27's private replacement for the obsolete cursorGlyphHandler:
           (CGImageRef glyph, PGDisplayCoord_t hotSpot). Without one the framework
           passes the glyph off as a 128x128 display mode and frame. */
        void (^glyph_handler)(id, PGDisplayCoord_t) = ^(id image, PGDisplayCoord_t hot) {
            set_glyph((__bridge CGImageRef)image, hot);
        };
        SEL set_glyph_handler = NSSelectorFromString(@"setCursorGlyphHandler2:");
        if ([dd respondsToSelector:set_glyph_handler])
            ((void (*)(id, SEL, id))objc_msgSend)(dd, set_glyph_handler, glyph_handler);
        dd.cursorShowHandler = ^(BOOL show) {
            cursor_shown = show;
            if (window_layer) present();
        };
        dd.cursorMoveHandler = ^{
            if (window_layer) present();
        };
        pgdisp = [pgdev newDisplayWithDescriptor:dd port:0 serialNum:1];
        PGDisplayMode *mode = [[PGDisplayMode alloc] initWithSizeInPixels:(PGDisplayCoord_t){1920, 1080}
                                                         refreshRateInHz:60];
        pgdisp.modeList = @[ mode ];
    }
    swap_handlers("gfx", gfx_read, gfx_write);
    swap_handlers("iosurface", iosfc_read, iosfc_write);
    return 0;
}

/* ---- --window: a Mac window showing the display, with keyboard and mouse ---- */

/* macOS virtual key codes (kVK_*) to Linux input key codes. */
static const uint16_t keymap[128] = {
    [0x00] = 30,  [0x01] = 31,  [0x02] = 32,  [0x03] = 33,  [0x04] = 35,  [0x05] = 34,  [0x06] = 44,  [0x07] = 45,
    [0x08] = 46,  [0x09] = 47,  [0x0a] = 86,  [0x0b] = 48,  [0x0c] = 16,  [0x0d] = 17,  [0x0e] = 18,  [0x0f] = 19,
    [0x10] = 21,  [0x11] = 20,  [0x12] = 2,   [0x13] = 3,   [0x14] = 4,   [0x15] = 5,   [0x16] = 7,   [0x17] = 6,
    [0x18] = 13,  [0x19] = 10,  [0x1a] = 8,   [0x1b] = 12,  [0x1c] = 9,   [0x1d] = 11,  [0x1e] = 27,  [0x1f] = 24,
    [0x20] = 22,  [0x21] = 26,  [0x22] = 23,  [0x23] = 25,  [0x24] = 28,  [0x25] = 38,  [0x26] = 36,  [0x27] = 40,
    [0x28] = 37,  [0x29] = 39,  [0x2a] = 43,  [0x2b] = 51,  [0x2c] = 53,  [0x2d] = 49,  [0x2e] = 50,  [0x2f] = 52,
    [0x30] = 15,  [0x31] = 57,  [0x32] = 41,  [0x33] = 14,  [0x35] = 1,   [0x36] = 126, [0x37] = 125, [0x38] = 42,
    [0x39] = 58,  [0x3a] = 56,  [0x3b] = 29,  [0x3c] = 54,  [0x3d] = 100, [0x3e] = 97,  [0x40] = 187, [0x41] = 83,
    [0x43] = 55,  [0x45] = 78,  [0x47] = 69,  [0x48] = 115, [0x49] = 114, [0x4a] = 113, [0x4b] = 98,  [0x4c] = 96,
    [0x4e] = 74,  [0x4f] = 188, [0x50] = 189, [0x51] = 117, [0x52] = 82,  [0x53] = 79,  [0x54] = 80,  [0x55] = 81,
    [0x56] = 75,  [0x57] = 76,  [0x58] = 77,  [0x59] = 71,  [0x5b] = 72,  [0x5c] = 73,  [0x60] = 63,  [0x61] = 64,
    [0x62] = 65,  [0x63] = 61,  [0x64] = 66,  [0x65] = 67,  [0x67] = 87,  [0x69] = 183, [0x6a] = 186, [0x6b] = 184,
    [0x6d] = 68,  [0x6f] = 88,  [0x71] = 185, [0x72] = 110, [0x73] = 102, [0x74] = 104, [0x75] = 111, [0x76] = 62,
    [0x77] = 107, [0x78] = 60,  [0x79] = 109, [0x7a] = 59,  [0x7b] = 105, [0x7c] = 106, [0x7d] = 108, [0x7e] = 103,
};

static void key(uint16_t mac, int down) {
    if (mac >= 128 || !keymap[mac]) return;
    hv_xhci_key_linux(board->usb, keymap[mac], down); /* the USB keyboard: the one macOS listens to */
    hv_virtio_input_send(board->keyboard, HV_EV_KEY, keymap[mac], down);
    hv_virtio_input_send(board->keyboard, HV_EV_SYN, 0, 0);
}

@interface HVView : NSView
@property BOOL captured;
@end

@implementation HVView {
    NSUInteger held_modifiers; /* bit per modifier key code, to tell presses from releases */
}
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer { return window_layer; }

- (void)setCapture:(BOOL)on {
    if (on == self.captured) return;
    self.captured = on;
    CGAssociateMouseAndMouseCursorPosition(!on);
    if (on) {
        [NSCursor hide];
        /* Keep the (frozen) host pointer over the window so clicks stay ours. */
        NSRect r = [self.window convertRectToScreen:[self convertRect:self.bounds toView:nil]];
        CGFloat top = NSScreen.screens.firstObject.frame.size.height;
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(r), top - NSMidY(r)));
    } else {
        [NSCursor unhide];
    }
    self.window.title = on ? @"HengeVM — press Control+Option to release the mouse" : @"HengeVM — click to use the mouse";
}

- (void)keyDown:(NSEvent *)e {
    if (!e.isARepeat) key(e.keyCode, 1);
}
- (void)keyUp:(NSEvent *)e { key(e.keyCode, 0); }
- (void)flagsChanged:(NSEvent *)e {
    uint16_t k = e.keyCode;
    if (k == 0x39) { /* Caps Lock reports each toggle: tap it */
        key(k, 1);
        key(k, 0);
    } else if (k < 64) {
        NSUInteger bit = (NSUInteger)1 << k;
        int down = !(held_modifiers & bit);
        held_modifiers ^= bit;
        key(k, down);
    }
    NSEventModifierFlags f = e.modifierFlags;
    if (self.captured && (f & NSEventModifierFlagControl) && (f & NSEventModifierFlagOption)) {
        /* Release the guest's Control and Option too, so they do not stay down there. */
        for (uint16_t mk = 0x3a; mk <= 0x3e; mk++)
            if (held_modifiers & ((NSUInteger)1 << mk)) {
                held_modifiers &= ~((NSUInteger)1 << mk);
                key(mk, 0);
            }
        [self setCapture:NO];
    }
}

- (void)moved:(NSEvent *)e {
    if (!self.captured) return;
    int dx = (int)lround(e.deltaX), dy = (int)lround(e.deltaY);
    if (!dx && !dy) return;
    if (dx) hv_virtio_input_send(board->mouse, HV_EV_REL, HV_REL_X, dx);
    if (dy) hv_virtio_input_send(board->mouse, HV_EV_REL, HV_REL_Y, dy);
    hv_virtio_input_send(board->mouse, HV_EV_SYN, 0, 0);
}
- (void)button:(uint16_t)code down:(int)down {
    hv_virtio_input_send(board->mouse, HV_EV_KEY, code, down);
    hv_virtio_input_send(board->mouse, HV_EV_SYN, 0, 0);
}
- (void)mouseMoved:(NSEvent *)e { [self moved:e]; }
- (void)mouseDragged:(NSEvent *)e { [self moved:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self moved:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self moved:e]; }
- (void)mouseDown:(NSEvent *)e {
    if (!self.captured) { /* the first click only takes the mouse */
        [self setCapture:YES];
        return;
    }
    [self button:HV_BTN_LEFT down:1];
}
- (void)mouseUp:(NSEvent *)e {
    if (self.captured) [self button:HV_BTN_LEFT down:0];
}
- (void)rightMouseDown:(NSEvent *)e {
    if (self.captured) [self button:HV_BTN_RIGHT down:1];
}
- (void)rightMouseUp:(NSEvent *)e {
    if (self.captured) [self button:HV_BTN_RIGHT down:0];
}
- (void)otherMouseDown:(NSEvent *)e {
    if (self.captured) [self button:HV_BTN_MIDDLE down:1];
}
- (void)otherMouseUp:(NSEvent *)e {
    if (self.captured) [self button:HV_BTN_MIDDLE down:0];
}
- (void)scrollWheel:(NSEvent *)e {
    if (!self.captured) return;
    /* Wheel clicks; trackpads give fine deltas, so gather them into whole steps. */
    static double acc_y, acc_x;
    acc_y += e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 10.0 : e.scrollingDeltaY;
    acc_x += e.hasPreciseScrollingDeltas ? e.scrollingDeltaX / 10.0 : e.scrollingDeltaX;
    int sy = (int)acc_y, sx = (int)acc_x;
    acc_y -= sy;
    acc_x -= sx;
    if (sy) hv_virtio_input_send(board->mouse, HV_EV_REL, HV_REL_WHEEL, sy);
    if (sx) hv_virtio_input_send(board->mouse, HV_EV_REL, HV_REL_HWHEEL, -sx);
    if (sy || sx) hv_virtio_input_send(board->mouse, HV_EV_SYN, 0, 0);
}
@end

@interface HVWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation HVWindowDelegate
- (void)windowDidResignKey:(NSNotification *)n {
    HVView *v = (HVView *)((NSWindow *)n.object).contentView;
    [v setCapture:NO];
}
- (void)windowWillClose:(NSNotification *)n {
    (void)n;
    exit(0);
}
@end

void hv_pvg_window_run(void) {
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        window_layer = [CAMetalLayer layer];
        window_layer.device = mtl;
        window_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        window_layer.framebufferOnly = NO;
        window_layer.drawableSize = CGSizeMake(width, height);
        window_layer.contentsGravity = kCAGravityResizeAspect;
        /* Half the guest's pixels in points: 1:1 on a Retina screen. */
        NSRect frame = NSMakeRect(0, 0, width / 2.0, height / 2.0);
        NSWindow *w = [[NSWindow alloc] initWithContentRect:frame
                                                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        HVView *v = [[HVView alloc] initWithFrame:frame];
        v.wantsLayer = YES;
        w.contentView = v;
        w.contentAspectRatio = NSMakeSize(width, height);
        w.acceptsMouseMovedEvents = YES;
        static HVWindowDelegate *delegate;
        delegate = [HVWindowDelegate new];
        w.delegate = delegate;
        w.title = @"HengeVM — click to use the mouse";
        [w center];
        [w makeKeyAndOrderFront:nil];
        [w makeFirstResponder:v];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
}
