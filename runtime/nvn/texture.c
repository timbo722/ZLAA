/*
 * texture.c - NVNtexture, NVNtextureView, NVNsampler, NVNtexturePool, NVNsamplerPool and the
 * builders. Layout math (sizes, block-linear) is in layout.c.
 */
#include "nvn.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t next_debug_id = 1;

/* ---- texture builder ---- */

#define TB ((TextureBuilder *)gptr(ARG(0)))
static void nvnTextureBuilderSetDevice(CPU *c) { TB->device = ARG(1); }
static void nvnTextureBuilderSetDefaults(CPU *c) {
    uint64_t device = TB->device;
    *TB = (TextureBuilder){ .device = device };
    TB->d = (TexDesc){ .target = TARGET_2D, .width = 1, .height = 1, .depth = 1, .levels = 1,
                       .swizzle = { 2, 3, 4, 5 },          /* NVN_TEXTURE_SWIZZLE_R, G, B, A */
                       .block_height_log2 = ~0u };
}
static void nvnTextureBuilderSetFlags(CPU *c)  { TB->d.flags = (uint32_t)ARG(1); }
static void nvnTextureBuilderSetTarget(CPU *c) { TB->d.target = (uint32_t)ARG(1); }
static void nvnTextureBuilderSetWidth(CPU *c)  { TB->d.width = (uint32_t)ARG(1); }
static void nvnTextureBuilderSetHeight(CPU *c) { TB->d.height = (uint32_t)ARG(1); }
static void nvnTextureBuilderSetDepth(CPU *c)  { TB->d.depth = (uint32_t)ARG(1); }
static void nvnTextureBuilderSetSize1D(CPU *c) { TB->d.width = (uint32_t)ARG(1); TB->d.height = TB->d.depth = 1; }
static void nvnTextureBuilderSetSize2D(CPU *c) { TB->d.width = (uint32_t)ARG(1); TB->d.height = (uint32_t)ARG(2); TB->d.depth = 1; }
static void nvnTextureBuilderSetSize3D(CPU *c) {
    TB->d.width = (uint32_t)ARG(1); TB->d.height = (uint32_t)ARG(2); TB->d.depth = (uint32_t)ARG(3);
}
static void nvnTextureBuilderSetLevels(CPU *c)  { TB->d.levels = (uint32_t)ARG(1); }
static void nvnTextureBuilderSetFormat(CPU *c)  { TB->d.format = (uint32_t)ARG(1); }
static void nvnTextureBuilderSetSamples(CPU *c) { TB->d.samples = (uint32_t)ARG(1); }
static void nvnTextureBuilderSetSwizzle(CPU *c) {
    for (int i = 0; i < 4; i++) TB->d.swizzle[i] = (uint8_t)ARG(1 + i);
}
static void nvnTextureBuilderSetDepthStencilMode(CPU *c) { TB->d.depth_stencil_mode = (uint32_t)ARG(1); }
static void nvnTextureBuilderSetStride(CPU *c)           { TB->d.stride = ARG(1); }
/* (builder, pool, offset) */
static void nvnTextureBuilderSetStorage(CPU *c)          { TB->d.pool = ARG(1); TB->d.offset = ARG(2); }
static void nvnTextureBuilderSetPackagedTextureData(CPU *c) { TB->packaged_data = ARG(1); }
/* (builder, const NVNpackagedTextureLayout *): 8 bytes from the texture file. The low word is
 * the block height (log2 GOBs) the texture was tiled with, which can differ from the default. */
static void nvnTextureBuilderSetPackagedTextureLayout(CPU *c) {
    uint32_t bh = ARG(1) ? g_ld32(ARG(1)) : ~0u;
    TB->d.block_height_log2 = bh <= 5 ? bh : ~0u;
}
static void nvnTextureBuilderSetGLTextureName(CPU *c) { TB->gl_name = (uint32_t)ARG(1); }

