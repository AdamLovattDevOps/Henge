#include "pvg.h"

#import <Foundation/Foundation.h>
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
    /* The latest frame, and one per 15 s named by seconds since start, to see when things happened. */
    static long last_minute = -1;
    long minute = (long)(([NSDate timeIntervalSinceReferenceDate] - attach_time) / 15);
    NSString *latest = [NSString stringWithUTF8String:shot_path];
    NSMutableArray *paths = [NSMutableArray arrayWithObject:latest];
    if (minute != last_minute) {
        last_minute = minute;
        [paths addObject:[[latest stringByDeletingPathExtension] stringByAppendingFormat:@"-%04lds.png", minute * 15]];
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

static void new_frame(void) {
    atomic_fetch_add(&frames, 1);
    double now = [NSDate timeIntervalSinceReferenceDate];
    if (!shot_path || now - last_shot < 3.0) return;
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

static uint64_t gfx_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque;
    uint64_t v = [pgdev mmioReadAtOffset:off];
    if (size == 8) v |= (uint64_t)[pgdev mmioReadAtOffset:off + 4] << 32;
    return v;
}

static void gfx_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)opaque;
    [pgdev mmioWriteAtOffset:off value:(uint32_t)v];
    if (size == 8) [pgdev mmioWriteAtOffset:off + 4 value:(uint32_t)(v >> 32)];
}

static uint64_t iosfc_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque;
    uint64_t v = [iosfc mmioReadAtOffset:off];
    return size == 8 ? v : v & ((1ull << (size * 8)) - 1);
}

static void iosfc_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)opaque, (void)size;
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
            __atomic_store_n(&board->cpu->exit_req, 1, __ATOMIC_RELAXED);
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
            __atomic_store_n(&board->cpu->exit_req, 1, __ATOMIC_RELAXED);
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
            width = size.x;
            height = size.y;
            if (board->m->log) fprintf(stderr, "hengevm: pvg: display mode %ux%u\n", width, height);
        };
        dd.cursorShowHandler = ^(BOOL show) {
            (void)show;
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
