/* Runs the guest's GPU commands on the host's Metal, on any Apple platform.
   The guest driver serializes Metal calls: an exec names command buffers in a
   task's memory, each a segment holding {tag, size} records, one per call, in
   the formats worked out from the guest driver and listed by the decoders
   below. Objects are named by a number (a ref); the task's object list says
   what each is, and they are made the first time they are used. */

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <TargetConditionals.h>

#include <mach/mach.h>
#include <mach/vm_map.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pvg_lite.h"

#if !TARGET_OS_IPHONE
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

/* Metal's own pipeline serialization, on _MTLDevice. The guest's Metal wrote the data. */
@protocol HvMetalSPI
- (id)newRenderPipelineDescriptorWithSerializedData:(dispatch_data_t)d deserializationContext:(id)c;
- (id)newTileRenderPipelineDescriptorWithSerializedData:(dispatch_data_t)d deserializationContext:(id)c;
@end

enum { OBJ_BUFFER = 1, OBJ_TEXTURE = 2, OBJ_FUNCTION = 6, OBJ_SERIALIZED = 7, OBJ_LINEAR_TEXTURE = 8, OBJ_MAPPED_TEXTURE = 0xb };

/* What is known about a made object, so a reused ref is noticed. */
@interface HvObject : NSObject {
@public
    id object;
    uint32_t entry[3];
    /* A buffer's guest pages, mapped contiguously. */
    uint8_t *mapped;
    uint64_t length;
    uint32_t vpage;
    uint32_t *pages;
    /* A texture's layout in guest memory. */
    uint32_t tex_mips, tex_slices;
    uint64_t tex_base, tex_stride;
    uint8_t tex_block_h;
    uint8_t *tex_layout; /* the mip table, 36 bytes each */
    BOOL guest_backed;
}
@end

@implementation HvObject
- (void)dealloc {
    if (mapped) vm_deallocate(mach_task_self(), (vm_address_t)mapped, length);
    free(pages);
    free(tex_layout);
}
@end

@class HvContext;

@interface HvReplay : NSObject {
@public
    HvPvgLite *lite;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    dispatch_queue_t worker;
    NSMutableDictionary<NSNumber *, NSMutableDictionary<NSNumber *, HvObject *> *> *tasks;
    NSMutableDictionary<NSNumber *, id<MTLTexture>> *mapped_textures;
    NSMutableSet<NSString *> *complaints;
    uint64_t execs, draws, failed;
    /* The newest frame, BGRA, for hv_pvg_metal_frame. */
    uint8_t *frame;
    uint32_t frame_w, frame_h;
    double last_capture;
    id<MTLBuffer> readback;
    const char *shot;
    unsigned shots;
}
- (void)complain:(NSString *)what;
- (HvObject *)object:(uint32_t)ref task:(uint32_t)task;
@end

/* What Metal asks while it reads a serialized pipeline. */
@interface HvContext : NSObject {
@public
    __unsafe_unretained HvReplay *replay;
    uint32_t task;
    NSString *failure;
}
@end

static double now_seconds(void) {
    return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC) * 1e-9;
}

/* A contiguous view of a task's guest pages; holes are left as zero pages. */
static BOOL map_pages(HvReplay *r, uint32_t task, HvObject *o) {
    uint64_t n = (o->length + 0x3fff) >> 14;
    vm_address_t base = 0;
    if (vm_allocate(mach_task_self(), &base, n << 14, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return NO;
    o->mapped = (uint8_t *)base;
    o->pages = calloc(n, 4);
    for (uint64_t i = 0; i < n; i++) {
        uint32_t gp = hv_pvg_lite_task_page(r->lite, task, ((uint64_t)o->vpage + i) << 14);
        if (!gp) continue;
        uint8_t *src = hv_pvg_lite_page(r->lite, gp);
        if (!src) continue;
        vm_address_t to = base + (i << 14);
        vm_prot_t cur, max;
        kern_return_t kr = vm_remap(mach_task_self(), &to, 0x4000, 0, VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)src, FALSE, &cur, &max, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "hengevm: pvg-metal: vm_remap failed: %d\n", kr);
            return NO;
        }
        o->pages[i] = gp;
    }
    return YES;
}