static void nvnTextureBuilderGetDevice(CPU *c)     { RET(TB->device); }
static void nvnTextureBuilderGetFlags(CPU *c)      { RET(TB->d.flags); }
static void nvnTextureBuilderGetTarget(CPU *c)     { RET(TB->d.target); }
static void nvnTextureBuilderGetWidth(CPU *c)      { RET(TB->d.width); }
static void nvnTextureBuilderGetHeight(CPU *c)     { RET(TB->d.height); }
static void nvnTextureBuilderGetDepth(CPU *c)      { RET(TB->d.depth); }
static void nvnTextureBuilderGetLevels(CPU *c)     { RET(TB->d.levels); }
static void nvnTextureBuilderGetFormat(CPU *c)     { RET(TB->d.format); }
static void nvnTextureBuilderGetSamples(CPU *c)    { RET(TB->d.samples); }
static void nvnTextureBuilderGetDepthStencilMode(CPU *c) { RET(TB->d.depth_stencil_mode); }
static void nvnTextureBuilderGetStride(CPU *c)     { RET(TB->d.stride); }
static void nvnTextureBuilderGetMemoryPool(CPU *c) { RET(TB->d.pool); }
static void nvnTextureBuilderGetMemoryOffset(CPU *c) { RET(TB->d.offset); }
static void nvnTextureBuilderGetPackagedTextureData(CPU *c) { RET(TB->packaged_data); }
static void nvnTextureBuilderGetGLTextureName(CPU *c) { RET(TB->gl_name); }
/* (builder, NVNtextureSwizzle *r, *g, *b, *a) */
static void nvnTextureBuilderGetSwizzle(CPU *c) {
    for (int i = 0; i < 4; i++) g_st32(ARG(1 + i), TB->d.swizzle[i]);
}
static void nvnTextureBuilderGetStorageSize(CPU *c)      { RET(texture_storage_size(&TB->d)); }
static void nvnTextureBuilderGetStorageAlignment(CPU *c) { RET(texture_storage_alignment(&TB->d)); }

/* ---- texture ---- */

static void nvnTextureInitialize(CPU *c) {
    TextureBuilder *b = gptr(ARG(1));
    Texture *t = gptr(ARG(0));
    *t = (Texture){ .d = b->d };
    t->address = pool_address(b->d.pool, b->d.offset);
    t->storage_size = texture_storage_size(&b->d);
    t->debug_id = next_debug_id++;
    static int trace = -1;                           /* LAA_TRACE_TEXELS also lists each texture made */
    if (trace < 0) trace = getenv("LAA_TRACE_TEXELS") != NULL;
    if (trace)
        fprintf(stderr, "nvn: texture %#llx: storage %#llx %ux%ux%u format %#x target %u levels %u flags %#x\n",
                (unsigned long long)ARG(0), (unsigned long long)t->address, b->d.width, b->d.height, b->d.depth,
                b->d.format, b->d.target, b->d.levels, b->d.flags);
    RET(1);
}
#define TEX ((Texture *)gptr(ARG(0)))
static void nvnTextureGetFlags(CPU *c)        { RET(TEX->d.flags); }
static void nvnTextureGetTarget(CPU *c)       { RET(TEX->d.target); }
static void nvnTextureGetWidth(CPU *c)        { RET(TEX->d.width); }
static void nvnTextureGetHeight(CPU *c)       { RET(TEX->d.height); }
static void nvnTextureGetDepth(CPU *c)        { RET(TEX->d.depth); }
static void nvnTextureGetLevels(CPU *c)       { RET(TEX->d.levels); }
static void nvnTextureGetFormat(CPU *c)       { RET(TEX->d.format); }
static void nvnTextureGetSamples(CPU *c)      { RET(TEX->d.samples); }
static void nvnTextureGetDepthStencilMode(CPU *c) { RET(TEX->d.depth_stencil_mode); }
static void nvnTextureGetStride(CPU *c)       { RET(TEX->d.stride); }
static void nvnTextureGetTextureAddress(CPU *c) { RET(TEX->address); }
static void nvnTextureGetMemoryPool(CPU *c)   { RET(TEX->d.pool); }
static void nvnTextureGetMemoryOffset(CPU *c) { RET(TEX->d.offset); }
static void nvnTextureGetStorageSize(CPU *c)  { RET(TEX->storage_size); }
static void nvnTextureGetDebugID(CPU *c)      { RET(TEX->debug_id); }
static void nvnTextureGetSwizzle(CPU *c)      { for (int i = 0; i < 4; i++) g_st32(ARG(1 + i), TEX->d.swizzle[i]); }
/* (texture, view) -> offset of the view's first byte; views always start at 0 for us */
static void nvnTextureGetViewOffset(CPU *c)   { RET(0); }
/* (texture, int plane?, NvRmSurface *out): the driver-level description of a texture, which the
 * movie player hands to the video decoder. We have no driver-level memory handles, so only the
 * size is filled in (the struct is at most 0x60 bytes in the one caller). Movies don't decode yet. */
static void nvnTextureGetNvRmSurfaceNVX(CPU *c) {
    memset(gptr(ARG(2)), 0, 0x60);
    g_st32(ARG(2), TEX->d.width);
    g_st32(ARG(2) + 4, TEX->d.height);
}
static void nvnTextureCompare(CPU *c) {
    RET(memcmp(&((Texture *)gptr(ARG(0)))->d, &((Texture *)gptr(ARG(1)))->d, sizeof(TexDesc)) == 0);
}

/* nvnTextureWriteTexels(texture, view, const NVNcopyRegion *region, const void *data) and the
 * Strided variant (+ row stride, image stride): CPU writes into the texture's storage. */
static void write_texels(CPU *c, uint64_t row_stride, uint64_t image_stride, int to_linear) {
    Texture *t = gptr(ARG(0));
    const TextureView *v = ARG(1) ? gptr(ARG(1)) : NULL;
    const int32_t *r = gptr(ARG(2));
    TexShape s;
    tex_shape(&t->d, &s);
    uint32_t level = v && (v->set & VIEW_LEVELS) ? v->base_level : 0;
    uint32_t layer = v && (v->set & VIEW_LAYERS) ? v->min_layer : 0;
    if (t->d.flags & TEXTURE_FLAG_LINEAR) {
        fprintf(stderr, "nvn: texel copy to/from a linear texture is not implemented\n");
        return;
    }
    LevelShape l = tex_level(&s, level);
    uint32_t region[6] = { (uint32_t)r[0] / s.fmt->bw, (uint32_t)r[1] / s.fmt->bh, (uint32_t)r[2],
                           ((uint32_t)r[3] + s.fmt->bw - 1) / s.fmt->bw, ((uint32_t)r[4] + s.fmt->bh - 1) / s.fmt->bh,
                           r[5] > 0 ? (uint32_t)r[5] : 1 };
    if (!row_stride) row_stride = (uint64_t)region[3] * s.fmt->bytes;
    if (!image_stride) image_stride = row_stride * region[4];
    uint8_t *tiled = gptr(t->address + layer * tex_layer_size(&s) + l.offset);
    tex_copy_level(tiled, gptr(ARG(3)), &s, &l, region, row_stride, image_stride, to_linear);
    if (!to_linear) nvn_texels_written(ARG(0), level, layer, region);   /* the GPU's copy is now stale */
    static int trace = -1;                           /* LAA_TRACE_TEXELS: every CPU texel write */
    if (trace < 0) trace = getenv("LAA_TRACE_TEXELS") != NULL;
    if (trace)
        fprintf(stderr, "nvn: %s texels: texture %#llx storage %#llx %ux%u format %#x level %u layer %u region %d,%d %dx%d\n",
                to_linear ? "read" : "write", (unsigned long long)ARG(0), (unsigned long long)t->address, s.width, s.height,
                t->d.format, level, layer, r[0], r[1], r[3], r[4]);
}
static void nvnTextureWriteTexels(CPU *c)        { write_texels(c, 0, 0, 0); }
static void nvnTextureWriteTexelsStrided(CPU *c) { write_texels(c, ARG(4), ARG(5), 0); }
static void nvnTextureReadTexels(CPU *c)         { write_texels(c, 0, 0, 1); }
static void nvnTextureReadTexelsStrided(CPU *c)  { write_texels(c, ARG(4), ARG(5), 1); }

/* ---- texture view ---- */