/* The page table changes under a buffer as the guest maps it in. */
static void refresh_pages(HvReplay *r, uint32_t task, HvObject *o) {
    if (!o->mapped) return;
    uint64_t n = (o->length + 0x3fff) >> 14;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t gp = hv_pvg_lite_task_page(r->lite, task, ((uint64_t)o->vpage + i) << 14);
        if (gp == o->pages[i] || !gp) continue;
        uint8_t *src = hv_pvg_lite_page(r->lite, gp);
        if (!src) continue;
        vm_address_t to = (vm_address_t)o->mapped + (i << 14);
        vm_prot_t cur, max;
        if (vm_remap(mach_task_self(), &to, 0x4000, 0, VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)src, FALSE, &cur, &max, VM_INHERIT_NONE) == KERN_SUCCESS)
            o->pages[i] = gp;
    }
}

/* The two serialized forms of a texture description. */
static MTLTextureDescriptor *descriptor_v1(const uint8_t *d) {
    MTLTextureDescriptor *t = [MTLTextureDescriptor new];
    t.textureType = (MTLTextureType)(d[0] & 0xf);
    t.usage = d[1];
    t.pixelFormat = (MTLPixelFormat)(d[2] | d[3] << 8);
    t.width = *(const uint32_t *)(d + 4);
    t.height = *(const uint32_t *)(d + 8);
    t.depth = *(const uint32_t *)(d + 12);
    t.mipmapLevelCount = *(const uint16_t *)(d + 16);
    t.sampleCount = *(const uint16_t *)(d + 18);
    t.arrayLength = *(const uint16_t *)(d + 20);
    return t;
}

static MTLTextureDescriptor *descriptor_v2(const uint8_t *d) {
    MTLTextureDescriptor *t = [MTLTextureDescriptor new];
    uint32_t usage, w, h, depth;
    uint16_t mips, samples, array;
    memcpy(&usage, d + 3, 4), memcpy(&w, d + 7, 4), memcpy(&h, d + 11, 4), memcpy(&depth, d + 15, 4);
    memcpy(&mips, d + 19, 2), memcpy(&samples, d + 21, 2), memcpy(&array, d + 23, 2);
    t.textureType = (MTLTextureType)(d[0] & 0xf);
    t.pixelFormat = (MTLPixelFormat)(d[1] | d[2] << 8);
    t.usage = usage;
    t.width = w, t.height = h, t.depth = depth;
    t.mipmapLevelCount = mips, t.sampleCount = samples, t.arrayLength = array;
    return t;
}

@implementation HvContext
- (void)resetFailureState { failure = nil; }
- (BOOL)hadLookupFailure { return failure != nil; }
- (NSString *)firstFailureReason { return failure; }
- (id)functionWithFunctionRef:(unsigned long long)ref {
    HvObject *o = [replay object:(uint32_t)ref task:task];
    if (!o || !o->object) {
        if (!failure) failure = [NSString stringWithFormat:@"function %llu", ref];
        return nil;
    }
    return o->object;
}
@end

@implementation HvReplay

- (void)complain:(NSString *)what {
    if ([complaints containsObject:what]) return;
    [complaints addObject:what];
    fprintf(stderr, "hengevm: pvg-metal: %s\n", what.UTF8String);
}