#define TV ((TextureView *)gptr(ARG(0)))
static void nvnTextureViewSetDefaults(CPU *c) { *TV = (TextureView){ 0 }; }
static void nvnTextureViewSetLevels(CPU *c) {
    TV->set |= VIEW_LEVELS; TV->base_level = (uint16_t)ARG(1); TV->num_levels = (uint16_t)ARG(2);
}
static void nvnTextureViewSetLayers(CPU *c) {
    TV->set |= VIEW_LAYERS; TV->min_layer = (uint16_t)ARG(1); TV->num_layers = (uint16_t)ARG(2);
}
static void nvnTextureViewSetFormat(CPU *c) { TV->set |= VIEW_FORMAT; TV->format = (uint32_t)ARG(1); }
static void nvnTextureViewSetSwizzle(CPU *c) {
    TV->set |= VIEW_SWIZZLE;
    for (int i = 0; i < 4; i++) TV->swizzle[i] = (uint8_t)ARG(1 + i);
}
static void nvnTextureViewSetDepthStencilMode(CPU *c) { TV->set |= VIEW_DS_MODE; TV->depth_stencil_mode = (uint32_t)ARG(1); }
static void nvnTextureViewSetTarget(CPU *c)           { TV->set |= VIEW_TARGET; TV->target = (uint32_t)ARG(1); }
/* getters: (view, out...) -> whether the view sets that field */
static void nvnTextureViewGetLevels(CPU *c) {
    g_st32(ARG(1), TV->base_level); g_st32(ARG(2), TV->num_levels); RET(!!(TV->set & VIEW_LEVELS));
}
static void nvnTextureViewGetLayers(CPU *c) {
    g_st32(ARG(1), TV->min_layer); g_st32(ARG(2), TV->num_layers); RET(!!(TV->set & VIEW_LAYERS));
}
static void nvnTextureViewGetFormat(CPU *c) { g_st32(ARG(1), TV->format); RET(!!(TV->set & VIEW_FORMAT)); }
static void nvnTextureViewGetSwizzle(CPU *c) {
    for (int i = 0; i < 4; i++) g_st32(ARG(1 + i), TV->swizzle[i]);
    RET(!!(TV->set & VIEW_SWIZZLE));
}
static void nvnTextureViewGetDepthStencilMode(CPU *c) { g_st32(ARG(1), TV->depth_stencil_mode); RET(!!(TV->set & VIEW_DS_MODE)); }
static void nvnTextureViewGetTarget(CPU *c)           { g_st32(ARG(1), TV->target); RET(!!(TV->set & VIEW_TARGET)); }
static void nvnTextureViewCompare(CPU *c) { RET(memcmp(gptr(ARG(0)), gptr(ARG(1)), sizeof(TextureView)) == 0); }

/* ---- sampler ---- */

#define SB (&((SamplerBuilder *)gptr(ARG(0)))->s)
static void nvnSamplerBuilderSetDevice(CPU *c) { ((SamplerBuilder *)gptr(ARG(0)))->device = ARG(1); }
static void nvnSamplerBuilderSetDefaults(CPU *c) {
    *SB = (SamplerDesc){ .wrap = { 1, 1, 1 },                  /* REPEAT */
                         .compare_func = 4,                    /* LEQUAL */
                         .lod_max = 1000.0f, .max_anisotropy = 1.0f };
}
static void nvnSamplerBuilderSetMinMagFilter(CPU *c) { SB->min_filter = (uint8_t)ARG(1); SB->mag_filter = (uint8_t)ARG(2); }
static void nvnSamplerBuilderSetWrapMode(CPU *c) {
    for (int i = 0; i < 3; i++) SB->wrap[i] = (uint8_t)ARG(1 + i);
}
static void nvnSamplerBuilderSetLodClamp(CPU *c)     { SB->lod_min = ARGF(0); SB->lod_max = ARGF(1); }
static void nvnSamplerBuilderSetLodBias(CPU *c)      { SB->lod_bias = ARGF(0); }
static void nvnSamplerBuilderSetLodSnap(CPU *c)      { SB->lod_snap = ARGF(0); }
static void nvnSamplerBuilderSetCompare(CPU *c)      { SB->compare_mode = (uint8_t)ARG(1); SB->compare_func = (uint8_t)ARG(2); }
static void nvnSamplerBuilderSetBorderColor(CPU *c)  { memcpy(SB->border, gptr(ARG(1)), 16); }
static void nvnSamplerBuilderSetMaxAnisotropy(CPU *c) { SB->max_anisotropy = ARGF(0); }
static void nvnSamplerBuilderSetReductionFilter(CPU *c) { SB->reduction = (uint8_t)ARG(1); }
static void nvnSamplerBuilderGetDevice(CPU *c)       { RET(((SamplerBuilder *)gptr(ARG(0)))->device); }

/* getters shared by builders and samplers (the SamplerDesc is in both) */
static void get_min_mag(CPU *c, SamplerDesc *s) { g_st32(ARG(1), s->min_filter); g_st32(ARG(2), s->mag_filter); }
static void get_wrap(CPU *c, SamplerDesc *s)    { for (int i = 0; i < 3; i++) g_st32(ARG(1 + i), s->wrap[i]); }
static void get_lod_clamp(CPU *c, SamplerDesc *s) {
    memcpy(gptr(ARG(1)), &s->lod_min, 4); memcpy(gptr(ARG(2)), &s->lod_max, 4);
}
static void get_compare(CPU *c, SamplerDesc *s) { g_st32(ARG(1), s->compare_mode); g_st32(ARG(2), s->compare_func); }
static void get_border(CPU *c, SamplerDesc *s)  { memcpy(gptr(ARG(1)), s->border, 16); }
#define SMP (&((Sampler *)gptr(ARG(0)))->s)
static void nvnSamplerBuilderGetMinMagFilter(CPU *c)    { get_min_mag(c, SB); }
static void nvnSamplerBuilderGetWrapMode(CPU *c)        { get_wrap(c, SB); }
static void nvnSamplerBuilderGetLodClamp(CPU *c)        { get_lod_clamp(c, SB); }
static void nvnSamplerBuilderGetLodBias(CPU *c)         { ret_float(c, SB->lod_bias); }
static void nvnSamplerBuilderGetLodSnap(CPU *c)         { ret_float(c, SB->lod_snap); }
static void nvnSamplerBuilderGetCompare(CPU *c)         { get_compare(c, SB); }
static void nvnSamplerBuilderGetBorderColor(CPU *c)     { get_border(c, SB); }
static void nvnSamplerBuilderGetMaxAnisotropy(CPU *c)   { ret_float(c, SB->max_anisotropy); }
static void nvnSamplerBuilderGetReductionFilter(CPU *c) { RET(SB->reduction); }

static void nvnSamplerInitialize(CPU *c) {
    *(Sampler *)gptr(ARG(0)) = (Sampler){ .s = ((SamplerBuilder *)gptr(ARG(1)))->s, .debug_id = next_debug_id++ };
    RET(1);
}
static void nvnSamplerGetMinMagFilter(CPU *c)    { get_min_mag(c, SMP); }
static void nvnSamplerGetWrapMode(CPU *c)        { get_wrap(c, SMP); }
static void nvnSamplerGetLodClamp(CPU *c)        { get_lod_clamp(c, SMP); }
static void nvnSamplerGetLodBias(CPU *c)         { ret_float(c, SMP->lod_bias); }
static void nvnSamplerGetCompare(CPU *c)         { get_compare(c, SMP); }
static void nvnSamplerGetBorderColor(CPU *c)     { get_border(c, SMP); }
static void nvnSamplerGetMaxAnisotropy(CPU *c)   { ret_float(c, SMP->max_anisotropy); }
static void nvnSamplerGetReductionFilter(CPU *c) { RET(SMP->reduction); }
static void nvnSamplerGetDebugID(CPU *c)         { RET(((Sampler *)gptr(ARG(0)))->debug_id); }
static void nvnSamplerCompare(CPU *c) { RET(memcmp(SMP, &((Sampler *)gptr(ARG(1)))->s, sizeof(SamplerDesc)) == 0); }

/* ---- texture and sampler pools: (pool object, memory pool, offset, count) ---- */