- (id)makeFunction:(uint64_t)at task:(uint32_t)task {
    struct __attribute__((packed)) { uint64_t off; uint32_t len; uint32_t tag, size; } h;
    if (hv_pvg_lite_task_read(lite, task, at, &h, sizeof h)) return nil;
    NSMutableData *lib = [NSMutableData dataWithLength:h.len];
    if (hv_pvg_lite_task_read(lite, task, h.off, lib.mutableBytes, h.len)) return nil;
    dispatch_data_t data = dispatch_data_create(lib.bytes, lib.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *err = nil;
    id<MTLLibrary> library = [device newLibraryWithData:data error:&err];
    if (!library) {
        [self complain:[NSString stringWithFormat:@"library: %@", err.localizedDescription]];
        return nil;
    }
    return [library newFunctionWithName:library.functionNames.firstObject];
}

- (id)makeSerialized:(uint64_t)at length:(uint32_t)size task:(uint32_t)task {
    NSMutableData *all = [NSMutableData dataWithLength:size];
    if (hv_pvg_lite_task_read(lite, task, at, all.mutableBytes, size)) return nil;
    const uint32_t *w = all.bytes;
    uint32_t tag = w[0];
    if (tag == 0xe || tag == 0x33) { /* render or tile pipeline: {tag, size, ref, length, data} */
        id<HvMetalSPI> spi = (id)device;
        HvContext *ctx = [HvContext new];
        ctx->replay = self, ctx->task = task;
        dispatch_data_t data = dispatch_data_create((const uint8_t *)all.bytes + 16, w[3], NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        id descriptor = tag == 0xe ? [spi newRenderPipelineDescriptorWithSerializedData:data deserializationContext:ctx]
                                   : [spi newTileRenderPipelineDescriptorWithSerializedData:data deserializationContext:ctx];
        if (!descriptor || ctx->failure) {
            [self complain:[NSString stringWithFormat:@"pipeline descriptor %u: %@", tag, ctx->failure]];
            return nil;
        }
        NSError *err = nil;
        id state = tag == 0xe ? [device newRenderPipelineStateWithDescriptor:descriptor error:&err]
                              : [device newRenderPipelineStateWithTileDescriptor:descriptor options:MTLPipelineOptionNone reflection:nil error:&err];
        if (!state) [self complain:[NSString stringWithFormat:@"pipeline %u: %@", tag, err.localizedDescription]];
        return state;
    }
    if (tag == 3) { /* sampler: {tag, size, ref, bits, lod min, lod max, ...} */
        uint64_t bits;
        memcpy(&bits, w + 3, 8);
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        d.minFilter = (MTLSamplerMinMagFilter)(bits & 3);
        d.magFilter = (MTLSamplerMinMagFilter)((bits >> 2) & 3);
        d.lodAverage = (bits >> 4) & 1;
        d.compareFunction = (MTLCompareFunction)((bits >> 5) & 7);
        d.sAddressMode = (MTLSamplerAddressMode)((bits >> 8) & 15);
        d.tAddressMode = (MTLSamplerAddressMode)((bits >> 12) & 15);
        d.rAddressMode = (MTLSamplerAddressMode)((bits >> 16) & 15);
        d.borderColor = (MTLSamplerBorderColor)((bits >> 20) & 15);
        d.mipFilter = (MTLSamplerMipFilter)((bits >> 24) & 3);
        NSUInteger aniso = (bits >> 26) & 31;
        d.maxAnisotropy = aniso ? aniso : 1;
        d.normalizedCoordinates = (bits >> 31) & 1;
        d.supportArgumentBuffers = (bits >> 32) & 1;
        float lod[2];
        memcpy(lod, w + 5, 8);
        d.lodMinClamp = lod[0], d.lodMaxClamp = lod[1];
        id s = [device newSamplerStateWithDescriptor:d];
        if (!s) [self complain:@"sampler failed"];
        return s;
    }
    [self complain:[NSString stringWithFormat:@"serialized object tag %u", tag]];
    return nil;
}

- (void)makeBuffer:(HvObject *)o at:(uint64_t)at task:(uint32_t)task {
    struct __attribute__((packed)) { uint64_t length; uint32_t vpage; uint32_t flags; } h;
    if (hv_pvg_lite_task_read(lite, task, at, &h, sizeof h)) return;
    o->length = (h.length + 0x3fff) & ~0x3fffull;
    o->vpage = h.vpage;
    if (!map_pages(self, task, o)) return;
    o->object = [device newBufferWithBytesNoCopy:o->mapped length:o->length options:MTLResourceStorageModeShared deallocator:nil];
    if (!o->object) [self complain:@"buffer failed"];
}

- (void)makeTexture:(HvObject *)o at:(uint64_t)at size:(uint32_t)size task:(uint32_t)task {
    NSMutableData *all = [NSMutableData dataWithLength:size];
    if (hv_pvg_lite_task_read(lite, task, at, all.mutableBytes, size)) return;
    const uint8_t *d = all.bytes;
    /* {length, page}, a layout table, {tag, size, ref}, the description. */
    memcpy(&o->length, d, 8);
    memcpy(&o->vpage, d + 8, 4);
    const uint8_t *layout = d + 12;
    o->tex_mips = *(const uint16_t *)layout & 0x3fff;
    o->tex_slices = *(const uint16_t *)(layout + 2);
    memcpy(&o->tex_base, layout + 4, 8);
    memcpy(&o->tex_stride, layout + 12, 8);
    o->tex_block_h = layout[0x15];
    uint32_t table = o->tex_mips * 36;
    if (12 + 0x18 + table + 12 > size) { [self complain:@"texture entry too small"]; return; }
    o->tex_layout = malloc(table);
    memcpy(o->tex_layout, layout + 0x18, table);
    const uint8_t *rec = layout + 0x18 + table;
    uint32_t tag = *(const uint32_t *)rec;
    MTLTextureDescriptor *t = (tag == 0x34 || tag == 0x39) ? descriptor_v2(rec + 12) : descriptor_v1(rec + 12);
    t.storageMode = MTLStorageModePrivate;
    t.usage |= MTLTextureUsageShaderRead;
    o->object = [device newTextureWithDescriptor:t];
    o->guest_backed = YES;
    if (!o->object) [self complain:[NSString stringWithFormat:@"texture format %lu %lux%lu failed", (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height]];
}

/* A texture over a buffer: {tag 9, size, ref, buffer ref, offset, bytes per row, description}. */
- (void)makeLinearTexture:(HvObject *)o at:(uint64_t)at task:(uint32_t)task {
    struct __attribute__((packed)) { uint32_t tag, size, ref, buffer; uint64_t offset, row; uint8_t d[32]; } h;
    if (hv_pvg_lite_task_read(lite, task, at, &h, sizeof h)) return;
    if (h.tag != 9) { [self complain:[NSString stringWithFormat:@"linear texture tag %u", h.tag]]; return; }
    HvObject *b = [self object:h.buffer task:task];
    id<MTLBuffer> buffer = b ? b->object : nil;
    if (!buffer || ![buffer conformsToProtocol:@protocol(MTLBuffer)]) { [self complain:@"linear texture without a buffer"]; return; }
    MTLTextureDescriptor *t = descriptor_v1(h.d);
    t.storageMode = MTLStorageModeShared;
    t.usage |= MTLTextureUsageShaderRead;
    o->object = [buffer newTextureWithDescriptor:t offset:h.offset bytesPerRow:h.row];
    if (!o->object) [self complain:[NSString stringWithFormat:@"linear texture format %lu row %llu failed", (unsigned long)t.pixelFormat, h.row]];
}

/* A surface the display can show: {mapping id, tag, size, ref, description}. */
- (void)makeMappedTexture:(HvObject *)o at:(uint64_t)at task:(uint32_t)task {
    struct __attribute__((packed)) { uint64_t mapping; uint32_t tag, size, ref; uint8_t d[32]; } h;
    if (hv_pvg_lite_task_read(lite, task, at, &h, sizeof h)) return;
    NSNumber *key = @(h.mapping);
    id<MTLTexture> tex = mapped_textures[key];
    if (!tex) {
        MTLTextureDescriptor *t = h.tag == 0x39 ? descriptor_v2(h.d) : descriptor_v1(h.d);
        t.storageMode = MTLStorageModePrivate;
        t.usage |= MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        tex = [device newTextureWithDescriptor:t];
        if (tex) mapped_textures[key] = tex;
        fprintf(stderr, "hengevm: pvg-metal: surface %llu: %lux%lu format %lu\n", h.mapping, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat);
    }
    o->object = tex;
}

- (HvObject *)object:(uint32_t)ref task:(uint32_t)task {
    if (!ref) return nil;
    uint64_t list = hv_pvg_lite_task_objects(lite, task);
    uint32_t e[3];
    if (!list || hv_pvg_lite_task_read(lite, task, list + (uint64_t)ref * 12, e, 12)) return nil;
    NSMutableDictionary *objs = tasks[@(task)];
    if (!objs) tasks[@(task)] = objs = [NSMutableDictionary new];
    HvObject *o = objs[@(ref)];
    if (o && !memcmp(o->entry, e, 12)) return o;
    o = [HvObject new];
    memcpy(o->entry, e, 12);
    objs[@(ref)] = o;
    uint32_t type = e[0] & 0xff, size = e[0] >> 8;
    uint64_t at = (uint64_t)e[2] << 32 | e[1];
    switch (type) {
    case OBJ_BUFFER: [self makeBuffer:o at:at task:task]; break;
    case OBJ_TEXTURE: [self makeTexture:o at:at size:size task:task]; break;
    case OBJ_FUNCTION: o->object = [self makeFunction:at task:task]; break;
    case OBJ_SERIALIZED: o->object = [self makeSerialized:at length:size task:task]; break;
    case OBJ_LINEAR_TEXTURE: [self makeLinearTexture:o at:at task:task]; break;
    case OBJ_MAPPED_TEXTURE: [self makeMappedTexture:o at:at task:task]; break;
    default: [self complain:[NSString stringWithFormat:@"object type %u", type]]; break;
    }
    return o;
}

- (id)get:(uint32_t)ref task:(uint32_t)task {
    HvObject *o = [self object:ref task:task];
    return o ? o->object : nil;
}

/* Guest bytes into a texture, for a texture the guest says it changed. */
- (void)upload:(HvObject *)o task:(uint32_t)task blit:(id<MTLBlitCommandEncoder> *)blit commandBuffer:(id<MTLCommandBuffer>)cb {
    id<MTLTexture> tex = o->object;
    if (!tex || !o->tex_layout || !o->length) return;
    uint64_t base = (uint64_t)o->vpage << 14;
    NSUInteger faces = tex.textureType == MTLTextureTypeCube || tex.textureType == MTLTextureTypeCubeArray ? 6 : 1;
    id<MTLBuffer> staging = [device newBufferWithLength:o->length options:MTLResourceStorageModeShared];
    if (!staging || hv_pvg_lite_task_read(lite, task, base, staging.contents, (uint32_t)o->length)) {
        [self complain:@"texture bytes unreadable"];
        return;
    }
    if (!*blit) *blit = [cb blitCommandEncoder];
    for (uint32_t s = 0; s < o->tex_slices * faces; s++)
        for (uint32_t m = 0; m < o->tex_mips; m++) {
            const uint8_t *mip = o->tex_layout + m * 36;
            uint64_t off, row;
            uint32_t w, h, d;
            memcpy(&off, mip, 8), memcpy(&row, mip + 0x10, 8), memcpy(&w, mip + 0x18, 4), memcpy(&h, mip + 0x1c, 4), memcpy(&d, mip + 0x20, 4);
            uint32_t bh = o->tex_block_h ? o->tex_block_h : 1;
            uint64_t from = o->tex_base + o->tex_stride * s + off;
            uint64_t image = row * ((h + bh - 1) / bh);
            if (from + image * d > o->length) continue;
            [*blit copyFromBuffer:staging sourceOffset:from sourceBytesPerRow:row sourceBytesPerImage:image
                       sourceSize:MTLSizeMake(w, h, d) toTexture:tex destinationSlice:s destinationLevel:m destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
}

/* A record's bytes, as a pointer into the buffer's private copy. */
typedef struct {
    const uint8_t *p;
    uint32_t size;
} Rec;

#define U32(off) (*(const uint32_t *)(r.p + (off)))
#define U16(off) (*(const uint16_t *)(r.p + (off)))
#define U64(off) ({ uint64_t _v; memcpy(&_v, r.p + (off), 8); _v; })

- (id<MTLRenderCommandEncoder>)beginPass:(Rec)r task:(uint32_t)task commandBuffer:(id<MTLCommandBuffer>)cb {
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
#define ATTACH(a, o)                                                 \
    do {                                                             \
        a.texture = [self get:U32((o)) task:task];                   \
        a.resolveTexture = [self get:U32((o) + 4) task:task];        \
        a.level = U16((o) + 8), a.slice = U16((o) + 10);             \
        a.depthPlane = U16((o) + 12), a.resolveLevel = U16((o) + 14); \
        a.resolveSlice = U16((o) + 16), a.resolveDepthPlane = U16((o) + 18); \
        a.loadAction = (MTLLoadAction)U16((o) + 20);                 \
        a.storeAction = (MTLStoreAction)U16((o) + 22);               \
        a.storeActionOptions = U16((o) + 24);                        \
    } while (0)
    ATTACH(d.depthAttachment, 0);
    double clear_depth;
    memcpy(&clear_depth, r.p + 0x1c, 8);
    d.depthAttachment.clearDepth = clear_depth;
    ATTACH(d.stencilAttachment, 0x28);
    d.stencilAttachment.clearStencil = U32(0x44);
    for (int i = 0; i < 8; i++) {
        uint32_t o = 0x4c + (uint32_t)i * 0x3c;
        if (o + 0x3c > r.size) break;
        MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
        ATTACH(c, o);
        double col[4];
        memcpy(col, r.p + o + 0x1c, sizeof col);
        c.clearColor = MTLClearColorMake(col[0], col[1], col[2], col[3]);
    }
    /* Memoryless and resolve store actions want the matching textures; a pass with none stays dontCare. */
    for (int i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
        if (!c.texture) c.storeAction = MTLStoreActionDontCare, c.loadAction = MTLLoadActionDontCare;
        else if (!c.resolveTexture && (c.storeAction == MTLStoreActionMultisampleResolve || c.storeAction == MTLStoreActionStoreAndMultisampleResolve))
            c.storeAction = MTLStoreActionStore;
    }
    if (!d.depthAttachment.texture) d.depthAttachment.loadAction = MTLLoadActionDontCare, d.depthAttachment.storeAction = MTLStoreActionDontCare;
    if (!d.stencilAttachment.texture) d.stencilAttachment.loadAction = MTLLoadActionDontCare, d.stencilAttachment.storeAction = MTLStoreActionDontCare;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
    if (!e) [self complain:@"no render encoder"];
    return e;
}

/* One serialized call. Tags are the guest's. */
- (void)render:(id<MTLRenderCommandEncoder>)e tag:(uint32_t)tag rec:(Rec)r task:(uint32_t)task {
    switch (tag) {
    case 116: [e setRenderPipelineState:[self get:U32(0) task:task]]; break;
    case 125: case 110: case 157: { /* vertex, fragment, tile buffers: {index, count}, {ref, offset}... */
        uint32_t index = U32(0), count = U32(4);
        if (count > 31) break;
        __unsafe_unretained id<MTLBuffer> buffers[31];
        id strong[31];
        NSUInteger offsets[31];
        for (uint32_t i = 0; i < count; i++) {
            strong[i] = [self get:U32(8 + i * 12) task:task];
            buffers[i] = strong[i];
            offsets[i] = (NSUInteger)U64(12 + i * 12);
        }
        NSRange range = NSMakeRange(index, count);
        if (tag == 125) [e setVertexBuffers:buffers offsets:offsets withRange:range];
        else if (tag == 110) [e setFragmentBuffers:buffers offsets:offsets withRange:range];
        else [e setTileBuffers:buffers offsets:offsets withRange:range];
        break;
    }
    case 126: [e setVertexBufferOffset:(NSUInteger)U64(4) atIndex:U32(0)]; break;
    case 111: [e setFragmentBufferOffset:(NSUInteger)U64(4) atIndex:U32(0)]; break;
    case 114: case 161: { /* fragment, tile textures */
        uint32_t index = U32(0), count = U32(4);
        if (count > 128) break;
        __unsafe_unretained id<MTLTexture> items[128];
        id strong[128];
        for (uint32_t i = 0; i < count; i++) {
            strong[i] = [self get:U32(8 + i * 4) task:task];
            items[i] = strong[i];
        }
        if (tag == 114) [e setFragmentTextures:items withRange:NSMakeRange(index, count)];
        else [e setTileTextures:items withRange:NSMakeRange(index, count)];
        break;
    }
    case 112: { /* fragment samplers */
        uint32_t index = U32(0), count = U32(4);
        if (count > 16) break;
        __unsafe_unretained id<MTLSamplerState> items[16];
        id strong[16];
        for (uint32_t i = 0; i < count; i++) {
            strong[i] = [self get:U32(8 + i * 4) task:task];
            items[i] = strong[i];
        }
        [e setFragmentSamplerStates:items withRange:NSMakeRange(index, count)];
        break;
    }
    case 117: {
        MTLScissorRect s;
        memcpy(&s, r.p, sizeof s);
        [e setScissorRect:s];
        break;
    }
    case 130: {
        MTLViewport v;
        memcpy(&v, r.p, sizeof v);
        [e setViewport:v];
        break;
    }
    case 1: /* {primitive, start 16, count 16} */
        [e drawPrimitives:(MTLPrimitiveType)U32(0) vertexStart:U16(4) vertexCount:U16(6)];
        draws++;
        break;
    case 3: /* {start, count, instances, primitive} */
        [e drawPrimitives:(MTLPrimitiveType)U16(6) vertexStart:U16(0) vertexCount:U16(2) instanceCount:U16(4)];
        draws++;
        break;
    case 7: { /* {primitive, index type, ref, count, offset} */
        id<MTLBuffer> b = [self get:U32(4) task:task];
        if (b) [e drawIndexedPrimitives:(MTLPrimitiveType)U16(0) indexCount:U16(8) indexType:(MTLIndexType)U16(2) indexBuffer:b indexBufferOffset:U16(10)];
        draws++;
        break;
    }
    case 6: {
        id<MTLBuffer> b = [self get:U32(4) task:task];
        if (b) [e drawIndexedPrimitives:(MTLPrimitiveType)U16(0) indexCount:(NSUInteger)U64(8) indexType:(MTLIndexType)U16(2) indexBuffer:b indexBufferOffset:(NSUInteger)U64(16)];
        draws++;
        break;
    }
    case 155: {
        MTLSize s;
        memcpy(&s, r.p, sizeof s);
        [e dispatchThreadsPerTile:s];
        break;
    }
    case 162: { /* threads per tile, then a tile region */
        MTLSize s;
        MTLRegion region;
        memcpy(&s, r.p, sizeof s);
        memcpy(&region, r.p + sizeof s, sizeof region);
        SEL sel = sel_registerName("dispatchThreadsPerTile:inRegion:");
        if ([(id)e respondsToSelector:sel]) ((void (*)(id, SEL, MTLSize, MTLRegion))objc_msgSend)(e, sel, s, region);
        else [e dispatchThreadsPerTile:s];
        break;
    }
    case 164: { /* where to write the tile size: {buffer ref, offset} */
        HvObject *o = [self object:U32(0) task:task];
        uint64_t off = U64(4);
        if (o && o->mapped && off + 4 <= o->length) {
            uint16_t dims[2] = {(uint16_t)e.tileWidth, (uint16_t)e.tileHeight};
            memcpy(o->mapped + off, dims, 4);
        }
        break;
    }
    default:
        [self complain:[NSString stringWithFormat:@"render command %u (%u bytes) skipped", tag, r.size]];
        break;
    }
}

/* One exec: {task, resources, buffers}, {ref, flags} per resource, {offset, length} per buffer. */
- (void)exec:(const uint32_t *)a length:(uint32_t)len {
    uint32_t task = a[0], nres = a[1], nbuf = a[2];
    if (12 + nres * 8ull + nbuf * 16ull > len) return;
    const uint32_t *res = a + 3;
    const uint8_t *bufs = (const uint8_t *)(res + nres * 2);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = nil;
    for (uint32_t i = 0; i < nres; i++) {
        HvObject *o = [self object:res[i * 2] task:task];
        if (!o) continue;
        if (o->mapped) refresh_pages(self, task, o);
        if ((res[i * 2 + 1] & 1) && o->guest_backed) [self upload:o task:task blit:&blit commandBuffer:cb];
    }
    [blit endEncoding];
    id<MTLRenderCommandEncoder> e = nil;
    for (uint32_t b = 0; b < nbuf; b++) {
        uint64_t off, blen;
        memcpy(&off, bufs + b * 16, 8);
        memcpy(&blen, bufs + b * 16 + 8, 8);
        if (blen > (64u << 20) || blen < 8) continue;
        NSMutableData *copy = [NSMutableData dataWithLength:(NSUInteger)blen];
        if (hv_pvg_lite_task_read(lite, task, off, copy.mutableBytes, (uint32_t)blen)) { [self complain:@"command buffer unreadable"]; continue; }
        const uint8_t *p = copy.bytes, *end = p + blen;
        uint32_t seglen, info;
        memcpy(&seglen, p, 4), memcpy(&info, p + 4, 4);
        p += 8;
        if ((info & 0xff) == 5) { /* protection options, then the real header */
            p += 8;
            if (p + 8 > end) continue;
            memcpy(&seglen, p, 4), memcpy(&info, p + 4, 4);
            p += 8;
        }
        uint32_t type = info & 0xff, cont = (info >> 8) & 0xff, more = (info >> 16) & 0xff;
        if (type != 0) { [self complain:[NSString stringWithFormat:@"encoder type %u skipped", type]]; continue; }
        if (p + seglen < end) end = p + seglen;
        if (!cont && e) { [e endEncoding]; e = nil; }
        @try {
            while (p + 8 <= end) {
                uint32_t tag, size;
                memcpy(&tag, p, 4), memcpy(&size, p + 4, 4);
                if (size < 8 || p + size > end) break;
                Rec r = {p + 8, size - 8};
                if (tag == 26) {
                    if (e) [e endEncoding];
                    e = [self beginPass:r task:task commandBuffer:cb];
                } else if (e) {
                    [self render:e tag:tag rec:r task:task];
                }
                p += size;
            }
        } @catch (NSException *x) {
            [self complain:[NSString stringWithFormat:@"exception: %@", x.reason]];
            failed++;
        }
        if (!more && e) { [e endEncoding]; e = nil; }
    }
    if (e) [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error) {
        failed++;
        [self complain:[NSString stringWithFormat:@"command buffer: %@", cb.error.localizedDescription]];
    }
    if (++execs % 2000 == 0)
        fprintf(stderr, "hengevm: pvg-metal: %llu execs, %llu draws, %llu failed\n", (unsigned long long)execs, (unsigned long long)draws, (unsigned long long)failed);
}

static inline uint8_t to_byte(float v) {
    if (!(v > 0)) return 0;
    if (v >= 1) return 255;
    /* The surface is linear light. */
    float s = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)(s * 255.0f + 0.5f);
}

/* What the display should show: copied to memory the host can read, at most a few times a second. */
- (void)present:(uint64_t)mapping {
    id<MTLTexture> t = mapped_textures[@(mapping)];
    if (!t) { [self complain:[NSString stringWithFormat:@"swap to unknown surface %llu", mapping]]; return; }
    double now = now_seconds();
    if (now - last_capture < 0.2) return;
    last_capture = now;
    NSUInteger bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : 4, row = t.width * bpp, size = row * t.height;
    if (readback.length < size) readback = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1)
              toBuffer:readback destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:size];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    uint32_t w = (uint32_t)t.width, h = (uint32_t)t.height;
    uint8_t *out = frame && frame_w == w && frame_h == h ? frame : malloc((size_t)w * h * 4);
    const uint8_t *src = readback.contents;
    if (bpp == 8) {
        for (uint32_t y = 0; y < h; y++) {
            const __fp16 *in = (const __fp16 *)(src + y * row);
            uint8_t *o = out + (size_t)y * w * 4;
            for (uint32_t x = 0; x < w; x++, in += 4, o += 4) {
                o[0] = to_byte(in[2]), o[1] = to_byte(in[1]), o[2] = to_byte(in[0]), o[3] = 255;
            }
        }
    } else {
        memcpy(out, src, (size_t)w * h * 4);
    }
    if (out != frame) free(frame);
    frame = out, frame_w = w, frame_h = h;
#if !TARGET_OS_IPHONE
    if (shot) {
        char path[1100];
        snprintf(path, sizeof path, "%s-%04u.png", shot, shots++);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(out, w, h, 8, (size_t)w * 4, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
        CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
        if (dest) {
            CGImageDestinationAddImage(dest, img, NULL);
            CGImageDestinationFinalize(dest);
            CFRelease(dest);
        }
        CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
    }
#endif
}

@end

HvPvgMetal *hv_pvg_metal_create(HvPvgLite *lite) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) return NULL;
        if (![dev respondsToSelector:@selector(newRenderPipelineDescriptorWithSerializedData:deserializationContext:)]) {
            fprintf(stderr, "hengevm: pvg-metal: this Metal cannot read serialized pipelines; no drawing\n");
            return NULL;
        }
        Protocol *proto = objc_getProtocol("MTLDeserializationContext");
        if (proto) class_addProtocol([HvContext class], proto);
        HvReplay *r = [HvReplay new];
        r->lite = lite;
        r->device = dev;
        r->queue = [dev newCommandQueueWithMaxCommandBufferCount:64];
        r->worker = dispatch_queue_create("hengevm.pvg", DISPATCH_QUEUE_SERIAL);
        r->tasks = [NSMutableDictionary new];
        r->mapped_textures = [NSMutableDictionary new];
        r->complaints = [NSMutableSet new];
        r->shot = getenv("HV_PVG_SHOT");
        fprintf(stderr, "hengevm: pvg-metal: drawing on %s\n", dev.name.UTF8String);
        return (HvPvgMetal *)CFBridgingRetain(r);
    }
}