static void pool_init(CPU *c, size_t entry_size) {
    DescriptorPool *p = gptr(ARG(0));
    *p = (DescriptorPool){ calloc(ARG(3), entry_size), ARG(1), ARG(2), (uint32_t)ARG(3) };
    RET(p->entries != NULL);
}
static void pool_finalize(CPU *c) {
    DescriptorPool *p = gptr(ARG(0));
    free(p->entries);
    p->entries = NULL;
}
#define DP ((DescriptorPool *)gptr(ARG(0)))
static void nvnTexturePoolInitialize(CPU *c)  { pool_init(c, sizeof(TexturePoolEntry)); }
static void nvnSamplerPoolInitialize(CPU *c)  { pool_init(c, sizeof(SamplerDesc)); }
static void nvnPoolGetMemoryPool(CPU *c)      { RET(DP->pool); }
static void nvnPoolGetMemoryOffset(CPU *c)    { RET(DP->offset); }
static void nvnPoolGetSize(CPU *c)            { RET(DP->count); }

/* (pool, int id, const NVNtexture *, const NVNtextureView *) */
static void nvnTexturePoolRegisterTexture(CPU *c) {
    uint32_t id = (uint32_t)ARG(1);
    if (id >= DP->count) return;
    TexturePoolEntry *e = &((TexturePoolEntry *)DP->entries)[id];
    e->texture = ARG(2);
    e->view = ARG(3) ? *(TextureView *)gptr(ARG(3)) : (TextureView){ 0 };
}
/* (pool, int id, const NVNsampler *) and (pool, int id, const NVNsamplerBuilder *) */
static void nvnSamplerPoolRegisterSampler(CPU *c) {
    uint32_t id = (uint32_t)ARG(1);
    if (id < DP->count) ((SamplerDesc *)DP->entries)[id] = ((Sampler *)gptr(ARG(2)))->s;
}
static void nvnSamplerPoolRegisterSamplerBuilder(CPU *c) {
    uint32_t id = (uint32_t)ARG(1);
    if (id < DP->count) ((SamplerDesc *)DP->entries)[id] = ((SamplerBuilder *)gptr(ARG(2)))->s;
}

const NvnEntry nvn_texture_functions[] = {
    NVN_ENTRY(nvnTextureBuilderSetDevice), NVN_ENTRY(nvnTextureBuilderSetDefaults), NVN_ENTRY(nvnTextureBuilderSetFlags),
    NVN_ENTRY(nvnTextureBuilderSetTarget), NVN_ENTRY(nvnTextureBuilderSetWidth), NVN_ENTRY(nvnTextureBuilderSetHeight),
    NVN_ENTRY(nvnTextureBuilderSetDepth), NVN_ENTRY(nvnTextureBuilderSetSize1D), NVN_ENTRY(nvnTextureBuilderSetSize2D),
    NVN_ENTRY(nvnTextureBuilderSetSize3D), NVN_ENTRY(nvnTextureBuilderSetLevels), NVN_ENTRY(nvnTextureBuilderSetFormat),
    NVN_ENTRY(nvnTextureBuilderSetSamples), NVN_ENTRY(nvnTextureBuilderSetSwizzle),
    NVN_ENTRY(nvnTextureBuilderSetDepthStencilMode), NVN_ENTRY(nvnTextureBuilderSetStride),
    NVN_ENTRY(nvnTextureBuilderSetStorage), NVN_ENTRY(nvnTextureBuilderSetPackagedTextureData),
    NVN_ENTRY(nvnTextureBuilderSetPackagedTextureLayout), NVN_ENTRY(nvnTextureBuilderSetGLTextureName),
    NVN_ENTRY(nvnTextureBuilderGetDevice), NVN_ENTRY(nvnTextureBuilderGetFlags), NVN_ENTRY(nvnTextureBuilderGetTarget),
    NVN_ENTRY(nvnTextureBuilderGetWidth), NVN_ENTRY(nvnTextureBuilderGetHeight), NVN_ENTRY(nvnTextureBuilderGetDepth),
    NVN_ENTRY(nvnTextureBuilderGetLevels), NVN_ENTRY(nvnTextureBuilderGetFormat), NVN_ENTRY(nvnTextureBuilderGetSamples),
    NVN_ENTRY(nvnTextureBuilderGetDepthStencilMode), NVN_ENTRY(nvnTextureBuilderGetStride),
    NVN_ENTRY(nvnTextureBuilderGetMemoryPool), NVN_ENTRY(nvnTextureBuilderGetMemoryOffset),
    NVN_ENTRY(nvnTextureBuilderGetPackagedTextureData), NVN_ENTRY(nvnTextureBuilderGetGLTextureName),
    NVN_ENTRY(nvnTextureBuilderGetSwizzle), NVN_ENTRY(nvnTextureBuilderGetStorageSize),
    NVN_ENTRY(nvnTextureBuilderGetStorageAlignment),
    { "nvnTextureBuilderGetPackagedTextureLayout", nvn_zero }, { "nvnTextureBuilderGetSparseTileLayout", nvn_zero },
    { "nvnTextureBuilderGetZCullStorageSize", nvn_zero }, { "nvnTextureBuilderGetStorageClass", nvn_zero },
    { "nvnTextureBuilderGetRawStorageClass", nvn_zero },

    NVN_ENTRY(nvnTextureInitialize), { "nvnTextureFinalize", nvn_nothing }, { "nvnTextureSetDebugLabel", nvn_nothing },
    NVN_ENTRY(nvnTextureGetFlags), NVN_ENTRY(nvnTextureGetTarget), NVN_ENTRY(nvnTextureGetWidth),
    NVN_ENTRY(nvnTextureGetHeight), NVN_ENTRY(nvnTextureGetDepth), NVN_ENTRY(nvnTextureGetLevels),
    NVN_ENTRY(nvnTextureGetFormat), NVN_ENTRY(nvnTextureGetSamples), NVN_ENTRY(nvnTextureGetDepthStencilMode),
    NVN_ENTRY(nvnTextureGetStride), NVN_ENTRY(nvnTextureGetTextureAddress), NVN_ENTRY(nvnTextureGetMemoryPool),
    NVN_ENTRY(nvnTextureGetMemoryOffset), NVN_ENTRY(nvnTextureGetStorageSize), NVN_ENTRY(nvnTextureGetDebugID),
    NVN_ENTRY(nvnTextureGetSwizzle), NVN_ENTRY(nvnTextureGetViewOffset), NVN_ENTRY(nvnTextureCompare), NVN_ENTRY(nvnTextureGetNvRmSurfaceNVX),
    { "nvnTextureGetStorageClass", nvn_zero }, { "nvnTextureGetRawStorageClass", nvn_zero },
    { "nvnTextureGetSparseTileLayout", nvn_zero }, { "nvnTextureGetZCullStorageSize", nvn_zero },
    { "nvnTextureFlushTexels", nvn_nothing }, { "nvnTextureInvalidateTexels", nvn_nothing },
    NVN_ENTRY(nvnTextureWriteTexels), NVN_ENTRY(nvnTextureWriteTexelsStrided),
    NVN_ENTRY(nvnTextureReadTexels), NVN_ENTRY(nvnTextureReadTexelsStrided),

    NVN_ENTRY(nvnTextureViewSetDefaults), NVN_ENTRY(nvnTextureViewSetLevels), NVN_ENTRY(nvnTextureViewSetLayers),
    NVN_ENTRY(nvnTextureViewSetFormat), NVN_ENTRY(nvnTextureViewSetSwizzle), NVN_ENTRY(nvnTextureViewSetDepthStencilMode),
    NVN_ENTRY(nvnTextureViewSetTarget), NVN_ENTRY(nvnTextureViewGetLevels), NVN_ENTRY(nvnTextureViewGetLayers),
    NVN_ENTRY(nvnTextureViewGetFormat), NVN_ENTRY(nvnTextureViewGetSwizzle), NVN_ENTRY(nvnTextureViewGetDepthStencilMode),
    NVN_ENTRY(nvnTextureViewGetTarget), NVN_ENTRY(nvnTextureViewCompare),

    NVN_ENTRY(nvnSamplerBuilderSetDevice), NVN_ENTRY(nvnSamplerBuilderSetDefaults),
    NVN_ENTRY(nvnSamplerBuilderSetMinMagFilter), NVN_ENTRY(nvnSamplerBuilderSetWrapMode),
    NVN_ENTRY(nvnSamplerBuilderSetLodClamp), NVN_ENTRY(nvnSamplerBuilderSetLodBias), NVN_ENTRY(nvnSamplerBuilderSetLodSnap),
    NVN_ENTRY(nvnSamplerBuilderSetCompare), NVN_ENTRY(nvnSamplerBuilderSetBorderColor),
    { "nvnSamplerBuilderSetBorderColori", nvnSamplerBuilderSetBorderColor },
    { "nvnSamplerBuilderSetBorderColorui", nvnSamplerBuilderSetBorderColor },
    NVN_ENTRY(nvnSamplerBuilderSetMaxAnisotropy), NVN_ENTRY(nvnSamplerBuilderSetReductionFilter),
    NVN_ENTRY(nvnSamplerBuilderGetDevice), NVN_ENTRY(nvnSamplerBuilderGetMinMagFilter),
    NVN_ENTRY(nvnSamplerBuilderGetWrapMode), NVN_ENTRY(nvnSamplerBuilderGetLodClamp),
    NVN_ENTRY(nvnSamplerBuilderGetLodBias), NVN_ENTRY(nvnSamplerBuilderGetLodSnap), NVN_ENTRY(nvnSamplerBuilderGetCompare),
    NVN_ENTRY(nvnSamplerBuilderGetBorderColor),
    { "nvnSamplerBuilderGetBorderColori", nvnSamplerBuilderGetBorderColor },
    { "nvnSamplerBuilderGetBorderColorui", nvnSamplerBuilderGetBorderColor },
    NVN_ENTRY(nvnSamplerBuilderGetMaxAnisotropy), NVN_ENTRY(nvnSamplerBuilderGetReductionFilter),

    NVN_ENTRY(nvnSamplerInitialize), { "nvnSamplerFinalize", nvn_nothing }, { "nvnSamplerSetDebugLabel", nvn_nothing },
    NVN_ENTRY(nvnSamplerGetMinMagFilter), NVN_ENTRY(nvnSamplerGetWrapMode), NVN_ENTRY(nvnSamplerGetLodClamp),
    NVN_ENTRY(nvnSamplerGetLodBias), NVN_ENTRY(nvnSamplerGetCompare), NVN_ENTRY(nvnSamplerGetBorderColor),
    { "nvnSamplerGetBorderColori", nvnSamplerGetBorderColor }, { "nvnSamplerGetBorderColorui", nvnSamplerGetBorderColor },
    NVN_ENTRY(nvnSamplerGetMaxAnisotropy), NVN_ENTRY(nvnSamplerGetReductionFilter), NVN_ENTRY(nvnSamplerGetDebugID),
    NVN_ENTRY(nvnSamplerCompare),

    NVN_ENTRY(nvnTexturePoolInitialize), { "nvnTexturePoolFinalize", pool_finalize },
    { "nvnTexturePoolSetDebugLabel", nvn_nothing },
    { "nvnTexturePoolGetMemoryPool", nvnPoolGetMemoryPool }, { "nvnTexturePoolGetMemoryOffset", nvnPoolGetMemoryOffset },
    { "nvnTexturePoolGetSize", nvnPoolGetSize },
    NVN_ENTRY(nvnTexturePoolRegisterTexture), { "nvnTexturePoolRegisterImage", nvnTexturePoolRegisterTexture },
    NVN_ENTRY(nvnSamplerPoolInitialize), { "nvnSamplerPoolFinalize", pool_finalize },
    { "nvnSamplerPoolSetDebugLabel", nvn_nothing },
    { "nvnSamplerPoolGetMemoryPool", nvnPoolGetMemoryPool }, { "nvnSamplerPoolGetMemoryOffset", nvnPoolGetMemoryOffset },
    { "nvnSamplerPoolGetSize", nvnPoolGetSize },
    NVN_ENTRY(nvnSamplerPoolRegisterSampler), NVN_ENTRY(nvnSamplerPoolRegisterSamplerBuilder),
    { NULL, NULL },
};