void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *lite, uint32_t ch, uint32_t *cmd, uint32_t len) {
    HvReplay *r = (__bridge HvReplay *)m;
    dispatch_async(r->worker, ^{
        @autoreleasepool {
            uint32_t op = cmd[0] & 0xffff, skip = cmd[0] >> 16 ? 8 : 0;
            const uint32_t *args = (const uint32_t *)((const uint8_t *)cmd + 12 + skip);
            uint32_t n = len > 12 + skip ? len - 12 - skip : 0;
            @try {
                if (op == 0x43 && n >= 12) {
                    [r exec:args length:n];
                } else if ((op == 0x25 || op == 0x28) && n >= 8) { /* {task, ref} or {task, tag, size, ref} */
                    uint32_t ref = op == 0x25 ? args[1] : (n >= 16 ? args[3] : 0);
                    [r->tasks[@(args[0])] removeObjectForKey:@(ref)];
                } else if (op == 0x20 && n >= 4) {
                    [r->tasks removeObjectForKey:@(args[0] >> 1)];
                } else {
                    hv_pvg_lite_command(lite, cmd, len);
                    if (op == 8 && n >= 16) [r present:(uint64_t)args[3] << 32 | args[2]];
                }
            } @catch (NSException *x) {
                [r complain:[NSString stringWithFormat:@"exception: %@", x.reason]];
            }
            hv_pvg_lite_complete(lite, ch, cmd[2]);
            free(cmd);
        }
    });
}

int hv_pvg_metal_frame(HvPvgMetal *m, const uint8_t **pixels, uint32_t *w, uint32_t *h) {
    if (!m) return -1;
    HvReplay *r = (__bridge HvReplay *)m;
    if (!r->frame) return -1;
    *pixels = r->frame, *w = r->frame_w, *h = r->frame_h;
    return 0;
}
