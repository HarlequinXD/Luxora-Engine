/* Luxora Vulkan renderer.
 *
 * Vulkan 1.3 with dynamic rendering. Per frame (two frames in flight):
 *   1. CPU culling + LOD selection + batching into instanced draws (one draw per mesh/LOD/material)
 *   2. Live GI levels upload (L1 SH, three RGBA16F 3D textures each, walls, distance fields) when they change
 *   3. cascaded shadow maps (depth array, one layer per cascade)
 *   4. reflection probes: time-sliced cubemap capture of the scene + GGX pre-filtered mips
 *   5. HDR forward PBR pass: sky, opaque geometry (+ normal/roughness/reflection-weight targets for SSR)
 *   6. screen-space reflections: half-res trace, then a composite that replaces probe reflections where
 *      the ray hit something on screen; editor grid and selection outline go on top
 *   7. screen-space ambient occlusion (half res, taken off the ambient light only), bloom (a chain of half-size
 *      levels down and back up), sun shafts (half res, marched towards the sun on screen)
 *   8. the final image: shafts + bloom, exposure, lens, ACES, colour grading, grain - into an LDR target (editor
 *      viewport) or straight into the swapchain
 *   9. UI pass on the swapchain (editor)
 */
#include "../lx_internal.h"
#include "vk.h"
#include "shaders_spv.h"

#define FRAMES 2
#define MAX_CASCADES 4
#define MAX_LIGHTS 16
#define LOCAL_SHADOW_LIGHTS 8    /* point / spot lights with shadows at once (the nearest to the camera) */
#define ATLAS_TILES 8            /* the local shadow atlas is ATLAS_TILES x ATLAS_TILES tiles; a point light uses 6 */
#define POINT_SHADOW_TAN 1.05f   /* cube faces are a touch wider than 90 degrees so filtering never runs off a face */
#define MAX_VIEWS_PER_FRAME 24   /* camera views + reflection-probe faces */
#define PROBE_SLOTS 5            /* 0: automatic environment probe, 1..4: Reflection Probe objects */
#define PROBE_MAX_MIPS 7
#define MAX_UI_TEXTURES 1024
#define BLOOM_LEVELS 6           /* the first at half resolution, each next half again */

/* ------------------------------------------------------------------ types */
typedef struct GpuBuffer { VkBuffer buf; VkDeviceMemory mem; VkDeviceSize size; void *mapped; } GpuBuffer;
typedef struct GpuImage {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;      /* full view (array/3D as appropriate) */
    VkImageView layer_views[MAX_CASCADES + 1];   /* cascades, then the precipitation map */
    VkFormat fmt;
    uint32_t w, h, d, layers, mips;
} GpuImage;

typedef struct MeshGpu {
    GpuBuffer vb[LX_MAX_LODS], ib[LX_MAX_LODS];
    bool dynamic;                  /* skinned: vertices streamed through per-frame host buffers */
    GpuBuffer dvb[FRAMES];
    uint32_t dver[FRAMES];
} MeshGpu;
/* A texture can be colour (sRGB: albedo) or data (linear: normal, roughness, height...). The same
 * image used both ways gets one GPU copy per interpretation, never the wrong one. */
typedef struct TexGpu { GpuImage img[2]; bool has[2]; } TexGpu;   /* [0] linear, [1] sRGB */

struct LxRenderTarget {
    int w, h;              /* output size */
    int iw, ih;            /* internal (render-scaled) size */
    GpuImage hdr, depth, ldr;
    GpuImage gnormal, gspec, ssr;   /* SSR inputs (full res) and trace result (half res) */
    int sw, sh;                     /* half-res size */
    VkDescriptorSet hdr_set, ssr_trace_set, ssr_comp_set;
    uint32_t ui_tex;
    /* after the main pass: ambient light (what AO darkens), occlusion and sun shafts (half res), bloom levels */
    GpuImage gamb, ao, rays, bloom[BLOOM_LEVELS];
    int bw[BLOOM_LEVELS], bh[BLOOM_LEVELS], bloom_n;
    VkDescriptorSet ao_set, ao_apply_set, rays_set, tone_set, bloom_down_set[BLOOM_LEVELS], bloom_up_set[BLOOM_LEVELS];
};

typedef struct UiTex { bool used, owned; GpuImage img; VkDescriptorSet set; int mode; } UiTex;

typedef struct FrameData {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
    GpuBuffer ubo;
    uint32_t ubo_used;
    GpuBuffer inst;
    VkDeviceSize inst_used;
    bool inst_overflow;
    GpuBuffer ui_vb, ui_ib;
    GpuBuffer gi_staging;                /* GI levels on their way to the textures */
    size_t gi_staging_size;
    VkDescriptorSet scene_set;
} FrameData;

#define MAT_TEXTURES 6
typedef struct MatSet { const LxTexture *tex[MAT_TEXTURES]; VkDescriptorSet set; } MatSet;

typedef struct SceneUBO {
    LxMat4 view, proj, view_proj, inv_view_proj;
    float camera_pos[4], sun_dir[4], sun_color[4], sky_zenith[4], sky_horizon[4], ground_color[4], fog_color[4];
    LxMat4 cascade_vp[MAX_CASCADES];
    float cascade_splits[4];
    float gi_origin[4], gi_cell[4], gi_res[4];
    int32_t counts[4];
    float probe_pos[PROBE_SLOTS][4], probe_min[PROBE_SLOTS][4], probe_max[PROBE_SLOTS][4];
    float refl[4];
    float lights[MAX_LIGHTS][12];
    float light_shadow[MAX_LIGHTS][8];   /* (first tile or -1, near, far, tan half-angle), (up xyz, tile resolution) */
    float gi_casc[LX_GI_CASCADES][4];    /* camera GI grids: first cell's corner, cell size (0 = off) */
    int32_t gi_casc_res[4];
    float bvol_rows[LX_MAX_BAKE_VOLUMES][3][4];   /* Baked Lighting Volumes: world -> 0..1 box (3 rows of an affine map) */
    float bvol_info[LX_MAX_BAKE_VOLUMES][4];      /* z offset, nx, ny, nz (texels) */
    float bvol_params[4];                         /* volumes in use, one layer's depth, the sun's shadowmask channel (-1 none) */
    /* Day-night sky and weather (see scene_common.glsl) */
    float sky_params[4], sky_sun[4], sky_moon[4], sky_misc[4], sky_pole[4];
    float cloud_a[4], cloud_b[4], cloud_c[4], weather_a[4], weather_b[4], bolt[4], bolt_b[4];
    LxMat4 precip_vp;
} SceneUBO;
_Static_assert(sizeof(SceneUBO) <= 4096, "SceneUBO must fit its 4 KB slot");

typedef struct Batch { LxMesh *mesh; int lod; const LxMaterial *mat; uint32_t first, count; float key; } Batch;
typedef struct DrawList {
    Batch *batches; int nb, cb;
    float *inst; int *item_batch; int ni, ci;
} DrawList;

typedef struct ProbeSlot {
    GpuImage cube;                                  /* cube view over every mip (what the PBR shader samples) */
    VkImageView face_view[PROBE_MAX_MIPS][6];       /* render targets */
    VkImageView src_view[PROBE_MAX_MIPS];           /* mips [0, m): what pre-filtering mip m reads */
    VkDescriptorSet src_set[PROBE_MAX_MIPS];
    LxEntity entity;                                /* 0 = automatic environment probe */
    bool used, valid, cycling, realtime;
    int next_face;
    uint64_t captured_hash, cycle_hash, want_hash;
    LxVec3 pos, bmin, bmax, cap_pos;
    float intensity, blend;
    bool box;
} ProbeSlot;

/* ------------------------------------------------------------------ state */
/* per-instance data: model matrix, tint, lightmap tile (size, x, y, atlas or -1) */
#define INST_FLOATS 24
#define INST_BYTES (INST_FLOATS * 4)
static const float NO_LIGHTMAP[4] = {0, 0, 0, -1};

static struct {
    bool ready;
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    uint32_t surface_gen;
    VkPhysicalDevice gpu;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkDevice device;
    uint32_t queue_family;
    bool queue_compute;
    VkQueue queue;
    VkCommandPool pool;
    /* swapchain */
    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D swap_extent;
    VkImage swap_images[8];
    VkImageView swap_views[8];
    VkSemaphore swap_done[8];
    uint32_t swap_count, image_index;
    bool swap_transfer_src;
    bool vsync, swap_dirty;
    bool swap_written;
    bool frame_active;
    /* frames */
    FrameData frames[FRAMES];
    uint32_t frame;
    VkDeviceSize inst_capacity;
    /* formats */
    VkFormat depth_format, hdr_format;
    /* descriptors */
    VkDescriptorPool desc_pool;
    VkDescriptorSetLayout scene_layout, material_layout, tex_layout, probe_layout, ssr_layout;
    VkPipelineLayout pbr_layout, shadow_layout, scene_only_layout, outline_layout, tonemap_layout, ui_layout, ssr_pipe_layout,
        filter_layout;
    VkSampler linear_repeat, linear_clamp, shadow_sampler, point_clamp, cube_sampler;
    VkFormat gnormal_format, gspec_format, gamb_format;
    VkDescriptorSetLayout post_set_layout;   /* four input pictures */
    VkPipelineLayout post_layout;            /* scene + inputs, 64 bytes of numbers */
    VkPipeline ao, ao_apply, bloom_down, bloom_up, godrays;
    /* pipelines */
    VkPipeline pbr, shadow, sky, grid, outline, tonemap_ldr, tonemap_swap, ui, precip;
    VkPipeline pbr_mrt, sky_mrt, pbr_probe, probe_filter, ssr_trace, ssr_composite;
    /* material variants: [normal, MRT, probe face][solid, cutout][one-sided, double-sided] ([x][0][0] are the three above),
     * see-through ones [one-sided, double-sided], and cut-out / dithered shadows */
    VkPipeline pbr_var[3][2][2], pbr_blend[2], shadow_cut;
    VkPipelineLayout shadow_cut_layout;
    /* reflection probes */
    ProbeSlot probes[PROBE_SLOTS];
    int probe_res, probe_mips;
    GpuImage probe_depth, dummy_cube;
    VkDescriptorSet probe_set, probe_dummy_set;
    DrawList probe_list;
    uint64_t frame_serial, probe_frame;
    int probe_rr;
    /* images */
    GpuImage shadow_map;
    int shadow_res, shadow_layers;
    GpuImage gc[3], gc_vis[2];
    GpuImage gsdf;                       /* the GI camera levels' distance fields (2 res per side, stacked along z): contact shadows */           /* camera grids for shading, stacked along z: SH r g b, walls */
    int gc_res;
    float gc_ubo[LX_GI_CASCADES][4];     /* what the shader is told: origin, cell (0 = off) */
    uint64_t gi_frame;                   /* GI runs once per frame, from the first view's camera */
    GpuImage gi_vis[2];                  /* GI cell walls for shading: (+x -x +y -y), (+z -z) as distance / 1.5 cells */
    uint32_t gi_up_sh[3], gi_up_vis[3], gi_up_sdf[2];  /* the GI versions in the textures: 0, 1 camera levels, 2 the far level */
    GpuImage local_shadow;               /* atlas of point (6 cube faces) and spot (1 tile) light shadow maps */
    int local_shadow_res;
    DrawList local_lists[LOCAL_SHADOW_LIGHTS];
    GpuImage gi[3];
    int gi_res[3];
    GpuImage white, flat_normal, dummy_array;
    struct TerrainSet { const LxMaterial *mat; uint32_t version; GpuImage a, b; VkDescriptorSet set; } *terrain_sets;
    int terrain_set_count, terrain_set_cap;
    UiTex ui_tex[MAX_UI_TEXTURES];
    MatSet *mat_sets; int mat_set_count, mat_set_cap;
    LxRenderTarget *backbuffer;
    /* settings & stats */
    LxGraphicsSettings settings;
    LxRenderStats stats;
    DrawList main_list, shadow_lists[MAX_CASCADES], precip_list, trans_list;
    char screenshot_path[LX_PATH_MAX];
    int validation_errors;
    double start_time;
    /* baked lighting: the lightmaps (2 layers per atlas: irradiance, direction) and the GPU baker */
    GpuImage lightmap;
    int lm_count;                        /* atlases in it (0: the 1-texel placeholder) */
    char lm_ref[LX_REF_MAX];
    uint32_t lm_epoch;
    VkDescriptorSetLayout lm_comp_layout;
    VkPipelineLayout lm_pl;
    VkPipeline lm_pipe;
    VkDescriptorSet lm_set;
    GpuBuffer lm_buf[8];                 /* BVH nodes, triangles, materials, lights, texels, sums, voxels, voxel sums */
    bool lm_baking;
    bool preview;                        /* drawing an asset thumbnail: the scene-wide work belongs to the real views */
    /* Baked Lighting Volumes: one 3D texture, volumes stacked along z, then that block again per layer
     * (SH red, green, blue, shadowmask) */
    GpuImage bvol;
    int bvol_count, bvol_depth;          /* volumes loaded; their summed depth (one layer) */
    int bvol_info[LX_MAX_BAKE_VOLUMES][4];   /* z offset, nx, ny, nz */
} R;

/* ------------------------------------------------------------------ helpers */
#define VK_CHECK(x)                                                                         \
    do {                                                                                    \
        VkResult _r = (x);                                                                  \
        if (_r != VK_SUCCESS) { LX_ERROR("%s failed: %s (%s:%d)", #x, lx_vk_result(_r), __FILE__, __LINE__); } \
    } while (0)

static uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < R.mem_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (R.mem_props.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static bool buffer_create(GpuBuffer *b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    memset(b, 0, sizeof *b);
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size ? size : 16;
    ci.usage = usage;
    if (vkCreateBuffer(R.device, &ci, NULL, &b->buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(R.device, b->buf, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                              : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = find_memory(req.memoryTypeBits, 0);
    if (vkAllocateMemory(R.device, &ai, NULL, &b->mem) != VK_SUCCESS) { vkDestroyBuffer(R.device, b->buf, NULL); b->buf = VK_NULL_HANDLE; return false; }
    vkBindBufferMemory(R.device, b->buf, b->mem, 0);
    b->size = ci.size;
    if (host) vkMapMemory(R.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->mapped);
    return true;
}

static void buffer_destroy(GpuBuffer *b) {
    if (!b->buf) return;
    if (b->mapped) vkUnmapMemory(R.device, b->mem);
    vkDestroyBuffer(R.device, b->buf, NULL);
    vkFreeMemory(R.device, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

static VkCommandBuffer one_shot_begin(void) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = R.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(R.device, &ai, &cmd);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

static void one_shot_end(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(R.device, &fci, NULL, &fence);
    vkQueueSubmit(R.queue, 1, &si, fence);
    vkWaitForFences(R.device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(R.device, fence, NULL);
    vkFreeCommandBuffers(R.device, R.pool, 1, &cmd);
}

static void image_barrier_layers(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                                 VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                                 VkAccessFlags dst_access, uint32_t base_mip, uint32_t mips, uint32_t base_layer, uint32_t layers) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.baseMipLevel = base_mip;
    b.subresourceRange.levelCount = mips;
    b.subresourceRange.baseArrayLayer = base_layer;
    b.subresourceRange.layerCount = layers;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                          VkAccessFlags dst_access, uint32_t base_mip, uint32_t mips, uint32_t layers) {
    image_barrier_layers(cmd, img, aspect, from, to, src_stage, src_access, dst_stage, dst_access, base_mip, mips, 0, layers);
}

typedef struct ImageDesc {
    uint32_t w, h, d, layers, mips;
    VkFormat fmt;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
    VkImageViewType view_type;
    bool layer_views;
    VkImageCreateFlags flags;
} ImageDesc;

static bool image_create(GpuImage *im, const ImageDesc *d) {
    memset(im, 0, sizeof *im);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = d->flags;
    ci.imageType = d->d > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = d->fmt;
    ci.extent.width = d->w;
    ci.extent.height = d->h;
    ci.extent.depth = d->d ? d->d : 1;
    ci.mipLevels = d->mips ? d->mips : 1;
    ci.arrayLayers = d->layers ? d->layers : 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = d->usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(R.device, &ci, NULL, &im->img) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(R.device, im->img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = find_memory(req.memoryTypeBits, 0);
    if (vkAllocateMemory(R.device, &ai, NULL, &im->mem) != VK_SUCCESS) return false;
    vkBindImageMemory(R.device, im->img, im->mem, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im->img;
    vi.viewType = d->view_type;
    vi.format = d->fmt;
    vi.subresourceRange.aspectMask = d->aspect;
    vi.subresourceRange.levelCount = ci.mipLevels;
    vi.subresourceRange.layerCount = ci.arrayLayers;
    vkCreateImageView(R.device, &vi, NULL, &im->view);
    if (d->layer_views) {
        for (uint32_t l = 0; l < ci.arrayLayers && l < MAX_CASCADES + 1; l++) {
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.subresourceRange.baseArrayLayer = l;
            vi.subresourceRange.layerCount = 1;
            vkCreateImageView(R.device, &vi, NULL, &im->layer_views[l]);
        }
    }
    im->fmt = d->fmt;
    im->w = d->w; im->h = d->h; im->d = ci.extent.depth; im->layers = ci.arrayLayers; im->mips = ci.mipLevels;
    return true;
}

static void image_destroy(GpuImage *im) {
    if (!im->img) return;
    for (int l = 0; l < MAX_CASCADES + 1; l++)
        if (im->layer_views[l]) vkDestroyImageView(R.device, im->layer_views[l], NULL);
    vkDestroyImageView(R.device, im->view, NULL);
    vkDestroyImage(R.device, im->img, NULL);
    vkFreeMemory(R.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

/* upload pixels into a 2D image (and optionally generate mips), leaving it SHADER_READ_ONLY */
/* every layer of the image (pixels holds them one after another), mips made by blitting */
static void image_upload_2d(GpuImage *im, const void *pixels, size_t bytes, bool gen_mips) {
    uint32_t L = im->layers ? im->layers : 1;
    GpuBuffer staging;
    buffer_create(&staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(staging.mapped, pixels, bytes);
    VkCommandBuffer cmd = one_shot_begin();
    image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, im->mips, L);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof region);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = L;
    region.imageExtent.width = im->w;
    region.imageExtent.height = im->h;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, staging.buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    if (gen_mips && im->mips > 1) {
        int32_t mw = (int32_t)im->w, mh = (int32_t)im->h;
        for (uint32_t m = 1; m < im->mips; m++) {
            image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, m - 1, 1, L);
            VkImageBlit blit;
            memset(&blit, 0, sizeof blit);
            blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = m - 1;
            blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = L;
            blit.dstSubresource.mipLevel = m;
            blit.srcOffsets[1].x = mw; blit.srcOffsets[1].y = mh; blit.srcOffsets[1].z = 1;
            mw = mw > 1 ? mw / 2 : 1;
            mh = mh > 1 ? mh / 2 : 1;
            blit.dstOffsets[1].x = mw; blit.dstOffsets[1].y = mh; blit.dstOffsets[1].z = 1;
            vkCmdBlitImage(cmd, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, m - 1, 1, L);
        }
        image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, im->mips - 1, 1, L);
    } else {
        image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, im->mips, L);
    }
    one_shot_end(cmd);
    buffer_destroy(&staging);
}

static bool format_supports(VkFormat f, VkFormatFeatureFlags feats) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(R.gpu, f, &p);
    return (p.optimalTilingFeatures & feats) == feats;
}

static VkShaderModule shader_module(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(R.device, &ci, NULL, &m));
    return m;
}

/* ------------------------------------------------------------------ pipelines */
enum { VTX_NONE, VTX_MESH, VTX_MESH_POS_ONLY, VTX_MESH_POS_NORMAL, VTX_UI };

typedef struct PipeDesc {
    const uint32_t *vs; size_t vs_size;
    const uint32_t *fs; size_t fs_size;
    VkPipelineLayout layout;
    int vertex;
    VkFormat color, depth;
    bool depth_test, depth_write, blend, depth_bias;
    bool additive;           /* ONE + ONE (signed: the SSR composite subtracts too) */
    bool premul;             /* ONE + ONE_MINUS_SRC_ALPHA: premultiplied see-through (alpha 0 = adds light) */
    int color_count;         /* 4 = main-pass MRT (colour, normal, reflection weight, ambient light) */
    VkCompareOp compare;
    VkCullModeFlags cull;
} PipeDesc;

static VkPipeline pipeline_create(const PipeDesc *d) {
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    VkShaderModule vs = shader_module(d->vs, d->vs_size), fs = d->fs ? shader_module(d->fs, d->fs_size) : VK_NULL_HANDLE;
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";

    VkVertexInputBindingDescription bindings[2];
    VkVertexInputAttributeDescription attrs[11];
    uint32_t nb = 0, na = 0;
    if (d->vertex == VTX_MESH || d->vertex == VTX_MESH_POS_ONLY || d->vertex == VTX_MESH_POS_NORMAL) {
        bindings[nb++] = (VkVertexInputBindingDescription){0, sizeof(LxVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        bindings[nb++] = (VkVertexInputBindingDescription){1, INST_BYTES, VK_VERTEX_INPUT_RATE_INSTANCE};
        attrs[na++] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        if (d->vertex == VTX_MESH_POS_NORMAL) attrs[na++] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12};
        if (d->vertex == VTX_MESH) {
            attrs[na++] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12};
            attrs[na++] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R32G32_SFLOAT, 24};
            attrs[na++] = (VkVertexInputAttributeDescription){3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32};
        }
        for (uint32_t c = 0; c < 4; c++) attrs[na++] = (VkVertexInputAttributeDescription){4 + c, 1, VK_FORMAT_R32G32B32A32_SFLOAT, c * 16};
        if (d->vertex == VTX_MESH) {
            attrs[na++] = (VkVertexInputAttributeDescription){8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 64};
            attrs[na++] = (VkVertexInputAttributeDescription){9, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(LxVertex, u2)};   /* lightmap UVs */
            attrs[na++] = (VkVertexInputAttributeDescription){10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 80};   /* lightmap tile: size, x, y, atlas (-1 none) */
        }
    } else if (d->vertex == VTX_UI) {
        bindings[nb++] = (VkVertexInputBindingDescription){0, sizeof(LxUiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        attrs[na++] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
        attrs[na++] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32_SFLOAT, 8};
        attrs[na++] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16};
    }
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = nb;
    vin.pVertexBindingDescriptions = bindings;
    vin.vertexAttributeDescriptionCount = na;
    vin.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d->cull;
    /* Meshes are counter-clockwise. The shaders flip Y in clip space and Vulkan's framebuffer Y
     * points down, so the two flips cancel and the winding stays counter-clockwise on screen. */
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = d->depth_bias;
    rs.depthBiasConstantFactor = 1.25f;
    rs.depthBiasSlopeFactor = 1.75f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d->depth_test;
    ds.depthWriteEnable = d->depth_write;
    ds.depthCompareOp = d->compare ? d->compare : VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cbs_arr[4];
    memset(cbs_arr, 0, sizeof cbs_arr);
    VkPipelineColorBlendAttachmentState cb;
    memset(&cb, 0, sizeof cb);
    cb.colorWriteMask = 0xf;
    if (d->additive) {
        cb.blendEnable = VK_TRUE;
        cb.srcColorBlendFactor = cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.colorBlendOp = cb.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (d->premul) {
        cb.blendEnable = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.colorBlendOp = cb.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (d->blend) {
        cb.blendEnable = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.colorBlendOp = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    uint32_t ncolor = d->color ? (d->color_count > 1 ? (uint32_t)d->color_count : 1u) : 0u;
    VkFormat formats[4] = {d->color, R.gnormal_format, R.gspec_format, R.gamb_format};
    cbs_arr[0] = cb;
    /* every attachment must use identical blend state (no independentBlend requirement) */
    for (uint32_t i = 1; i < ncolor; i++) cbs_arr[i] = cb;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = ncolor;
    cbs.pAttachments = cbs_arr;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = ncolor;
    ri.pColorAttachmentFormats = formats;
    ri.depthAttachmentFormat = d->depth;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    ci.stageCount = d->fs ? 2 : 1;
    ci.pStages = st;
    ci.pVertexInputState = &vin;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cbs;
    ci.pDynamicState = &dy;
    ci.layout = d->layout;
    VkPipeline p = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(R.device, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    vkDestroyShaderModule(R.device, vs, NULL);
    if (fs) vkDestroyShaderModule(R.device, fs, NULL);
    return p;
}

static VkDescriptorSetLayout set_layout(const VkDescriptorSetLayoutBinding *b, uint32_t n) {
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = n;
    ci.pBindings = b;
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(R.device, &ci, NULL, &l));
    return l;
}

static VkPipelineLayout pipe_layout(const VkDescriptorSetLayout *sets, uint32_t n, uint32_t push_size, VkShaderStageFlags push_stages) {
    VkPushConstantRange pr = {push_stages, 0, push_size};
    VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = n;
    ci.pSetLayouts = sets;
    ci.pushConstantRangeCount = push_size ? 1 : 0;
    ci.pPushConstantRanges = &pr;
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(R.device, &ci, NULL, &l));
    return l;
}

static VkDescriptorSet alloc_set(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = R.desc_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet s = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(R.device, &ai, &s));
    return s;
}

static void write_image(VkDescriptorSet set, uint32_t binding, VkImageView view, VkSampler sampler, VkImageLayout layout) {
    VkDescriptorImageInfo ii = {sampler, view, layout};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(R.device, 1, &w, 0, NULL);
}

/* ------------------------------------------------------------------ device setup */
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    LX_UNUSED(type); LX_UNUSED(user);
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        R.validation_errors++;
        LX_ERROR("[vulkan] %s", data->pMessage);
    } else if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) LX_WARN("[vulkan] %s", data->pMessage);
    return VK_FALSE;
}

int lx_renderer_validation_errors(void) { return R.validation_errors; }

static bool has_layer(const char *name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *ls = lx_alloc(sizeof *ls * (n ? n : 1));
    vkEnumerateInstanceLayerProperties(&n, ls);
    bool found = false;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(ls[i].layerName, name)) found = true;
    lx_free(ls);
    return found;
}

static bool has_instance_ext(const char *name) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *es = lx_alloc(sizeof *es * (n ? n : 1));
    vkEnumerateInstanceExtensionProperties(NULL, &n, es);
    bool found = false;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(es[i].extensionName, name)) found = true;
    lx_free(es);
    return found;
}

static bool device_has_ext(VkPhysicalDevice d, const char *name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(d, NULL, &n, NULL);
    VkExtensionProperties *es = lx_alloc(sizeof *es * (n ? n : 1));
    vkEnumerateDeviceExtensionProperties(d, NULL, &n, es);
    bool found = false;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(es[i].extensionName, name)) found = true;
    lx_free(es);
    return found;
}

static bool create_instance(void) {
    if (!lx_vk_load_global((PFN_vkGetInstanceProcAddr)lx_platform_vk_loader())) { LX_ERROR("Vulkan loader unavailable"); return false; }
    uint32_t api = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&api);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Luxora";
    app.pEngineName = "Luxora Engine";
    app.engineVersion = VK_MAKE_VERSION(LX_VERSION_MAJOR, LX_VERSION_MINOR, LX_VERSION_PATCH);
    app.apiVersion = api >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : (api >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1);
    const char *exts[8];
    int ne = lx_platform_vk_extensions(exts, 6);
    const char *layers[1];
    uint32_t nl = 0;
    const char *want_validation = getenv("LUXORA_VALIDATION");
    bool validation = want_validation && want_validation[0] == '1';
    if (validation) {
        if (has_layer("VK_LAYER_KHRONOS_validation")) layers[nl++] = "VK_LAYER_KHRONOS_validation";
        else LX_WARN("LUXORA_VALIDATION=1 but VK_LAYER_KHRONOS_validation is not installed");
    }
    bool debug_utils = has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) && validation;
    if (debug_utils) exts[ne++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)ne;
    ci.ppEnabledExtensionNames = exts;
    ci.enabledLayerCount = nl;
    ci.ppEnabledLayerNames = layers;
    VkResult r = vkCreateInstance(&ci, NULL, &R.instance);
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER) { LX_ERROR("No Vulkan driver found. Install or update your GPU driver (Vulkan 1.3)."); return false; }
    if (r != VK_SUCCESS) { LX_ERROR("vkCreateInstance failed: %s", lx_vk_result(r)); return false; }
    lx_vk_load_instance(R.instance);
    if (debug_utils && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT di = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        di.pfnUserCallback = debug_cb;
        vkCreateDebugUtilsMessengerEXT(R.instance, &di, NULL, &R.messenger);
        LX_INFO("Vulkan validation enabled");
    }
    return true;
}

static bool pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(R.instance, &n, NULL);
    if (!n) { LX_ERROR("No Vulkan devices found"); return false; }
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    vkEnumeratePhysicalDevices(R.instance, &n, devs);
    int best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (!device_has_ext(devs[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        bool dyn = p.apiVersion >= VK_API_VERSION_1_3 || device_has_ext(devs[i], "VK_KHR_dynamic_rendering");
        if (!dyn) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, NULL);
        VkQueueFamilyProperties qp[16];
        if (qn > 16) qn = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qp);
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, R.surface, &present);
            if (!(qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 500 : 10;
            if (score > best_score) {
                best_score = score; R.gpu = devs[i]; R.queue_family = q; R.props = p;
                R.queue_compute = (qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
            }
            break;
        }
    }
    if (best_score < 0) { LX_ERROR("No Vulkan device supports presentation + dynamic rendering"); return false; }
    vkGetPhysicalDeviceMemoryProperties(R.gpu, &R.mem_props);
    LX_INFO("Vulkan %u.%u.%u on %s", VK_VERSION_MAJOR(R.props.apiVersion), VK_VERSION_MINOR(R.props.apiVersion),
            VK_VERSION_PATCH(R.props.apiVersion), R.props.deviceName);
    return true;
}

static bool create_device(void) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = R.queue_family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char *exts[4];
    uint32_t ne = 0;
    exts[ne++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    bool core13 = R.props.apiVersion >= VK_API_VERSION_1_3;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeatures dr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    if (core13) {
        f13.dynamicRendering = VK_TRUE;
        ci.pNext = &f13;
    } else {
        exts[ne++] = "VK_KHR_dynamic_rendering";
        dr.dynamicRendering = VK_TRUE;
        ci.pNext = &dr;
    }
    VkPhysicalDeviceFeatures supported, enabled;
    vkGetPhysicalDeviceFeatures(R.gpu, &supported);
    memset(&enabled, 0, sizeof enabled);
    enabled.samplerAnisotropy = supported.samplerAnisotropy;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = ne;
    ci.ppEnabledExtensionNames = exts;
    ci.pEnabledFeatures = &enabled;
    VkResult r = vkCreateDevice(R.gpu, &ci, NULL, &R.device);
    if (r != VK_SUCCESS) { LX_ERROR("vkCreateDevice failed: %s", lx_vk_result(r)); return false; }
    if (!lx_vk_load_device(R.device)) return false;
    vkGetDeviceQueue(R.device, R.queue_family, 0, &R.queue);
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = R.queue_family;
    VK_CHECK(vkCreateCommandPool(R.device, &pi, NULL, &R.pool));
    VkFormat depth_candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
    R.depth_format = VK_FORMAT_D16_UNORM;
    for (int i = 0; i < 3; i++)
        if (format_supports(depth_candidates[i], VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            R.depth_format = depth_candidates[i];
            break;
        }
    R.hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    return true;
}

static void destroy_swapchain(void) {
    for (uint32_t i = 0; i < R.swap_count; i++) {
        vkDestroyImageView(R.device, R.swap_views[i], NULL);
        vkDestroySemaphore(R.device, R.swap_done[i], NULL);
    }
    R.swap_count = 0;
    if (R.swapchain) vkDestroySwapchainKHR(R.device, R.swapchain, NULL);
    R.swapchain = VK_NULL_HANDLE;
}

static bool create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(R.gpu, R.surface, &caps);
    int w, h;
    lx_window_size(&w, &h);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) { ext.width = (uint32_t)w; ext.height = (uint32_t)h; }
    if (ext.width == 0 || ext.height == 0) return false;
    if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (caps.maxImageExtent.width && ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
    if (caps.maxImageExtent.height && ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.gpu, R.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.gpu, R.surface, &nf, fmts);
    VkSurfaceFormatKHR chosen = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = fmts[i]; break; }
    uint32_t np = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(R.gpu, R.surface, &np, NULL);
    VkPresentModeKHR modes[16];
    if (np > 16) np = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(R.gpu, R.surface, &np, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!R.vsync)
        for (uint32_t i = 0; i < np; i++) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR) mode = modes[i];
        }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = R.surface;
    ci.minImageCount = count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    R.swap_transfer_src = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (R.swap_transfer_src ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    VkCompositeAlphaFlagBitsKHR alphas[] = {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR};
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (int i = 0; i < 4; i++) if (caps.supportedCompositeAlpha & alphas[i]) { ci.compositeAlpha = alphas[i]; break; }
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = R.swapchain;
    VkSwapchainKHR sc;
    VkResult r = vkCreateSwapchainKHR(R.device, &ci, NULL, &sc);
    if (r != VK_SUCCESS) { LX_ERROR("vkCreateSwapchainKHR: %s", lx_vk_result(r)); return false; }
    destroy_swapchain();
    R.swapchain = sc;
    R.swap_format = chosen.format;
    R.swap_extent = ext;
    vkGetSwapchainImagesKHR(R.device, sc, &R.swap_count, NULL);
    if (R.swap_count > 8) R.swap_count = 8;
    vkGetSwapchainImagesKHR(R.device, sc, &R.swap_count, R.swap_images);
    for (uint32_t i = 0; i < R.swap_count; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = R.swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = R.swap_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vkCreateImageView(R.device, &vi, NULL, &R.swap_views[i]);
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(R.device, &si, NULL, &R.swap_done[i]);
    }
    R.swap_dirty = false;
    return true;
}

/* ------------------------------------------------------------------ resources */
/* one layer per cascade, and one more: the precipitation map (the area around the camera seen from straight above) */
static void create_shadow_map(int res, int layers) {
    image_destroy(&R.shadow_map);
    ImageDesc d = {(uint32_t)res, (uint32_t)res, 1, (uint32_t)layers + 1, 1, R.depth_format,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                   VK_IMAGE_VIEW_TYPE_2D_ARRAY, true};
    image_create(&R.shadow_map, &d);
    VkCommandBuffer cmd = one_shot_begin();
    image_barrier(cmd, R.shadow_map.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, (uint32_t)layers + 1);
    one_shot_end(cmd);
    R.shadow_res = res;
    R.shadow_layers = layers;
}

/* the point / spot shadow atlas follows the shadow quality: tiles of shadow_resolution / 8 */
static void create_local_shadow(int res) {
    image_destroy(&R.local_shadow);
    ImageDesc d = {(uint32_t)res, (uint32_t)res, 1, 1, 1, R.depth_format,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                   VK_IMAGE_VIEW_TYPE_2D, false};
    image_create(&R.local_shadow, &d);
    VkCommandBuffer cmd = one_shot_begin();
    image_barrier(cmd, R.local_shadow.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
    one_shot_end(cmd);
    R.local_shadow_res = res;
}
static int local_shadow_size(int shadow_res) { return shadow_res <= 1024 ? 1024 : shadow_res >= 4096 ? 4096 : 2048; }

/* the cube faces of a point light's shadow: direction and up (the shader picks the same ones) */
static const float CUBE_FACES[6][6] = {{1, 0, 0, 0, 1, 0}, {-1, 0, 0, 0, 1, 0}, {0, 1, 0, 0, 0, 1},
                                       {0, -1, 0, 0, 0, 1}, {0, 0, 1, 0, 1, 0}, {0, 0, -1, 0, 1, 0}};

static uint16_t f2h(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7c00u);
    return (uint16_t)(sign | ((uint32_t)e << 10) | (m >> 13));
}

static void create_gi_images(int rx, int ry, int rz) {
    for (int c = 0; c < 3; c++) image_destroy(&R.gi[c]);
    ImageDesc d = {(uint32_t)rx, (uint32_t)ry, (uint32_t)rz, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | 0, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_3D, false};
    size_t bytes = (size_t)rx * (size_t)ry * (size_t)rz * 8;
    GpuBuffer zero;
    buffer_create(&zero, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memset(zero.mapped, 0, bytes);
    VkCommandBuffer cmd = one_shot_begin();
    for (int c = 0; c < 3; c++) {
        image_create(&R.gi[c], &d);
        image_barrier(cmd, R.gi[c].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1, 1);
        VkBufferImageCopy region;
        memset(&region, 0, sizeof region);
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = (uint32_t)rx;
        region.imageExtent.height = (uint32_t)ry;
        region.imageExtent.depth = (uint32_t)rz;
        vkCmdCopyBufferToImage(cmd, zero.buf, R.gi[c].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        image_barrier(cmd, R.gi[c].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
    }
    one_shot_end(cmd);
    buffer_destroy(&zero);
    for (int i = 0; i < 2; i++) {
        image_destroy(&R.gi_vis[i]);
        ImageDesc vd = {(uint32_t)rx, (uint32_t)ry, (uint32_t)rz, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | 0,
                        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_3D, false};
        image_create(&R.gi_vis[i], &vd);
    }
    {   /* open everywhere until the real walls arrive */
        size_t vb = (size_t)rx * (size_t)ry * (size_t)rz * 4;
        GpuBuffer ones;
        buffer_create(&ones, vb, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        memset(ones.mapped, 255, vb);
        VkCommandBuffer vc = one_shot_begin();
        for (int i = 0; i < 2; i++) {
            image_barrier(vc, R.gi_vis[i].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1, 1);
            VkBufferImageCopy region;
            memset(&region, 0, sizeof region);
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = (uint32_t)rx; region.imageExtent.height = (uint32_t)ry; region.imageExtent.depth = (uint32_t)rz;
            vkCmdCopyBufferToImage(vc, ones.buf, R.gi_vis[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            image_barrier(vc, R.gi_vis[i].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
        }
        one_shot_end(vc);
        buffer_destroy(&ones);
        R.gi_up_vis[2] = 0;
    }
    R.gi_res[0] = rx; R.gi_res[1] = ry; R.gi_res[2] = rz;
    R.gi_up_sh[2] = 0;
    (void)bytes;
}

static void write_scene_sets(void) {
    for (int f = 0; f < FRAMES; f++) {
        VkDescriptorSet s = R.frames[f].scene_set;
        VkDescriptorBufferInfo bi = {R.frames[f].ubo.buf, 0, sizeof(SceneUBO)};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(R.device, 1, &w, 0, NULL);
        write_image(s, 1, R.shadow_map.view, R.shadow_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_image(s, 5, R.local_shadow.view, R.shadow_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (int c = 0; c < 3; c++) write_image(s, (uint32_t)(2 + c), R.gi[c].view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (int c = 0; c < 2; c++) write_image(s, (uint32_t)(6 + c), R.gi_vis[c].view, R.point_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (int c = 0; c < 3; c++) write_image(s, (uint32_t)(8 + c), R.gc[c].view, R.point_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        for (int c = 0; c < 2; c++) write_image(s, (uint32_t)(11 + c), R.gc_vis[c].view, R.point_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_image(s, 15, R.gsdf.view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_image(s, 13, R.lightmap.view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_image(s, 14, R.bvol.view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

static VkSampler make_sampler(VkFilter filter, VkSamplerAddressMode mode, bool compare, float max_lod) {
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = ci.minFilter = filter;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = mode;
    ci.maxLod = max_lod;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (compare) { ci.compareEnable = VK_TRUE; ci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL; }
    VkSampler s;
    VK_CHECK(vkCreateSampler(R.device, &ci, NULL, &s));
    return s;
}

/* ------------------------------------------------------------------ reflection probe resources */
static void probes_destroy(void) {
    for (int i = 0; i < PROBE_SLOTS; i++) {
        ProbeSlot *p = &R.probes[i];
        for (int m = 0; m < PROBE_MAX_MIPS; m++) {
            for (int f = 0; f < 6; f++)
                if (p->face_view[m][f]) vkDestroyImageView(R.device, p->face_view[m][f], NULL);
            if (p->src_view[m]) vkDestroyImageView(R.device, p->src_view[m], NULL);
            if (p->src_set[m]) vkFreeDescriptorSets(R.device, R.desc_pool, 1, &p->src_set[m]);
        }
        image_destroy(&p->cube);
        memset(p, 0, sizeof *p);
    }
    image_destroy(&R.probe_depth);
    R.probe_res = 0;
}

static VkImageView cube_view(VkImage img, VkImageViewType type, uint32_t base_mip, uint32_t mips, uint32_t base_layer, uint32_t layers) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = type;
    vi.format = R.hdr_format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = base_mip;
    vi.subresourceRange.levelCount = mips;
    vi.subresourceRange.baseArrayLayer = base_layer;
    vi.subresourceRange.layerCount = layers;
    VkImageView v = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(R.device, &vi, NULL, &v));
    return v;
}

static void write_probe_set(VkDescriptorSet set, bool dummy) {
    VkDescriptorImageInfo ii[PROBE_SLOTS];
    for (int i = 0; i < PROBE_SLOTS; i++) {
        ii[i].sampler = R.cube_sampler;
        ii[i].imageView = dummy ? R.dummy_cube.view : R.probes[i].cube.view;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = PROBE_SLOTS;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = ii;
    vkUpdateDescriptorSets(R.device, 1, &w, 0, NULL);
}

/* (Re)create every probe cubemap at the given face size, with GGX mips down to 8x8 (max 7 levels). */
static void probes_create(int res) {
    probes_destroy();
    if (res < 16) res = 16;
    if (res > 1024) res = 1024;
    int mips = 0;
    for (int r = res; r >= 8 && mips < PROBE_MAX_MIPS; r >>= 1) mips++;
    R.probe_res = res;
    R.probe_mips = mips;
    VkCommandBuffer cmd = one_shot_begin();
    for (int i = 0; i < PROBE_SLOTS; i++) {
        ProbeSlot *p = &R.probes[i];
        ImageDesc d = {(uint32_t)res, (uint32_t)res, 1, 6, (uint32_t)mips, R.hdr_format,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                       VK_IMAGE_VIEW_TYPE_CUBE, false, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT};
        image_create(&p->cube, &d);
        for (int m = 0; m < mips; m++) {
            for (int f = 0; f < 6; f++) p->face_view[m][f] = cube_view(p->cube.img, VK_IMAGE_VIEW_TYPE_2D, (uint32_t)m, 1, (uint32_t)f, 1);
            if (m > 0) {
                p->src_view[m] = cube_view(p->cube.img, VK_IMAGE_VIEW_TYPE_CUBE, 0, (uint32_t)m, 0, 6);
                p->src_set[m] = alloc_set(R.tex_layout);
                write_image(p->src_set[m], 0, p->src_view[m], R.cube_sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
        /* contents are undefined until captured; shaders only read a slot once it is valid */
        image_barrier(cmd, p->cube.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0,
                      (uint32_t)mips, 6);
    }
    ImageDesc dd = {(uint32_t)res, (uint32_t)res, 1, 1, 1, R.depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D, false, 0};
    image_create(&R.probe_depth, &dd);
    one_shot_end(cmd);
    write_probe_set(R.probe_set, false);
}

static void create_dummy_cube(void) {
    ImageDesc d = {1, 1, 1, 6, 1, R.hdr_format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   VK_IMAGE_VIEW_TYPE_CUBE, false, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT};
    image_create(&R.dummy_cube, &d);
    VkCommandBuffer cmd = one_shot_begin();
    image_barrier(cmd, R.dummy_cube.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1, 6);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    vkCmdClearColorImage(cmd, R.dummy_cube.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    image_barrier(cmd, R.dummy_cube.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 6);
    one_shot_end(cmd);
}

/* a device-local storage buffer filled from memory (scene data: uploaded when the static geometry changes) */
static void gi_upload(GpuBuffer *b, const void *data, size_t bytes) {
    buffer_destroy(b);
    buffer_create(b, bytes ? bytes : 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    GpuBuffer st;
    buffer_create(&st, bytes ? bytes : 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (bytes) memcpy(st.mapped, data, bytes);
    VkCommandBuffer cmd = one_shot_begin();
    VkBufferCopy rg = {0, 0, bytes ? bytes : 16};
    vkCmdCopyBuffer(cmd, st.buf, b->buf, 1, &rg);
    one_shot_end(cmd);
    buffer_destroy(&st);
}
static void gi_zero_buffer(GpuBuffer *b, size_t bytes) {
    buffer_destroy(b);
    buffer_create(b, bytes ? bytes : 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    VkCommandBuffer cmd = one_shot_begin();
    vkCmdFillBuffer(cmd, b->buf, 0, VK_WHOLE_SIZE, 0);
    one_shot_end(cmd);
}

/* ------------------------------------------------------------------ baked lighting */
/* Lightmaps live in one 2D array texture: layer 2a is atlas a's irradiance, 2a+1 its light direction.
 * data: half floats, size*size*4 per layer; NULL makes the 1-texel placeholder. */
static void lightmap_upload(const uint16_t *data, int size, int atlases) {
    image_destroy(&R.lightmap);
    int layers = atlases > 0 ? atlases * 2 : 2;
    ImageDesc d = {(uint32_t)size, (uint32_t)size, 1, (uint32_t)layers, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY, false, 0};
    image_create(&R.lightmap, &d);
    size_t layer_bytes = (size_t)size * (size_t)size * 8;
    GpuBuffer st;
    buffer_create(&st, layer_bytes * (size_t)layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (data) memcpy(st.mapped, data, layer_bytes * (size_t)layers);
    else memset(st.mapped, 0, layer_bytes * (size_t)layers);
    VkCommandBuffer cmd = one_shot_begin();
    image_barrier(cmd, R.lightmap.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1, (uint32_t)layers);
    VkBufferImageCopy region = {0};
    region.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, (uint32_t)layers};
    region.imageExtent = (VkExtent3D){(uint32_t)size, (uint32_t)size, 1};
    vkCmdCopyBufferToImage(cmd, st.buf, R.lightmap.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cmd, R.lightmap.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1,
                  (uint32_t)layers);
    one_shot_end(cmd);
    buffer_destroy(&st);
    R.lm_count = data ? atlases : 0;
}

/* the volumes' 3D texture: w x h x (4 * depth) half-float RGBA; NULL = a 1-texel placeholder */
static void bvol_upload(const uint16_t *data, int w, int h, int depth) {
    image_destroy(&R.bvol);
    int d4 = depth * 4;
    ImageDesc d = {(uint32_t)w, (uint32_t)h, (uint32_t)d4, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_3D, false, 0};
    image_create(&R.bvol, &d);
    size_t bytes = (size_t)w * h * d4 * 8;
    GpuBuffer st;
    buffer_create(&st, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (data) memcpy(st.mapped, data, bytes);
    else memset(st.mapped, 0, bytes);
    VkCommandBuffer cmd = one_shot_begin();
    image_barrier(cmd, R.bvol.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1, 1);
    VkBufferImageCopy region = {0};
    region.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = (VkExtent3D){(uint32_t)w, (uint32_t)h, (uint32_t)d4};
    vkCmdCopyBufferToImage(cmd, st.buf, R.bvol.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cmd, R.bvol.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
    one_shot_end(cmd);
    buffer_destroy(&st);
    R.bvol_depth = depth;
}

/* the scene's volume files (vol0.lxlv ...) packed into one 3D texture */
static void volumes_load(const LxScene *s) {
    uint16_t *one[LX_MAX_BAKE_VOLUMES] = {0};
    int n[LX_MAX_BAKE_VOLUMES][3], count = s->volume_count < LX_MAX_BAKE_VOLUMES ? s->volume_count : LX_MAX_BAKE_VOLUMES;
    int W = 1, H = 1, D = 0;
    R.bvol_count = 0;
    for (int v = 0; v < count; v++) {
        char rel[LX_REF_MAX + 32], path[LX_PATH_MAX];
        snprintf(rel, sizeof rel, "%s/vol%d.lxlv", s->lightmap_ref, v);
        lx_asset_path(path, sizeof path, rel);
        one[v] = lx_volume_read(path, n[v]);
        if (!one[v]) { LX_WARN("Lighting volume missing or broken: %s (bake again)", path); count = v; break; }
        if (n[v][0] > W) W = n[v][0];
        if (n[v][1] > H) H = n[v][1];
        R.bvol_info[v][0] = D;
        R.bvol_info[v][1] = n[v][0]; R.bvol_info[v][2] = n[v][1]; R.bvol_info[v][3] = n[v][2];
        D += n[v][2];
    }
    if (!count) { bvol_upload(NULL, 1, 1, 1); for (int v = 0; v < count || v < LX_MAX_BAKE_VOLUMES; v++) lx_free(one[v]); return; }
    uint16_t *all = lx_calloc((size_t)W * H * D * 4 * 4, sizeof(uint16_t));
    for (int v = 0; v < count; v++) {
        size_t vc = (size_t)n[v][0] * n[v][1] * n[v][2];
        for (int layer = 0; layer < 4; layer++)
            for (int z = 0; z < n[v][2]; z++)
                for (int y = 0; y < n[v][1]; y++)
                    for (int x = 0; x < n[v][0]; x++) {
                        size_t src = ((size_t)layer * vc + ((size_t)z * n[v][1] + y) * n[v][0] + x) * 4;
                        size_t dz = (size_t)layer * D + (size_t)R.bvol_info[v][0] + (size_t)z;
                        size_t dst = ((dz * H + (size_t)y) * W + (size_t)x) * 4;
                        memcpy(all + dst, one[v] + src, 8);
                    }
        lx_free(one[v]);
    }
    bvol_upload(all, W, H, D);
    lx_free(all);
    R.bvol_count = count;
}

/* loads the scene's lightmaps when they changed (a bake, a clear, another scene). Only before anything of this
 * frame is drawn: the scene descriptor sets get rewritten. */
static void lightmaps_sync(LxScene *s, FrameData *fr) {
    uint32_t epoch = lx_lightmap_epoch();
    if (!strcmp(R.lm_ref, s->lightmap_ref) && R.lm_epoch == epoch && (s->lightmap_count > 0) == (R.lm_count > 0) &&
        (s->volume_count > 0) == (R.bvol_count > 0))
        return;
    if (fr->ubo_used) return;   /* next frame */
    vkDeviceWaitIdle(R.device);
    lx_strcpy(R.lm_ref, sizeof R.lm_ref, s->lightmap_ref);
    R.lm_epoch = epoch;
    uint16_t *all = NULL;
    int size = 0, n = s->lightmap_count;
    for (int a = 0; a < n; a++) {
        char rel[LX_REF_MAX + 32], path[LX_PATH_MAX];
        snprintf(rel, sizeof rel, "%s/lm%d.lxlm", s->lightmap_ref, a);
        lx_asset_path(path, sizeof path, rel);
        int sz = 0;
        uint16_t *one = lx_lightmap_read(path, &sz);
        if (!one || (a && sz != size)) { LX_WARN("Lightmap missing or broken: %s (bake again)", path); lx_free(one); lx_free(all); all = NULL; n = 0; break; }
        size = sz;
        size_t layer = (size_t)sz * sz * 4;
        if (!all) all = lx_alloc(layer * 2 * sizeof(uint16_t) * (size_t)n);
        memcpy(all + layer * 2 * (size_t)a, one, layer * 2 * sizeof(uint16_t));
        lx_free(one);
    }
    if (all) lightmap_upload(all, size, n);
    else lightmap_upload(NULL, 1, 0);
    lx_free(all);
    volumes_load(s);
    write_scene_sets();
}

/* this frame's volume boxes: world -> 0..1 inside each baked volume */
static void volumes_fill_ubo(LxScene *s, SceneUBO *ubo) {
    int used = 0;
    for (int i = 1; i < s->count && R.bvol_count; i++) {
        const LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_BAKE_VOLUME) || d->vol_index < 0 || d->vol_index >= R.bvol_count) continue;
        LxVec3 sz = d->bvol.size;
        if (fabsf(sz.x) < 1e-4f || fabsf(sz.y) < 1e-4f || fabsf(sz.z) < 1e-4f) continue;
        LxMat4 box = lx_mat4_mul(d->world, lx_mat4_trs(lx_v3(0, 0, 0), lx_quat_identity(), sz));
        LxMat4 inv = lx_mat4_inverse(box);
        for (int r = 0; r < 3; r++) {
            ubo->bvol_rows[used][r][0] = inv.m[0 + r];
            ubo->bvol_rows[used][r][1] = inv.m[4 + r];
            ubo->bvol_rows[used][r][2] = inv.m[8 + r];
            ubo->bvol_rows[used][r][3] = inv.m[12 + r] + 0.5f;
        }
        for (int k = 0; k < 4; k++) ubo->bvol_info[used][k] = (float)R.bvol_info[d->vol_index][k];
        if (++used >= LX_MAX_BAKE_VOLUMES) break;
    }
    ubo->bvol_params[0] = (float)used;
    ubo->bvol_params[1] = (float)R.bvol_depth;
}

/* the lightmap tile an object draws with (or none) */
static const float SWAYS[4] = {0, 0, 0, -2};   /* grass: the vertex shader bends it in the wind */
static const float *entity_lightmap(const LxEntityData *d, const LxMesh *mesh, float out[4]) {
    if (d->generated == LX_GEN_GRASS) return SWAYS;
    if (d->lm_index < 0 || d->lm_index >= R.lm_count || !mesh->has_uv2 || mesh->skin || mesh->dynamic) return NO_LIGHTMAP;
    out[0] = d->lm_st[0]; out[1] = d->lm_st[1]; out[2] = d->lm_st[2]; out[3] = (float)d->lm_index;
    return out;
}

static void lm_comp_free(void) {
    for (int i = 0; i < 8; i++) buffer_destroy(&R.lm_buf[i]);
    if (R.lm_pipe) vkDestroyPipeline(R.device, R.lm_pipe, NULL);
    if (R.lm_pl) vkDestroyPipelineLayout(R.device, R.lm_pl, NULL);
    if (R.lm_comp_layout) vkDestroyDescriptorSetLayout(R.device, R.lm_comp_layout, NULL);
    R.lm_pipe = VK_NULL_HANDLE;
    R.lm_pl = VK_NULL_HANDLE;
    R.lm_comp_layout = VK_NULL_HANDLE;
}

static bool lm_comp_init(void) {
    if (R.lm_pipe) return true;
    VkDescriptorSetLayoutBinding b[8];
    for (uint32_t i = 0; i < 8; i++) b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    R.lm_comp_layout = set_layout(b, 8);
    R.lm_pl = pipe_layout(&R.lm_comp_layout, 1, 80, VK_SHADER_STAGE_COMPUTE_BIT);
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = shader_module(lx_spv_lm_bake_comp, sizeof lx_spv_lm_bake_comp);
    ci.stage.pName = "main";
    ci.layout = R.lm_pl;
    VkResult r = vkCreateComputePipelines(R.device, VK_NULL_HANDLE, 1, &ci, NULL, &R.lm_pipe);
    vkDestroyShaderModule(R.device, ci.stage.module, NULL);
    if (r != VK_SUCCESS) { LX_WARN("Lightmap baking pipeline failed (%d): baking on the CPU", (int)r); R.lm_pipe = VK_NULL_HANDLE; return false; }
    R.lm_set = alloc_set(R.lm_comp_layout);
    return true;
}

bool lx_renderer_lightmap_begin(const LxLightmapBake *b) {
    const char *force_cpu = getenv("LUXORA_BAKE_CPU");
    if (!R.device || !R.queue_compute || (force_cpu && force_cpu[0] == '1') || !lm_comp_init()) return false;
    vkDeviceWaitIdle(R.device);
    const LxBvh *bvh = &b->bvh;
    int nn = bvh->node_count ? bvh->node_count : 1, nt = bvh->tri_count ? bvh->tri_count : 1;
    float *nodes = lx_calloc((size_t)nn * 8, sizeof(float));
    for (int i = 0; i < bvh->node_count; i++) {
        const LxBvhNode *n = &bvh->nodes[i];
        float *o = nodes + i * 8;
        o[0] = n->bounds.min.x; o[1] = n->bounds.min.y; o[2] = n->bounds.min.z; memcpy(&o[3], &n->left_or_first, 4);
        o[4] = n->bounds.max.x; o[5] = n->bounds.max.y; o[6] = n->bounds.max.z; memcpy(&o[7], &n->count, 4);
    }
    if (!bvh->node_count) { nodes[0] = nodes[1] = nodes[2] = 1; nodes[4] = nodes[5] = nodes[6] = -1; }   /* nothing to hit: an empty box */
    float *tris = lx_calloc((size_t)nt * 16, sizeof(float));
    for (int k = 0; k < bvh->tri_count; k++) {
        int t = bvh->tri_index[k], mat = bvh->mat_index[t];
        float *o = tris + k * 16;
        o[0] = bvh->v0[t].x; o[1] = bvh->v0[t].y; o[2] = bvh->v0[t].z; memcpy(&o[3], &mat, 4);
        o[4] = bvh->e1[t].x; o[5] = bvh->e1[t].y; o[6] = bvh->e1[t].z;
        o[8] = bvh->e2[t].x; o[9] = bvh->e2[t].y; o[10] = bvh->e2[t].z;
        o[12] = bvh->normals[t].x; o[13] = bvh->normals[t].y; o[14] = bvh->normals[t].z;
    }
    int nm = bvh->material_count ? bvh->material_count : 1;
    float *mats = lx_calloc((size_t)nm * 8, sizeof(float));
    for (int i = 0; i < bvh->material_count; i++) {
        LxVec3 a = bvh->materials[i].albedo, e = bvh->materials[i].emissive;
        mats[i * 8] = a.x; mats[i * 8 + 1] = a.y; mats[i * 8 + 2] = a.z;
        mats[i * 8 + 4] = e.x; mats[i * 8 + 5] = e.y; mats[i * 8 + 6] = e.z;
    }
    int nl = b->light_count ? b->light_count : 1;
    float *lights = lx_calloc((size_t)nl * 16, sizeof(float));
    for (int i = 0; i < b->light_count; i++) {
        const LxLmLight *L = &b->lights[i];
        float *o = lights + i * 16;
        o[0] = L->pos.x; o[1] = L->pos.y; o[2] = L->pos.z; o[3] = (float)L->type;
        o[4] = L->color.x; o[5] = L->color.y; o[6] = L->color.z; o[7] = (float)L->mode;
        o[8] = L->dir.x; o[9] = L->dir.y; o[10] = L->dir.z; o[11] = L->range;
        o[12] = L->cos_outer; o[13] = L->radius; o[14] = L->cos_size; o[15] = (float)(L->mask_channel + 1);
    }
    gi_upload(&R.lm_buf[0], nodes, (size_t)nn * 32);
    gi_upload(&R.lm_buf[1], tris, (size_t)nt * 64);
    gi_upload(&R.lm_buf[2], mats, (size_t)nm * 32);
    gi_upload(&R.lm_buf[3], lights, (size_t)nl * 64);
    static const float none[12] = {0};
    if (b->texel_count) gi_upload(&R.lm_buf[4], b->texels, (size_t)b->texel_count * 48);
    else gi_upload(&R.lm_buf[4], none, sizeof none);
    gi_zero_buffer(&R.lm_buf[5], (size_t)(b->texel_count ? b->texel_count : 1) * LX_LM_ACC * 4);
    if (b->voxel_count) gi_upload(&R.lm_buf[6], b->vox_pos, (size_t)b->voxel_count * 16);
    else gi_upload(&R.lm_buf[6], none, 16);
    gi_zero_buffer(&R.lm_buf[7], (size_t)(b->voxel_count ? b->voxel_count : 1) * 80);
    lx_free(nodes); lx_free(tris); lx_free(mats); lx_free(lights);
    VkDescriptorBufferInfo bi[8];
    VkWriteDescriptorSet w[8];
    for (int i = 0; i < 8; i++) {
        bi[i] = (VkDescriptorBufferInfo){R.lm_buf[i].buf, 0, VK_WHOLE_SIZE};
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = R.lm_set;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(R.device, 8, w, 0, NULL);
    R.lm_baking = true;
    return true;
}

void lx_renderer_lightmap_trace(LxLightmapBake *b, int first, int count, uint32_t sample) {
    if (!R.lm_baking) return;
    struct { float zenith[4], horizon[4], ground[4]; int32_t counts[4]; uint32_t sample[4]; } pc;
    memset(&pc, 0, sizeof pc);
    pc.zenith[0] = b->sky_zenith.x; pc.zenith[1] = b->sky_zenith.y; pc.zenith[2] = b->sky_zenith.z; pc.zenith[3] = b->ambient;
    pc.horizon[0] = b->sky_horizon.x; pc.horizon[1] = b->sky_horizon.y; pc.horizon[2] = b->sky_horizon.z; pc.horizon[3] = b->eps;
    pc.ground[0] = b->ground.x; pc.ground[1] = b->ground.y; pc.ground[2] = b->ground.z;
    pc.ground[3] = b->set.ao_intensity > 0 ? b->set.ao_radius : 0;   /* ambient occlusion ray length (0 = off) */
    /* work items: texels first, then volume voxels (a slice is always one kind) */
    bool vox = first >= b->texel_count;
    pc.counts[0] = vox ? first - b->texel_count : first; pc.counts[1] = count; pc.counts[2] = b->light_count; pc.counts[3] = b->bounces;
    pc.sample[0] = sample;
    pc.sample[1] = vox ? 1u : 0u;
    pc.sample[2] = b->sky_separate ? 1u : 0u;
    VkCommandBuffer cmd = one_shot_begin();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.lm_pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, R.lm_pl, 0, 1, &R.lm_set, 0, NULL);
    vkCmdPushConstants(cmd, R.lm_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (uint32_t)(count + 63) / 64, 1, 1);
    one_shot_end(cmd);
}

bool lx_renderer_lightmap_end(LxLightmapBake *b, bool read_back) {
    if (!R.lm_baking) return false;
    bool ok = true;
    for (int pass = 0; read_back && pass < 2; pass++) {   /* texel sums, then voxel sums (5 vec4 -> 20 floats each, same layout) */
        size_t bytes = pass ? (size_t)b->voxel_count * 80 : (size_t)b->texel_count * LX_LM_ACC * 4;
        if (!bytes) continue;
        GpuBuffer st;
        buffer_create(&st, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VkCommandBuffer cmd = one_shot_begin();
        VkBufferCopy rg = {0, 0, bytes};
        vkCmdCopyBuffer(cmd, R.lm_buf[pass ? 7 : 5].buf, st.buf, 1, &rg);
        one_shot_end(cmd);
        if (st.mapped) memcpy(pass ? b->vox_acc : b->accum, st.mapped, bytes);
        else ok = false;
        buffer_destroy(&st);
    }
    for (int i = 0; i < 8; i++) buffer_destroy(&R.lm_buf[i]);
    R.lm_baking = false;
    return ok;
}

static void clear_image(VkCommandBuffer cmd, GpuImage *im, float v) {
    image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1, 1);
    VkClearColorValue c = {{v, v, v, v}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
}

/* the GI camera levels' textures (res x res x res each, stacked along z); res 0 = placeholders */
static void gc_create(int res) {
    if (R.device) vkDeviceWaitIdle(R.device);
    int r = res > 0 ? res : 1;
    VkImageUsageFlags storage = 0;
    for (int c = 0; c < 5; c++) {
        GpuImage *im = c < 3 ? &R.gc[c] : &R.gc_vis[c - 3];
        image_destroy(im);
        ImageDesc d = {(uint32_t)r, (uint32_t)r, (uint32_t)(r * LX_GI_CASCADES), 1, 1, c < 3 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | storage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_3D, false};
        image_create(im, &d);
    }
    image_destroy(&R.gsdf);
    ImageDesc sd = {(uint32_t)(r * 2), (uint32_t)(r * 2), (uint32_t)(r * 2 * LX_GI_CASCADES), 1, 1, VK_FORMAT_R16_SFLOAT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_3D, false};
    image_create(&R.gsdf, &sd);
    VkCommandBuffer cmd = one_shot_begin();
    for (int c = 0; c < 5; c++) clear_image(cmd, c < 3 ? &R.gc[c] : &R.gc_vis[c - 3], c < 3 ? 0.0f : 1.0f);
    clear_image(cmd, &R.gsdf, 3.0f);
    one_shot_end(cmd);
    R.gc_res = res;
    R.gi_up_sh[0] = R.gi_up_sh[1] = R.gi_up_vis[0] = R.gi_up_vis[1] = R.gi_up_sdf[0] = R.gi_up_sdf[1] = 0;
}

static void create_static_resources(void) {
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 24}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 4096;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(R.device, &pi, NULL, &R.desc_pool));

    VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding sb[16] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, vf, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    R.scene_layout = set_layout(sb, 16);
    VkDescriptorSetLayoutBinding mb[MAT_TEXTURES + 2];   /* + a terrain's layer arrays (placeholders for other materials) */
    for (uint32_t i = 0; i < MAT_TEXTURES + 2; i++) mb[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    R.material_layout = set_layout(mb, MAT_TEXTURES + 2);
    VkDescriptorSetLayoutBinding tb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    R.tex_layout = set_layout(&tb, 1);
    VkDescriptorSetLayoutBinding pb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PROBE_SLOTS, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    R.probe_layout = set_layout(&pb, 1);
    VkDescriptorSetLayoutBinding rb[5];
    for (uint32_t i = 0; i < 5; i++) rb[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    R.ssr_layout = set_layout(rb, 5);

    /* small formats for the SSR inputs: 10-bit normals, 8-bit reflection weight + roughness */
    R.gnormal_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    if (!format_supports(R.gnormal_format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        R.gnormal_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    R.gspec_format = VK_FORMAT_R8G8B8A8_UNORM;
    R.gamb_format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;   /* the ambient light, for ambient occlusion */
    if (!format_supports(R.gamb_format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        R.gamb_format = R.hdr_format;
    VkDescriptorSetLayoutBinding qb[4];
    for (uint32_t i = 0; i < 4; i++) qb[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    R.post_set_layout = set_layout(qb, 4);
    VkDescriptorSetLayout post_sets[2] = {R.scene_layout, R.post_set_layout};
    R.post_layout = pipe_layout(post_sets, 2, 64, VK_SHADER_STAGE_FRAGMENT_BIT);

    VkDescriptorSetLayout pbr_sets[3] = {R.scene_layout, R.material_layout, R.probe_layout};
    R.pbr_layout = pipe_layout(pbr_sets, 3, 96, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkDescriptorSetLayout ssr_sets[3] = {R.scene_layout, R.ssr_layout, R.probe_layout};
    R.ssr_pipe_layout = pipe_layout(ssr_sets, 3, 32, VK_SHADER_STAGE_FRAGMENT_BIT);
    R.filter_layout = pipe_layout(&R.tex_layout, 1, 16, VK_SHADER_STAGE_FRAGMENT_BIT);
    R.shadow_layout = pipe_layout(NULL, 0, 64, VK_SHADER_STAGE_VERTEX_BIT);
    R.scene_only_layout = pipe_layout(&R.scene_layout, 1, 0, 0);
    R.outline_layout = pipe_layout(&R.scene_layout, 1, 16, VK_SHADER_STAGE_FRAGMENT_BIT);
    R.tonemap_layout = pipe_layout(&R.tex_layout, 1, 16, VK_SHADER_STAGE_FRAGMENT_BIT);
    R.ui_layout = pipe_layout(&R.tex_layout, 1, 16, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    R.linear_repeat = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, false, 16.0f);
    R.linear_clamp = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false, 0.0f);
    R.shadow_sampler = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, true, 0.0f);
    R.point_clamp = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false, 0.0f);
    R.cube_sampler = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false, 16.0f);

    PipeDesc p;
    memset(&p, 0, sizeof p);
    p.vs = lx_spv_pbr_vert; p.vs_size = sizeof lx_spv_pbr_vert;
    p.fs = lx_spv_pbr_frag; p.fs_size = sizeof lx_spv_pbr_frag;
    p.layout = R.pbr_layout; p.vertex = VTX_MESH; p.color = R.hdr_format; p.depth = R.depth_format;
    p.depth_test = p.depth_write = true; p.compare = VK_COMPARE_OP_LESS_OR_EQUAL; p.cull = VK_CULL_MODE_BACK_BIT;
    R.pbr = pipeline_create(&p);
    /* probe faces are rendered mirrored (cubemap convention), which flips the triangle winding */
    p.cull = VK_CULL_MODE_FRONT_BIT;
    R.pbr_probe = pipeline_create(&p);
    p.cull = VK_CULL_MODE_BACK_BIT;
    p.fs = lx_spv_pbr_mrt_frag; p.fs_size = sizeof lx_spv_pbr_mrt_frag;
    p.color_count = 4;
    R.pbr_mrt = pipeline_create(&p);
    /* material variants (see-through pixels, both faces) */
    R.pbr_var[0][0][0] = R.pbr; R.pbr_var[1][0][0] = R.pbr_mrt; R.pbr_var[2][0][0] = R.pbr_probe;
    for (int kind = 0; kind < 3; kind++)
        for (int cut = 0; cut < 2; cut++)
            for (int two = 0; two < 2; two++) {
                if (!cut && !two) continue;
                p.color_count = kind == 1 ? 4 : 1;
                if (kind == 1) { p.fs = cut ? lx_spv_pbr_mrt_cut_frag : lx_spv_pbr_mrt_frag; p.fs_size = cut ? sizeof lx_spv_pbr_mrt_cut_frag : sizeof lx_spv_pbr_mrt_frag; }
                else { p.fs = cut ? lx_spv_pbr_cut_frag : lx_spv_pbr_frag; p.fs_size = cut ? sizeof lx_spv_pbr_cut_frag : sizeof lx_spv_pbr_frag; }
                p.cull = two ? VK_CULL_MODE_NONE : kind == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
                R.pbr_var[kind][cut][two] = pipeline_create(&p);
            }
    /* see-through: after the solid world, tested against its depth, not written; premultiplied colour */
    p.fs = lx_spv_pbr_blend_frag; p.fs_size = sizeof lx_spv_pbr_blend_frag;
    p.color_count = 1;
    p.depth_write = false;
    p.premul = true;
    for (int two = 0; two < 2; two++) {
        p.cull = two ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        R.pbr_blend[two] = pipeline_create(&p);
    }

    memset(&p, 0, sizeof p);
    p.vs = lx_spv_fullscreen_vert; p.vs_size = sizeof lx_spv_fullscreen_vert;
    p.fs = lx_spv_probe_filter_frag; p.fs_size = sizeof lx_spv_probe_filter_frag;
    p.layout = R.filter_layout; p.color = R.hdr_format;
    R.probe_filter = pipeline_create(&p);

    memset(&p, 0, sizeof p);
    p.vs = lx_spv_fullscreen_vert; p.vs_size = sizeof lx_spv_fullscreen_vert;
    p.fs = lx_spv_ssr_trace_frag; p.fs_size = sizeof lx_spv_ssr_trace_frag;
    p.layout = R.ssr_pipe_layout; p.color = R.hdr_format;
    R.ssr_trace = pipeline_create(&p);

    memset(&p, 0, sizeof p);
    p.vs = lx_spv_fullscreen_vert; p.vs_size = sizeof lx_spv_fullscreen_vert;
    p.fs = lx_spv_ssr_composite_frag; p.fs_size = sizeof lx_spv_ssr_composite_frag;
    p.layout = R.ssr_pipe_layout; p.color = R.hdr_format; p.depth = R.depth_format; p.additive = true;
    R.ssr_composite = pipeline_create(&p);

    memset(&p, 0, sizeof p);
    p.vs = lx_spv_shadow_vert; p.vs_size = sizeof lx_spv_shadow_vert;
    p.layout = R.shadow_layout; p.vertex = VTX_MESH_POS_ONLY; p.depth = R.depth_format;
    p.depth_test = p.depth_write = true; p.depth_bias = true; p.cull = VK_CULL_MODE_NONE;
    R.shadow = pipeline_create(&p);
    {   /* see-through shadows: the material's colour map (set 0), the light's matrix + the material's alpha numbers */
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80};
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &R.material_layout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pr;
        VK_CHECK(vkCreatePipelineLayout(R.device, &li, NULL, &R.shadow_cut_layout));
    }
    p.vs = lx_spv_shadow_cut_vert; p.vs_size = sizeof lx_spv_shadow_cut_vert;
    p.fs = lx_spv_shadow_cut_frag; p.fs_size = sizeof lx_spv_shadow_cut_frag;
    p.layout = R.shadow_cut_layout; p.vertex = VTX_MESH;
    R.shadow_cut = pipeline_create(&p);

    memset(&p, 0, sizeof p);
    p.vs = lx_spv_sky_vert; p.vs_size = sizeof lx_spv_sky_vert;   /* at the far plane: only where no geometry is */
    p.fs = lx_spv_sky_frag; p.fs_size = sizeof lx_spv_sky_frag;
    p.layout = R.scene_only_layout; p.color = R.hdr_format; p.depth = R.depth_format;
    p.depth_test = true; p.compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    R.sky = pipeline_create(&p);
    p.fs = lx_spv_sky_mrt_frag; p.fs_size = sizeof lx_spv_sky_mrt_frag;
    p.color_count = 4;   /* writes "no reflection" and "no ambient light" into the extra targets */
    R.sky_mrt = pipeline_create(&p);

    memset(&p, 0, sizeof p);
    p.vs = lx_spv_precip_vert; p.vs_size = sizeof lx_spv_precip_vert;   /* rain and snow */
    p.fs = lx_spv_precip_frag; p.fs_size = sizeof lx_spv_precip_frag;
    p.layout = R.scene_only_layout; p.color = R.hdr_format; p.depth = R.depth_format;
    p.depth_test = true; p.blend = true; p.compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    R.precip = pipeline_create(&p);

    memset(&p, 0, sizeof p);
    p.vs = lx_spv_grid_vert; p.vs_size = sizeof lx_spv_grid_vert;
    p.fs = lx_spv_grid_frag; p.fs_size = sizeof lx_spv_grid_frag;
    p.layout = R.scene_only_layout; p.color = R.hdr_format; p.depth = R.depth_format;
    p.depth_test = true; p.blend = true; p.compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    R.grid = pipeline_create(&p);

    memset(&p, 0, sizeof p);
    p.vs = lx_spv_outline_vert; p.vs_size = sizeof lx_spv_outline_vert;
    p.fs = lx_spv_outline_frag; p.fs_size = sizeof lx_spv_outline_frag;
    p.layout = R.outline_layout; p.vertex = VTX_MESH_POS_NORMAL; p.color = R.hdr_format; p.depth = R.depth_format;
    p.depth_test = true; p.compare = VK_COMPARE_OP_LESS_OR_EQUAL; p.cull = VK_CULL_MODE_FRONT_BIT;
    R.outline = pipeline_create(&p);

    memset(&p, 0, sizeof p);
    p.vs = lx_spv_fullscreen_vert; p.vs_size = sizeof lx_spv_fullscreen_vert;
    p.fs = lx_spv_tonemap_frag; p.fs_size = sizeof lx_spv_tonemap_frag;
    p.layout = R.post_layout; p.color = VK_FORMAT_R8G8B8A8_UNORM;
    R.tonemap_ldr = pipeline_create(&p);
    /* ambient occlusion, bloom, sun shafts */
    p.fs = lx_spv_ao_frag; p.fs_size = sizeof lx_spv_ao_frag; p.color = VK_FORMAT_R8_UNORM;
    R.ao = pipeline_create(&p);
    p.fs = lx_spv_bloom_down_frag; p.fs_size = sizeof lx_spv_bloom_down_frag; p.color = R.hdr_format;
    R.bloom_down = pipeline_create(&p);
    p.fs = lx_spv_godrays_frag; p.fs_size = sizeof lx_spv_godrays_frag;
    R.godrays = pipeline_create(&p);
    p.fs = lx_spv_bloom_up_frag; p.fs_size = sizeof lx_spv_bloom_up_frag; p.additive = true;
    R.bloom_up = pipeline_create(&p);
    p.fs = lx_spv_ao_apply_frag; p.fs_size = sizeof lx_spv_ao_apply_frag; p.depth = R.depth_format;   /* inside the overlay pass */
    R.ao_apply = pipeline_create(&p);

    /* white + flat normal defaults */
    ImageDesc d = {1, 1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false, 0};
    uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255};
    image_create(&R.white, &d);
    image_upload_2d(&R.white, white, 4, false);
    image_create(&R.flat_normal, &d);
    image_upload_2d(&R.flat_normal, flat, 4, false);
    ImageDesc da = {1, 1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY, false, 0};
    image_create(&R.dummy_array, &da);   /* the terrain layer slots of ordinary materials */
    image_upload_2d(&R.dummy_array, white, 4, false);

    for (int f = 0; f < FRAMES; f++) {
        FrameData *fr = &R.frames[f];
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = R.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(R.device, &ai, &fr->cmd);
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(R.device, &fi, NULL, &fr->fence);
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(R.device, &si, NULL, &fr->acquired);
        buffer_create(&fr->ubo, 4096 * MAX_VIEWS_PER_FRAME, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        buffer_create(&fr->inst, R.inst_capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        buffer_create(&fr->ui_vb, 1 << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        buffer_create(&fr->ui_ib, 1 << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        fr->scene_set = alloc_set(R.scene_layout);
    }
    create_shadow_map(R.settings.shadow_resolution, R.settings.shadow_cascades);
    create_local_shadow(local_shadow_size(R.settings.shadow_resolution));
    create_gi_images(2, 2, 2);
    gc_create(0);
    lightmap_upload(NULL, 1, 0);
    bvol_upload(NULL, 1, 1, 1);
    write_scene_sets();
    create_dummy_cube();
    R.probe_set = alloc_set(R.probe_layout);
    R.probe_dummy_set = alloc_set(R.probe_layout);
    write_probe_set(R.probe_dummy_set, true);
    probes_create(R.settings.reflection_resolution > 0 ? R.settings.reflection_resolution : 128);
}

static void create_swap_pipelines(void) {
    if (R.tonemap_swap) vkDestroyPipeline(R.device, R.tonemap_swap, NULL);
    if (R.ui) vkDestroyPipeline(R.device, R.ui, NULL);
    PipeDesc p;
    memset(&p, 0, sizeof p);
    p.vs = lx_spv_fullscreen_vert; p.vs_size = sizeof lx_spv_fullscreen_vert;
    p.fs = lx_spv_tonemap_frag; p.fs_size = sizeof lx_spv_tonemap_frag;
    p.layout = R.post_layout; p.color = R.swap_format;
    R.tonemap_swap = pipeline_create(&p);
    memset(&p, 0, sizeof p);
    p.vs = lx_spv_ui_vert; p.vs_size = sizeof lx_spv_ui_vert;
    p.fs = lx_spv_ui_frag; p.fs_size = sizeof lx_spv_ui_frag;
    p.layout = R.ui_layout; p.vertex = VTX_UI; p.color = R.swap_format; p.blend = true;
    R.ui = pipeline_create(&p);
}

/* ------------------------------------------------------------------ public: lifetime */
bool lx_renderer_init(void) {
    if (R.ready) return true;
    LxGraphicsSettings keep = R.settings;
    memset(&R, 0, sizeof R);
    R.settings = keep.shadow_resolution ? keep : lx_graphics_defaults(2);
    R.vsync = R.settings.vsync;
    R.inst_capacity = 16u << 20;
    R.start_time = lx_time();
    if (!create_instance()) return false;
    if (!lx_platform_vk_create_surface(R.instance, &R.surface)) { LX_ERROR("Could not create a Vulkan surface"); return false; }
    R.surface_gen = lx_platform_surface_generation();
    if (!pick_device() || !create_device()) return false;
    if (!create_swapchain()) { LX_ERROR("Could not create the swapchain"); return false; }
    create_static_resources();
    create_swap_pipelines();
    lx_gi_init();
    R.ready = true;
    return true;
}

void lx_renderer_wait_idle(void) { if (R.device) vkDeviceWaitIdle(R.device); }

static void release_all_meshes_textures(void);

void lx_renderer_shutdown(void) {
    if (!R.device) return;
    vkDeviceWaitIdle(R.device);
    lx_assets_clear();
    release_all_meshes_textures();
    for (int i = 0; i < MAX_UI_TEXTURES; i++)
        if (R.ui_tex[i].used && R.ui_tex[i].owned) image_destroy(&R.ui_tex[i].img);
    if (R.backbuffer) lx_render_target_destroy(R.backbuffer);
    for (int f = 0; f < FRAMES; f++) {
        FrameData *fr = &R.frames[f];
        buffer_destroy(&fr->ubo); buffer_destroy(&fr->inst); buffer_destroy(&fr->ui_vb); buffer_destroy(&fr->ui_ib);
        buffer_destroy(&fr->gi_staging);
        vkDestroyFence(R.device, fr->fence, NULL);
        vkDestroySemaphore(R.device, fr->acquired, NULL);
    }
    image_destroy(&R.shadow_map);
    image_destroy(&R.local_shadow);
    for (int c = 0; c < 3; c++) image_destroy(&R.gc[c]);
    for (int c = 0; c < 2; c++) image_destroy(&R.gc_vis[c]);
    image_destroy(&R.gsdf);
    lm_comp_free();
    image_destroy(&R.lightmap);
    image_destroy(&R.bvol);
    for (int c = 0; c < 3; c++) image_destroy(&R.gi[c]);
    for (int c = 0; c < 2; c++) image_destroy(&R.gi_vis[c]);
    image_destroy(&R.white);
    image_destroy(&R.flat_normal);
    image_destroy(&R.dummy_array);
    for (int i = 0; i < R.terrain_set_count; i++) { image_destroy(&R.terrain_sets[i].a); image_destroy(&R.terrain_sets[i].b); }
    lx_free(R.terrain_sets);
    probes_destroy();
    image_destroy(&R.dummy_cube);
    VkPipeline pipes[] = {R.pbr, R.shadow, R.sky, R.grid, R.outline, R.tonemap_ldr, R.tonemap_swap, R.ui, R.precip,
                          R.pbr_mrt, R.sky_mrt, R.pbr_probe, R.probe_filter, R.ssr_trace, R.ssr_composite,
                          R.ao, R.ao_apply, R.bloom_down, R.bloom_up, R.godrays, R.pbr_blend[0], R.pbr_blend[1], R.shadow_cut};
    for (size_t i = 0; i < sizeof pipes / sizeof pipes[0]; i++) if (pipes[i]) vkDestroyPipeline(R.device, pipes[i], NULL);
    for (int k = 0; k < 3; k++)
        for (int c = 0; c < 2; c++)
            for (int t = 0; t < 2; t++)
                if ((c || t) && R.pbr_var[k][c][t]) vkDestroyPipeline(R.device, R.pbr_var[k][c][t], NULL);
    VkPipelineLayout layouts[] = {R.pbr_layout, R.shadow_layout, R.scene_only_layout, R.outline_layout, R.tonemap_layout, R.ui_layout,
                                  R.ssr_pipe_layout, R.filter_layout, R.post_layout, R.shadow_cut_layout};
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) vkDestroyPipelineLayout(R.device, layouts[i], NULL);
    VkDescriptorSetLayout set_layouts[] = {R.scene_layout, R.material_layout, R.tex_layout, R.probe_layout, R.ssr_layout, R.post_set_layout};
    for (size_t i = 0; i < sizeof set_layouts / sizeof set_layouts[0]; i++) vkDestroyDescriptorSetLayout(R.device, set_layouts[i], NULL);
    vkDestroyDescriptorPool(R.device, R.desc_pool, NULL);
    VkSampler samplers[] = {R.linear_repeat, R.linear_clamp, R.shadow_sampler, R.point_clamp, R.cube_sampler};
    for (size_t i = 0; i < sizeof samplers / sizeof samplers[0]; i++) vkDestroySampler(R.device, samplers[i], NULL);
    destroy_swapchain();
    vkDestroyCommandPool(R.device, R.pool, NULL);
    vkDestroyDevice(R.device, NULL);
    vkDestroySurfaceKHR(R.instance, R.surface, NULL);
    if (R.messenger) vkDestroyDebugUtilsMessengerEXT(R.instance, R.messenger, NULL);
    vkDestroyInstance(R.instance, NULL);
    lx_free(R.mat_sets);
    for (int i = 0; i < LOCAL_SHADOW_LIGHTS; i++) { lx_free(R.local_lists[i].batches); lx_free(R.local_lists[i].inst); lx_free(R.local_lists[i].item_batch); }
    for (int i = 0; i < MAX_CASCADES; i++) { lx_free(R.shadow_lists[i].batches); lx_free(R.shadow_lists[i].inst); lx_free(R.shadow_lists[i].item_batch); }
    lx_free(R.main_list.batches); lx_free(R.main_list.inst); lx_free(R.main_list.item_batch);
    lx_free(R.trans_list.batches); lx_free(R.trans_list.inst); lx_free(R.trans_list.item_batch);
    lx_free(R.precip_list.batches); lx_free(R.precip_list.inst); lx_free(R.precip_list.item_batch);
    lx_free(R.probe_list.batches); lx_free(R.probe_list.inst); lx_free(R.probe_list.item_batch);
    lx_gi_shutdown();
    memset(&R, 0, sizeof R);
}

LxGraphicsSettings *lx_graphics_settings(void) {
    if (!R.settings.shadow_resolution) R.settings = lx_graphics_defaults(2);
    return &R.settings;
}
const LxRenderStats *lx_render_stats(void) { return &R.stats; }
void lx_renderer_set_vsync(bool on) { if (R.vsync != on) { R.vsync = on; R.swap_dirty = true; } }

/* ------------------------------------------------------------------ meshes & textures */
typedef struct LiveMesh { LxMesh *m; } LiveMesh;
static LxMesh **g_live_meshes; static int g_live_mesh_count, g_live_mesh_cap;
static LxTexture **g_live_tex; static int g_live_tex_count, g_live_tex_cap;

static GpuBuffer upload_buffer(const void *data, VkDeviceSize size, VkBufferUsageFlags usage) {
    GpuBuffer dst, staging;
    buffer_create(&dst, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    buffer_create(&staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(staging.mapped, data, (size_t)size);
    VkCommandBuffer cmd = one_shot_begin();
    VkBufferCopy c = {0, 0, size};
    vkCmdCopyBuffer(cmd, staging.buf, dst.buf, 1, &c);
    one_shot_end(cmd);
    buffer_destroy(&staging);
    return dst;
}

/* the mesh an object draws: its deformed copy when skinned */
static LxMesh *entity_mesh(LxEntityData *d) { return d->skin ? lx_skin_mesh_of(d) : lx_asset_mesh(d->mesh.mesh); }

static MeshGpu *mesh_gpu(LxMesh *m) {
    if (m->gpu) {
        MeshGpu *g = m->gpu;
        if (g->dynamic) {
            /* this frame's copy of the deformed vertices */
            int f = R.frame;
            size_t bytes = sizeof(LxVertex) * (size_t)m->lods[0].vertex_count;
            if (!g->dvb[f].buf) { buffer_create(&g->dvb[f], bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true); g->dver[f] = m->version - 1; }
            if (g->dver[f] != m->version) { memcpy(g->dvb[f].mapped, m->lods[0].vertices, bytes); g->dver[f] = m->version; }
            g->vb[0] = g->dvb[f];
        }
        return g;
    }
    MeshGpu *g = lx_calloc(1, sizeof *g);
    if (m->dynamic) {
        g->dynamic = true;
        g->ib[0] = upload_buffer(m->lods[0].indices, sizeof(uint32_t) * (VkDeviceSize)m->lods[0].index_count, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        m->gpu = g;
        LX_VEC_PUSH(g_live_meshes, g_live_mesh_count, g_live_mesh_cap, m);
        return mesh_gpu(m);
    }
    for (int l = 0; l < m->lod_count; l++) {
        g->vb[l] = upload_buffer(m->lods[l].vertices, sizeof(LxVertex) * (VkDeviceSize)m->lods[l].vertex_count, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        g->ib[l] = upload_buffer(m->lods[l].indices, sizeof(uint32_t) * (VkDeviceSize)m->lods[l].index_count, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }
    m->gpu = g;
    LX_VEC_PUSH(g_live_meshes, g_live_mesh_count, g_live_mesh_cap, m);
    return g;
}

static void mesh_gpu_destroy(MeshGpu *g) {
    if (g->dynamic) {
        for (int f = 0; f < FRAMES; f++) buffer_destroy(&g->dvb[f]);
        buffer_destroy(&g->ib[0]);
    } else
        for (int l = 0; l < LX_MAX_LODS; l++) { buffer_destroy(&g->vb[l]); buffer_destroy(&g->ib[l]); }
    lx_free(g);
}

/* GPU buffers of meshes freed while frames may still use them: the frame being recorded right now
 * (an object deleted from a button click, Stop restoring the scene) and the frames in flight.
 * They're destroyed a few frames later, once nothing can reference them. */
typedef struct MeshGarbage { MeshGpu *g; int frames_left; } MeshGarbage;
static MeshGarbage *g_mesh_garbage;
static int g_mesh_garbage_count, g_mesh_garbage_cap;

static void mesh_garbage_collect(bool all) {
    int k = 0;
    for (int i = 0; i < g_mesh_garbage_count; i++) {
        MeshGarbage *mg = &g_mesh_garbage[i];
        if (all || --mg->frames_left <= 0) mesh_gpu_destroy(mg->g);
        else g_mesh_garbage[k++] = *mg;
    }
    g_mesh_garbage_count = k;
}

void lx_renderer_mesh_release(LxMesh *m) {
    if (!m || !m->gpu || !R.device) return;
    MeshGpu *g = m->gpu;
    m->gpu = NULL;
    for (int i = 0; i < g_live_mesh_count; i++) if (g_live_meshes[i] == m) { g_live_meshes[i] = g_live_meshes[--g_live_mesh_count]; break; }
    MeshGarbage mg = {g, FRAMES + 1};
    LX_VEC_PUSH(g_mesh_garbage, g_mesh_garbage_count, g_mesh_garbage_cap, mg);
}

static GpuImage *texture_gpu(LxTexture *t, bool srgb) {
    TexGpu *g = t->gpu;
    if (!g) {
        g = lx_calloc(1, sizeof *g);
        t->gpu = g;
        LX_VEC_PUSH(g_live_tex, g_live_tex_count, g_live_tex_cap, t);
    }
    int k = srgb ? 1 : 0;
    if (g->has[k]) return &g->img[k];
    VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t mips = 1;
    bool can_blit = format_supports(fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if (can_blit) for (uint32_t sz = (uint32_t)(t->width > t->height ? t->width : t->height); sz > 1; sz >>= 1) mips++;
    ImageDesc d = {(uint32_t)t->width, (uint32_t)t->height, 1, 1, mips, fmt,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false};
    image_create(&g->img[k], &d);
    image_upload_2d(&g->img[k], t->pixels, (size_t)t->width * (size_t)t->height * 4, mips > 1);
    g->has[k] = true;
    return &g->img[k];
}

void lx_renderer_texture_release(LxTexture *t) {
    if (!t || !t->gpu || !R.device) return;
    vkDeviceWaitIdle(R.device);
    for (int i = 0; i < R.mat_set_count; i++) {
        bool uses = false;
        for (int k = 0; k < MAT_TEXTURES; k++) uses |= R.mat_sets[i].tex[k] == t;
        if (uses) {
            vkFreeDescriptorSets(R.device, R.desc_pool, 1, &R.mat_sets[i].set);
            R.mat_sets[i--] = R.mat_sets[--R.mat_set_count];
        }
    }
    TexGpu *g = t->gpu;
    for (int k = 0; k < 2; k++) if (g->has[k]) image_destroy(&g->img[k]);
    lx_free(g);
    t->gpu = NULL;
    for (int i = 0; i < g_live_tex_count; i++) if (g_live_tex[i] == t) { g_live_tex[i] = g_live_tex[--g_live_tex_count]; break; }
}

static void release_all_meshes_textures(void) {
    while (g_live_mesh_count) lx_renderer_mesh_release(g_live_meshes[0]);
    vkDeviceWaitIdle(R.device);
    mesh_garbage_collect(true);
    while (g_live_tex_count) lx_renderer_texture_release(g_live_tex[0]);
}

/* a terrain's material: its layers' textures as two arrays (colour + roughness; normal, height, ao) */
static VkDescriptorSet terrain_set(const LxMaterial *m, const LxTerrainGpuLayers *tl) {
    struct TerrainSet *ts = NULL;
    for (int i = 0; i < R.terrain_set_count; i++) if (R.terrain_sets[i].mat == m) { ts = &R.terrain_sets[i]; break; }
    if (ts && ts->version == tl->version) return ts->set;
    if (!ts) {
        struct TerrainSet n;
        memset(&n, 0, sizeof n);
        n.mat = m;
        LX_VEC_PUSH(R.terrain_sets, R.terrain_set_count, R.terrain_set_cap, n);
        ts = &R.terrain_sets[R.terrain_set_count - 1];
    } else {   /* the layers changed: new images (rare: editing the layers) */
        vkDeviceWaitIdle(R.device);
        image_destroy(&ts->a);
        image_destroy(&ts->b);
        if (ts->set) vkFreeDescriptorSets(R.device, R.desc_pool, 1, &ts->set);
    }
    ts->version = tl->version;
    uint32_t mips = 1;
    for (int k = tl->size; k > 1; k >>= 1) mips++;
    VkFormat fa = VK_FORMAT_R8G8B8A8_SRGB, fb = VK_FORMAT_R8G8B8A8_UNORM;
    bool blit = format_supports(fa, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if (!blit) mips = 1;
    ImageDesc d = {(uint32_t)tl->size, (uint32_t)tl->size, 1, (uint32_t)tl->count, mips, fa,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   VK_IMAGE_VIEW_TYPE_2D_ARRAY, false, 0};
    size_t bytes = (size_t)tl->count * tl->size * tl->size * 4;
    image_create(&ts->a, &d);
    image_upload_2d(&ts->a, tl->a, bytes, mips > 1);
    d.fmt = fb;
    image_create(&ts->b, &d);
    image_upload_2d(&ts->b, tl->b, bytes, mips > 1);
    ts->set = alloc_set(R.material_layout);
    for (int k = 0; k < MAT_TEXTURES; k++)
        write_image(ts->set, (uint32_t)k, k == 1 ? R.flat_normal.view : R.white.view, R.linear_repeat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_image(ts->set, MAT_TEXTURES, ts->a.view, R.linear_repeat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_image(ts->set, MAT_TEXTURES + 1, ts->b.view, R.linear_repeat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return ts->set;
}

static VkDescriptorSet material_set(const LxMaterial *m) {
    const LxTerrainGpuLayers *tl = lx_terrain_gpu_layers(m);
    if (tl) return terrain_set(m, tl);
    const char *refs[MAT_TEXTURES] = {m->albedo_map, m->normal_map, m->roughness_map, m->metallic_map, m->ao_map, m->height_map};
    LxTexture *tex[MAT_TEXTURES];
    for (int k = 0; k < MAT_TEXTURES; k++) tex[k] = refs[k][0] ? lx_asset_texture(refs[k]) : NULL;
    for (int i = 0; i < R.mat_set_count; i++)
        if (!memcmp(R.mat_sets[i].tex, tex, sizeof tex)) return R.mat_sets[i].set;
    MatSet ms;
    memcpy(ms.tex, tex, sizeof tex);
    ms.set = alloc_set(R.material_layout);
    for (int k = 0; k < MAT_TEXTURES; k++) {
        VkImageView fallback = k == 1 ? R.flat_normal.view : R.white.view;
        VkImageView view = tex[k] ? texture_gpu(tex[k], k == 0)->view : fallback;   /* only albedo is colour */
        write_image(ms.set, (uint32_t)k, view, R.linear_repeat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    write_image(ms.set, MAT_TEXTURES, R.dummy_array.view, R.linear_repeat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_image(ms.set, MAT_TEXTURES + 1, R.dummy_array.view, R.linear_repeat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    LX_VEC_PUSH(R.mat_sets, R.mat_set_count, R.mat_set_cap, ms);
    return ms.set;
}

/* ------------------------------------------------------------------ UI textures */
static uint32_t ui_tex_register(GpuImage *img, bool owned, int mode) {
    for (uint32_t i = 0; i < MAX_UI_TEXTURES; i++) {
        if (R.ui_tex[i].used) continue;
        UiTex *t = &R.ui_tex[i];
        t->used = true;
        t->owned = owned;
        t->mode = mode;
        if (img) t->img = *img;
        t->set = alloc_set(R.tex_layout);
        write_image(t->set, 0, t->img.view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return i;
    }
    LX_ERROR("UI texture limit reached");
    return 0;
}

void lx_renderer_set_font_atlas(const uint8_t *alpha, int w, int h) {
    vkDeviceWaitIdle(R.device);
    UiTex *t = &R.ui_tex[0];
    if (t->used) {
        image_destroy(&t->img);
        vkFreeDescriptorSets(R.device, R.desc_pool, 1, &t->set);
        t->used = false;
    }
    ImageDesc d = {(uint32_t)w, (uint32_t)h, 1, 1, 1, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false};
    GpuImage img;
    image_create(&img, &d);
    image_upload_2d(&img, alpha, (size_t)w * (size_t)h, false);
    t->used = true;
    t->owned = true;
    t->mode = 0;
    t->img = img;
    t->set = alloc_set(R.tex_layout);
    write_image(t->set, 0, img.view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

uint32_t lx_ui_texture_from_rgba(const uint8_t *rgba, int w, int h) {
    ImageDesc d = {(uint32_t)w, (uint32_t)h, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false};
    GpuImage img;
    image_create(&img, &d);
    image_upload_2d(&img, rgba, (size_t)w * (size_t)h * 4, false);
    /* slot 0 is reserved for the font */
    if (!R.ui_tex[0].used) { R.ui_tex[0].used = true; R.ui_tex[0].set = VK_NULL_HANDLE; }
    return ui_tex_register(&img, true, 1);
}

uint32_t lx_ui_white_texture(void) {
    static uint32_t id;
    if (!id && R.ready) {
        if (!R.ui_tex[0].used) { R.ui_tex[0].used = true; R.ui_tex[0].set = VK_NULL_HANDLE; }
        id = ui_tex_register(&R.white, false, 1);
    }
    return id;
}

void lx_ui_texture_free(uint32_t id) {
    if (id == 0 || id >= MAX_UI_TEXTURES || !R.ui_tex[id].used) return;
    vkDeviceWaitIdle(R.device);
    UiTex *t = &R.ui_tex[id];
    if (t->owned) image_destroy(&t->img);
    vkFreeDescriptorSets(R.device, R.desc_pool, 1, &t->set);
    memset(t, 0, sizeof *t);
}

/* ------------------------------------------------------------------ render targets */
static void post_set(VkDescriptorSet set, VkImageView a, VkSampler sa, VkImageLayout la, VkImageView b, VkSampler sb, VkImageLayout lb,
                     VkImageView c, VkSampler sc, VkImageLayout lc) {
    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    write_image(set, 0, a ? a : R.white.view, a ? sa : R.linear_clamp, a ? la : RO);
    write_image(set, 1, b ? b : R.white.view, b ? sb : R.linear_clamp, b ? lb : RO);
    write_image(set, 2, c ? c : R.white.view, c ? sc : R.linear_clamp, c ? lc : RO);
    write_image(set, 3, R.white.view, R.linear_clamp, RO);
}
/* the pictures after the main pass: ambient light, occlusion, sun shafts, the bloom levels, and their input sets */
static void post_alloc(LxRenderTarget *rt) {
    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, DRO = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ImageDesc d = {(uint32_t)rt->iw, (uint32_t)rt->ih, 1, 1, 1, R.gamb_format, use, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false, 0};
    image_create(&rt->gamb, &d);
    d.w = (uint32_t)rt->sw; d.h = (uint32_t)rt->sh; d.fmt = VK_FORMAT_R8_UNORM;
    image_create(&rt->ao, &d);
    d.fmt = R.hdr_format;
    image_create(&rt->rays, &d);
    int w = rt->iw, h = rt->ih;
    rt->bloom_n = 0;
    for (int i = 0; i < BLOOM_LEVELS; i++) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        rt->bw[i] = w; rt->bh[i] = h;
        d.w = (uint32_t)w; d.h = (uint32_t)h;
        image_create(&rt->bloom[i], &d);
        rt->bloom_n++;
        if (w <= 4 || h <= 4) break;
    }
    rt->ao_set = alloc_set(R.post_set_layout);
    post_set(rt->ao_set, rt->depth.view, R.point_clamp, DRO, rt->gnormal.view, R.point_clamp, RO, 0, 0, 0);
    rt->ao_apply_set = alloc_set(R.post_set_layout);
    post_set(rt->ao_apply_set, rt->ao.view, R.linear_clamp, RO, rt->depth.view, R.point_clamp, DRO, rt->gamb.view, R.point_clamp, RO);
    rt->rays_set = alloc_set(R.post_set_layout);
    post_set(rt->rays_set, rt->hdr.view, R.linear_clamp, RO, rt->depth.view, R.point_clamp, DRO, 0, 0, 0);
    rt->tone_set = alloc_set(R.post_set_layout);
    post_set(rt->tone_set, rt->hdr.view, R.linear_clamp, RO, rt->bloom[0].view, R.linear_clamp, RO, rt->rays.view, R.linear_clamp, RO);
    for (int i = 0; i < rt->bloom_n; i++) {
        rt->bloom_down_set[i] = alloc_set(R.post_set_layout);
        post_set(rt->bloom_down_set[i], i ? rt->bloom[i - 1].view : rt->hdr.view, R.linear_clamp, RO, 0, 0, 0, 0, 0, 0);
        rt->bloom_up_set[i] = alloc_set(R.post_set_layout);
        post_set(rt->bloom_up_set[i], rt->bloom[i].view, R.linear_clamp, RO, 0, 0, 0, 0, 0, 0);
    }
    /* sampled before their first pass writes them (the final image reads shafts and bloom even when they're off) */
    VkCommandBuffer cmd = one_shot_begin();
    GpuImage *all[3 + BLOOM_LEVELS] = {&rt->ao, &rt->rays, &rt->gamb};
    int n = 3;
    for (int i = 0; i < rt->bloom_n; i++) all[n++] = &rt->bloom[i];
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (int i = 0; i < n; i++) {
        image_barrier(cmd, all[i]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1, 1);
        vkCmdClearColorImage(cmd, all[i]->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        image_barrier(cmd, all[i]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RO, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
    }
    one_shot_end(cmd);
}
static void post_free(LxRenderTarget *rt) {
    image_destroy(&rt->gamb);
    image_destroy(&rt->ao);
    image_destroy(&rt->rays);
    for (int i = 0; i < rt->bloom_n; i++) {
        image_destroy(&rt->bloom[i]);
        VkDescriptorSet two[2] = {rt->bloom_down_set[i], rt->bloom_up_set[i]};
        for (int k = 0; k < 2; k++) if (two[k]) vkFreeDescriptorSets(R.device, R.desc_pool, 1, &two[k]);
        rt->bloom_down_set[i] = rt->bloom_up_set[i] = VK_NULL_HANDLE;
    }
    rt->bloom_n = 0;
    VkDescriptorSet sets[4] = {rt->ao_set, rt->ao_apply_set, rt->rays_set, rt->tone_set};
    for (int i = 0; i < 4; i++) if (sets[i]) vkFreeDescriptorSets(R.device, R.desc_pool, 1, &sets[i]);
    rt->ao_set = rt->ao_apply_set = rt->rays_set = rt->tone_set = VK_NULL_HANDLE;
}
/* one full-screen pass into `dst` (w x h): load keeps what's there (for adding) */
static void post_pass(VkCommandBuffer cmd, GpuImage *dst, int w, int h, bool load, VkPipeline pipe, VkDescriptorSet scene, uint32_t ubo_off,
                      VkDescriptorSet in, const float pc[16]) {
    image_barrier(cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, load ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, 0, 1, 1);
    VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = dst->view;
    a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent.width = (uint32_t)w;
    ri.renderArea.extent.height = (uint32_t)h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &a;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDescriptorSet sets[2] = {scene, in};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.post_layout, 0, 2, sets, 1, &ubo_off);
    vkCmdPushConstants(cmd, R.post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
}
static void target_alloc(LxRenderTarget *rt, bool with_ldr) {
    float scale = lx_clampf(R.settings.render_scale, 0.25f, 2.0f);
    rt->iw = (int)((float)rt->w * scale); if (rt->iw < 1) rt->iw = 1;
    rt->ih = (int)((float)rt->h * scale); if (rt->ih < 1) rt->ih = 1;
    ImageDesc hd = {(uint32_t)rt->iw, (uint32_t)rt->ih, 1, 1, 1, R.hdr_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false};
    image_create(&rt->hdr, &hd);
    ImageDesc dd = {(uint32_t)rt->iw, (uint32_t)rt->ih, 1, 1, 1, R.depth_format,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_VIEW_TYPE_2D, false, 0};
    image_create(&rt->depth, &dd);
    rt->hdr_set = alloc_set(R.tex_layout);
    write_image(rt->hdr_set, 0, rt->hdr.view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    /* screen-space reflection buffers */
    ImageDesc gn = {(uint32_t)rt->iw, (uint32_t)rt->ih, 1, 1, 1, R.gnormal_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false, 0};
    image_create(&rt->gnormal, &gn);
    gn.fmt = R.gspec_format;
    image_create(&rt->gspec, &gn);
    rt->sw = rt->iw > 1 ? rt->iw / 2 : 1;
    rt->sh = rt->ih > 1 ? rt->ih / 2 : 1;
    ImageDesc sr = {(uint32_t)rt->sw, (uint32_t)rt->sh, 1, 1, 1, R.hdr_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false, 0};
    image_create(&rt->ssr, &sr);
    rt->ssr_trace_set = alloc_set(R.ssr_layout);
    rt->ssr_comp_set = alloc_set(R.ssr_layout);
    VkDescriptorSet both[2] = {rt->ssr_trace_set, rt->ssr_comp_set};
    for (int i = 0; i < 2; i++) {
        write_image(both[i], 1, rt->depth.view, R.point_clamp, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        write_image(both[i], 2, rt->gnormal.view, R.point_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        write_image(both[i], 3, rt->gspec.view, R.point_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    write_image(rt->ssr_trace_set, 0, rt->hdr.view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_image(rt->ssr_comp_set, 4, rt->ssr.view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    post_alloc(rt);
    if (with_ldr) {
        ImageDesc ld = {(uint32_t)rt->w, (uint32_t)rt->h, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false};
        image_create(&rt->ldr, &ld);
        /* start in SHADER_READ so the UI can sample it before the first render */
        VkCommandBuffer cmd = one_shot_begin();
        image_barrier(cmd, rt->ldr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
        one_shot_end(cmd);
        if (!R.ui_tex[0].used) { R.ui_tex[0].used = true; R.ui_tex[0].set = VK_NULL_HANDLE; }
        if (rt->ui_tex) {
            UiTex *t = &R.ui_tex[rt->ui_tex];
            t->img = rt->ldr;
            write_image(t->set, 0, rt->ldr.view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        } else rt->ui_tex = ui_tex_register(&rt->ldr, false, 1);
    }
}

static void target_free(LxRenderTarget *rt) {
    post_free(rt);
    image_destroy(&rt->hdr);
    image_destroy(&rt->depth);
    image_destroy(&rt->ldr);
    image_destroy(&rt->gnormal);
    image_destroy(&rt->gspec);
    image_destroy(&rt->ssr);
    VkDescriptorSet sets[3] = {rt->hdr_set, rt->ssr_trace_set, rt->ssr_comp_set};
    for (int i = 0; i < 3; i++)
        if (sets[i]) vkFreeDescriptorSets(R.device, R.desc_pool, 1, &sets[i]);
    rt->hdr_set = rt->ssr_trace_set = rt->ssr_comp_set = VK_NULL_HANDLE;
}

LxRenderTarget *lx_render_target_create(int w, int h) {
    LxRenderTarget *rt = lx_calloc(1, sizeof *rt);
    rt->w = w > 0 ? w : 1;
    rt->h = h > 0 ? h : 1;
    target_alloc(rt, true);
    return rt;
}

void lx_render_target_resize(LxRenderTarget *rt, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    float scale = lx_clampf(R.settings.render_scale, 0.25f, 2.0f);
    if (rt->w == w && rt->h == h && rt->iw == (int)((float)w * scale)) return;
    vkDeviceWaitIdle(R.device);
    bool ldr = rt->ldr.img != VK_NULL_HANDLE;
    target_free(rt);
    rt->w = w;
    rt->h = h;
    target_alloc(rt, ldr);
}

void lx_render_target_destroy(LxRenderTarget *rt) {
    if (!rt) return;
    vkDeviceWaitIdle(R.device);
    if (rt->ui_tex) {
        UiTex *t = &R.ui_tex[rt->ui_tex];
        vkFreeDescriptorSets(R.device, R.desc_pool, 1, &t->set);
        memset(t, 0, sizeof *t);
    }
    target_free(rt);
    lx_free(rt);
}

uint32_t lx_render_target_texture(const LxRenderTarget *rt) { return rt ? rt->ui_tex : 0; }
/* thumbnails: views drawn in preview mode leave GI, lightmaps and reflection probes to the real views */
void lx_renderer_set_preview(bool on) { R.preview = on; }
bool lx_renderer_can_draw_view(void) { return R.frame_active && R.frames[R.frame].ubo_used < MAX_VIEWS_PER_FRAME; }

/* ------------------------------------------------------------------ frame */
bool lx_renderer_begin_frame(void) {
    if (!R.ready) return false;
    R.frame_active = false;
    int w, h;
    lx_window_size(&w, &h);
    if (w <= 0 || h <= 0) return false;
    if (lx_platform_surface_generation() != R.surface_gen) {
        /* Android recreated the native window: new surface + swapchain */
        vkDeviceWaitIdle(R.device);
        destroy_swapchain();
        vkDestroySurfaceKHR(R.instance, R.surface, NULL);
        if (!lx_platform_vk_create_surface(R.instance, &R.surface)) return false;
        R.surface_gen = lx_platform_surface_generation();
        R.swap_dirty = true;
    }
    FrameData *fr = &R.frames[R.frame];
    vkWaitForFences(R.device, 1, &fr->fence, VK_TRUE, UINT64_MAX);
    if (g_mesh_garbage_count) mesh_garbage_collect(false);
    if (fr->inst_overflow) {
        /* grow the instance stream (safe now: this frame's GPU work is done) */
        R.inst_capacity *= 2;
        for (int f = 0; f < FRAMES; f++) {
            vkWaitForFences(R.device, 1, &R.frames[f].fence, VK_TRUE, UINT64_MAX);
        }
        vkDeviceWaitIdle(R.device);
        for (int f = 0; f < FRAMES; f++) {
            buffer_destroy(&R.frames[f].inst);
            buffer_create(&R.frames[f].inst, R.inst_capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
            R.frames[f].inst_overflow = false;
        }
        LX_INFO("Instance buffer grown to %u MB", (unsigned)(R.inst_capacity >> 20));
    }
    if (R.swap_dirty || R.swap_extent.width != (uint32_t)w || R.swap_extent.height != (uint32_t)h) {
        vkDeviceWaitIdle(R.device);
        VkFormat old = R.swap_format;
        if (!create_swapchain()) return false;
        if (old != R.swap_format) create_swap_pipelines();
    }
    VkResult r = vkAcquireNextImageKHR(R.device, R.swapchain, UINT64_MAX, fr->acquired, VK_NULL_HANDLE, &R.image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { R.swap_dirty = true; return false; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LX_ERROR("vkAcquireNextImageKHR: %s", lx_vk_result(r)); return false; }
    if (r == VK_SUBOPTIMAL_KHR) R.swap_dirty = true;
    vkResetFences(R.device, 1, &fr->fence);
    vkResetCommandBuffer(fr->cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(fr->cmd, &bi);
    fr->ubo_used = 0;
    fr->inst_used = 0;
    R.swap_written = false;
    R.frame_active = true;
    R.frame_serial++;
    memset(&R.stats, 0, sizeof R.stats);
    return true;
}

static void begin_swap_pass(VkCommandBuffer cmd, bool clear, LxVec4 clear_color) {
    if (!R.swap_written) {
        image_barrier(cmd, R.swap_images[R.image_index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, 1, 1);
    }
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = R.swap_views[R.image_index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = (!R.swap_written && clear) ? VK_ATTACHMENT_LOAD_OP_CLEAR : (R.swap_written ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[0] = clear_color.x;
    ca.clearValue.color.float32[1] = clear_color.y;
    ca.clearValue.color.float32[2] = clear_color.z;
    ca.clearValue.color.float32[3] = 1.0f;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = R.swap_extent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    R.swap_written = true;
    VkViewport vp = {0, 0, (float)R.swap_extent.width, (float)R.swap_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, R.swap_extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

static void write_ppm_from(const uint8_t *px, uint32_t w, uint32_t h, bool bgra, const char *path) {
    size_t n = (size_t)w * h;
    uint8_t *rgb = lx_alloc(n * 3);
    for (size_t i = 0; i < n; i++) {
        rgb[i * 3 + 0] = px[i * 4 + (bgra ? 2 : 0)];
        rgb[i * 3 + 1] = px[i * 4 + 1];
        rgb[i * 3 + 2] = px[i * 4 + (bgra ? 0 : 2)];
    }
    char header[64];
    int hl = snprintf(header, sizeof header, "P6\n%u %u\n255\n", w, h);
    uint8_t *file = lx_alloc((size_t)hl + n * 3);
    memcpy(file, header, (size_t)hl);
    memcpy(file + hl, rgb, n * 3);
    lx_file_write(path, file, (size_t)hl + n * 3);
    lx_free(file);
    lx_free(rgb);
}

void lx_renderer_end_frame(void) {
    if (!R.frame_active) return;
    FrameData *fr = &R.frames[R.frame];
    VkCommandBuffer cmd = fr->cmd;
    if (!R.swap_written) {
        begin_swap_pass(cmd, true, lx_v4(0, 0, 0, 1));
        vkCmdEndRendering(cmd);
    }
    GpuBuffer shot;
    memset(&shot, 0, sizeof shot);
    bool want_shot = R.screenshot_path[0] && R.swap_transfer_src;
    if (want_shot) {
        buffer_create(&shot, (VkDeviceSize)R.swap_extent.width * R.swap_extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        image_barrier(cmd, R.swap_images[R.image_index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0, 1, 1);
        VkBufferImageCopy region;
        memset(&region, 0, sizeof region);
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = R.swap_extent.width;
        region.imageExtent.height = R.swap_extent.height;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, R.swap_images[R.image_index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buf, 1, &region);
        image_barrier(cmd, R.swap_images[R.image_index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 1, 1);
    } else {
        image_barrier(cmd, R.swap_images[R.image_index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 1, 1);
    }
    vkEndCommandBuffer(cmd);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &fr->acquired;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &R.swap_done[R.image_index];
    VK_CHECK(vkQueueSubmit(R.queue, 1, &si, fr->fence));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &R.swap_done[R.image_index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &R.swapchain;
    pi.pImageIndices = &R.image_index;
    VkResult r = vkQueuePresentKHR(R.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) R.swap_dirty = true;
    if (want_shot) {
        vkWaitForFences(R.device, 1, &fr->fence, VK_TRUE, UINT64_MAX);
        bool bgra = R.swap_format == VK_FORMAT_B8G8R8A8_UNORM || R.swap_format == VK_FORMAT_B8G8R8A8_SRGB;
        write_ppm_from(shot.mapped, R.swap_extent.width, R.swap_extent.height, bgra, R.screenshot_path);
        LX_INFO("Screenshot saved to %s", R.screenshot_path);
        buffer_destroy(&shot);
        R.screenshot_path[0] = 0;
    }
    R.frame = (R.frame + 1) % FRAMES;
    R.frame_active = false;
    R.stats.frame_ms = 0;
}

bool lx_renderer_screenshot(const char *path) {
    if (!R.swap_transfer_src) { LX_WARN("Swapchain does not support readback on this device"); return false; }
    lx_strcpy(R.screenshot_path, sizeof R.screenshot_path, path);
    return true;
}

/* ------------------------------------------------------------------ draw lists */
static void dl_reset(DrawList *dl) { dl->nb = 0; dl->ni = 0; }

static void dl_add_lm(DrawList *dl, LxMesh *mesh, int lod, const LxMaterial *mat, const float *model, LxVec4 tint, const float *lm);
static void dl_add(DrawList *dl, LxMesh *mesh, int lod, const LxMaterial *mat, const float *model, LxVec4 tint) {
    dl_add_lm(dl, mesh, lod, mat, model, tint, NO_LIGHTMAP);
}
static float g_dl_key = -1;   /* >= 0: the next item gets a batch of its own with this sort key (see-through, by distance) */
static void dl_add_lm(DrawList *dl, LxMesh *mesh, int lod, const LxMaterial *mat, const float *model, LxVec4 tint, const float *lm) {
    int b = -1;
    if (g_dl_key < 0)
        for (int i = dl->nb - 1; i >= 0; i--)
            if (dl->batches[i].mesh == mesh && dl->batches[i].lod == lod && dl->batches[i].mat == mat) { b = i; break; }
    if (b < 0) {
        Batch nb = {mesh, lod, mat, 0, 0, g_dl_key};
        LX_VEC_PUSH(dl->batches, dl->nb, dl->cb, nb);
        b = dl->nb - 1;
    }
    dl->batches[b].count++;
    if (dl->ni >= dl->ci) {
        dl->ci = dl->ci ? dl->ci * 2 : 1024;
        dl->inst = lx_realloc(dl->inst, INST_BYTES * (size_t)dl->ci);
        dl->item_batch = lx_realloc(dl->item_batch, sizeof(int) * (size_t)dl->ci);
    }
    float *dst = dl->inst + (size_t)dl->ni * INST_FLOATS;
    memcpy(dst, model, 64);
    dst[16] = tint.x; dst[17] = tint.y; dst[18] = tint.z; dst[19] = tint.w;
    memcpy(dst + 20, lm, 16);
    dl->item_batch[dl->ni++] = b;
}

/* Group instances by batch into the frame's instance stream. Returns false if it does not fit. */
static bool dl_upload(DrawList *dl, FrameData *fr) {
    VkDeviceSize need = (VkDeviceSize)dl->ni * INST_BYTES;
    if (fr->inst_used + need > fr->inst.size) { fr->inst_overflow = true; return false; }
    uint32_t base = (uint32_t)(fr->inst_used / INST_BYTES);
    uint32_t off = base;
    for (int b = 0; b < dl->nb; b++) {
        dl->batches[b].first = off;
        off += dl->batches[b].count;
        dl->batches[b].count = 0;
    }
    float *stream = (float *)fr->inst.mapped;
    for (int i = 0; i < dl->ni; i++) {
        Batch *bt = &dl->batches[dl->item_batch[i]];
        memcpy(stream + (size_t)(bt->first + bt->count) * INST_FLOATS, dl->inst + (size_t)i * INST_FLOATS, INST_BYTES);
        bt->count++;
    }
    fr->inst_used += need;
    return true;
}

/* see-through draws, furthest first (after dl_upload: each batch keeps its place in the stream) */
static int batch_far_first(const void *a, const void *b) {
    float ka = ((const Batch *)a)->key, kb = ((const Batch *)b)->key;
    return ka < kb ? 1 : ka > kb ? -1 : 0;
}
static int pick_lod(const LxMesh *m, float screen_size) {
    if (!R.settings.lod_enabled) return 0;
    float bias = R.settings.lod_bias > 0.05f ? R.settings.lod_bias : 1.0f;
    for (int l = 0; l < m->lod_count; l++)
        if (screen_size * bias >= m->lods[l].screen_size) return l;
    return m->lod_count - 1;
}

enum { DRAW_DEPTH, DRAW_MAIN, DRAW_MRT, DRAW_PROBE, DRAW_SEE_THROUGH };
static int mat_flags(const LxMaterial *m) {
    int flags = (m->roughness_map[0] ? 1 : 0) | (m->metallic_map[0] ? 2 : 0) | (m->ao_map[0] ? 4 : 0) |
                (m->roughness_is_smoothness ? 8 : 0) | (m->parallax_shadows ? 16 : 0);
    if (lx_terrain_gpu_layers(m)) flags |= 32;   /* MF_TERRAIN: blend the layers the vertices carry */
    if (m->blend_mode == LX_BLEND_GLASS) flags |= 256;
    if (m->blend_mode == LX_BLEND_ADDITIVE) flags |= 512;
    return flags;
}
/* Draws a list. DRAW_DEPTH: shadows and the precipitation map (the caller pushes the light's matrix as `light_vp`);
 * solid materials share R.shadow, Cutout / Fade / Glass use R.shadow_cut with their colour map, Additive casts none.
 * The shading modes pick each batch's pipeline from its material (cut-out pixels, both faces). */
static void draw_batches(VkCommandBuffer cmd, const DrawList *dl, VkBuffer inst, int mode, VkDescriptorSet scene_set, uint32_t ubo_off,
                         const float *light_vp) {
    VkPipeline bound = VK_NULL_HANDLE;
    for (int b = 0; b < dl->nb; b++) {
        const Batch *bt = &dl->batches[b];
        if (!bt->count) continue;
        const LxMaterial *m = bt->mat;
        if (mode == DRAW_DEPTH) {
            if (m->blend_mode == LX_BLEND_ADDITIVE) continue;
            bool cut = m->blend_mode != LX_BLEND_OPAQUE && (m->albedo_map[0] || m->albedo.w < 0.999f) && !lx_terrain_gpu_layers(m);
            VkPipeline want = cut ? R.shadow_cut : R.shadow;
            if (want != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
                if (light_vp) {
                    if (cut) vkCmdPushConstants(cmd, R.shadow_cut_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, light_vp);
                    else vkCmdPushConstants(cmd, R.shadow_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, light_vp);
                }
                bound = want;
            }
            if (cut) {
                VkDescriptorSet ms = material_set(m);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.shadow_cut_layout, 0, 1, &ms, 0, NULL);
                float mp[4] = {m->albedo.w, m->alpha_cutoff, m->uv_scale > 0 ? m->uv_scale : 1.0f, m->blend_mode == LX_BLEND_CUTOUT ? 0.0f : 1.0f};
                vkCmdPushConstants(cmd, R.shadow_cut_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 64, 16, mp);
            }
        } else {
            VkPipeline want;
            int two = m->double_sided ? 1 : 0;
            if (mode == DRAW_SEE_THROUGH) want = R.pbr_blend[two];
            else want = R.pbr_var[mode == DRAW_MRT ? 1 : mode == DRAW_PROBE ? 2 : 0][m->blend_mode == LX_BLEND_CUTOUT ? 1 : 0][two];
            if (want != bound) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want); bound = want; }
        }
        MeshGpu *g = mesh_gpu(bt->mesh);
        VkBuffer vbs[2] = {g->vb[bt->lod].buf, inst};
        VkDeviceSize offs[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
        vkCmdBindIndexBuffer(cmd, g->ib[bt->lod].buf, 0, VK_INDEX_TYPE_UINT32);
        if (mode != DRAW_DEPTH) {
            VkDescriptorSet sets[2] = {scene_set, material_set(m)};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pbr_layout, 0, 2, sets, 1, &ubo_off);
            float nstr = m->normal_map[0] ? lx_maxf(m->normal_strength, 0.001f) * (m->normal_flip_green ? -1.0f : 1.0f) : 0.0f;
            int flags = mat_flags(m);
            const LxTerrainGpuLayers *tl = lx_terrain_gpu_layers(m);
            float pc[24] = {m->albedo.x, m->albedo.y, m->albedo.z, m->albedo.w,
                            m->metallic, m->roughness, m->uv_scale > 0 ? m->uv_scale : 1.0f, nstr,
                            m->emissive.x, m->emissive.y, m->emissive.z, m->emissive_strength,
                            m->height_map[0] ? lx_maxf(m->height_scale, 0.0f) : 0.0f, m->ao_strength, (float)flags,
                            lx_clampf(m->alpha_cutoff, 0.0f, 1.0f)};
            if (tl) {   /* each layer's tiling (metres a repeat covers), how many layers */
                for (int l = 0; l < 6; l++) pc[16 + l] = tl->tiling[l] > 0.05f ? tl->tiling[l] : 4.0f;
                pc[22] = (float)tl->count;
                pc[23] = tl->parallax_depth;
            }
            vkCmdPushConstants(cmd, R.pbr_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96, pc);
        }
        vkCmdDrawIndexed(cmd, (uint32_t)bt->mesh->lods[bt->lod].index_count, bt->count, 0, 0, bt->first);
        R.stats.draw_calls++;
        R.stats.instances_drawn += (int)bt->count;
        R.stats.triangles += (int64_t)bt->count * (bt->mesh->lods[bt->lod].index_count / 3);
    }
}

/* ------------------------------------------------------------------ cascades */
static void compute_cascades(const LxRenderView *v, LxVec3 light_dir, int count, float max_dist, LxMat4 *out_vp, float *splits) {
    LxMat4 inv_view = lx_mat4_inverse(v->view);
    float tan_half = 1.0f / v->proj.m[5];
    float aspect = v->proj.m[5] / v->proj.m[0];
    float nearp = v->near_clip > 0 ? v->near_clip : 0.1f;
    float farp = lx_minf(max_dist, v->far_clip > 0 ? v->far_clip : 1000.0f);
    float prev = nearp;
    for (int c = 0; c < count; c++) {
        float p = (float)(c + 1) / (float)count;
        float log_s = nearp * powf(farp / nearp, p), lin_s = nearp + (farp - nearp) * p;
        float split = lx_lerpf(lin_s, log_s, 0.75f);
        splits[c] = split;
        /* bounding sphere of the frustum slice in view space */
        LxVec3 center_v = lx_v3(0, 0, -(prev + split) * 0.5f);
        float far_h = split * tan_half, far_w = far_h * aspect;
        LxVec3 corner = lx_v3(far_w, far_h, -split);
        float radius = lx_len3(lx_sub3(corner, center_v));
        float near_h = prev * tan_half, near_w = near_h * aspect;
        radius = lx_maxf(radius, lx_len3(lx_sub3(lx_v3(near_w, near_h, -prev), center_v)));
        radius = ceilf(radius * 16.0f) / 16.0f;
        LxVec3 center = lx_mat4_point(inv_view, center_v);
        LxVec3 up = fabsf(light_dir.y) > 0.99f ? lx_v3(0, 0, 1) : lx_v3(0, 1, 0);
        float back = radius + 60.0f;
        LxMat4 lview = lx_mat4_look_at(lx_madd3(center, light_dir, -back), center, up);
        /* snap the centre to shadow-map texels to stop shimmering when the camera moves */
        float texel = 2.0f * radius / (float)R.shadow_res;
        LxVec3 lc = lx_mat4_point(lview, center);
        float sx = floorf(lc.x / texel) * texel - lc.x, sy = floorf(lc.y / texel) * texel - lc.y;
        LxMat4 proj = lx_mat4_ortho(-radius + sx, radius + sx, -radius + sy, radius + sy, 0.1f, back + radius + 60.0f);
        out_vp[c] = lx_mat4_mul(proj, lview);
        prev = split;
    }
}

/* ------------------------------------------------------------------ scene rendering */
void lx_gi_reset(void);

/* Live GI's levels into their textures: the far level into gi[] / gi_vis[], the two camera levels stacked along z
 * in gc[] / gc_vis[]. Only what changed is copied (once the light has settled, nothing). */
static void gi_copy(VkCommandBuffer cmd, FrameData *fr, size_t *off, GpuImage *dst, int w, int h, int d, int zoff, const void *px, size_t bytes) {
    memcpy((uint8_t *)fr->gi_staging.mapped + *off, px, bytes);
    image_barrier(cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1, 1);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof region);
    region.bufferOffset = (VkDeviceSize)*off;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.z = zoff;
    region.imageExtent.width = (uint32_t)w; region.imageExtent.height = (uint32_t)h; region.imageExtent.depth = (uint32_t)d;
    vkCmdCopyBufferToImage(cmd, fr->gi_staging.buf, dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image_barrier(cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
    *off += (bytes + 15) & ~(size_t)15;
}
static void upload_gi(VkCommandBuffer cmd, FrameData *fr) {
    const LxGiVolume *gv = lx_gi_volume();
    if (!gv->valid || !gv->sh) return;
    if (gv->res[0] != R.gi_res[0] || gv->res[1] != R.gi_res[1] || gv->res[2] != R.gi_res[2]) {
        vkDeviceWaitIdle(R.device);
        create_gi_images(gv->res[0], gv->res[1], gv->res[2]);
        write_scene_sets();
    }
    int cres = gv->casc[0].on ? gv->casc[0].res : 0;
    if (cres && cres != R.gc_res) { gc_create(cres); write_scene_sets(); }
    /* the staging space this frame could need: every level, colours and walls */
    size_t far_cells = (size_t)gv->res[0] * (size_t)gv->res[1] * (size_t)gv->res[2], cc = (size_t)cres * cres * cres;
    size_t need = (far_cells + cc * LX_GI_CASCADES) * (3 * 8 + 2 * 4) + cc * 8 * 2 * LX_GI_CASCADES + 32 * 16;
    if (fr->gi_staging_size < need) {   /* this frame's buffer: the GPU finished with it (its fence was waited on) */
        buffer_destroy(&fr->gi_staging);
        buffer_create(&fr->gi_staging, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        fr->gi_staging_size = need;
    }
    size_t off = 0;
    static uint16_t *half;
    static uint8_t *walls;
    static size_t half_cap, walls_cap;
    for (int l = 0; l < 3; l++) {
        bool far = l == 2;
        if (!far && (!gv->casc[l].on || gv->casc[l].res != R.gc_res)) continue;
        const float *sh = far ? gv->sh : gv->casc[l].sh, *vis = far ? gv->vis : gv->casc[l].vis;
        uint32_t ver = far ? gv->version : gv->casc[l].version, vver = far ? gv->vis_version : gv->casc[l].vis_version;
        int w = far ? gv->res[0] : R.gc_res, h = far ? gv->res[1] : R.gc_res, d = far ? gv->res[2] : R.gc_res, z = far ? 0 : l * R.gc_res;
        size_t cells = (size_t)w * h * d;
        if (ver != R.gi_up_sh[l]) {
            if (half_cap < cells * 4) { half_cap = cells * 4; half = lx_realloc(half, half_cap * 2); }
            for (int c = 0; c < 3; c++) {
                for (size_t i = 0; i < cells; i++)
                    for (int k = 0; k < 4; k++) half[i * 4 + (size_t)k] = f2h(sh[i * 12 + (size_t)c * 4 + (size_t)k]);
                gi_copy(cmd, fr, &off, far ? &R.gi[c] : &R.gc[c], w, h, d, z, half, cells * 8);
            }
            R.gi_up_sh[l] = ver;
        }
        if (vver != R.gi_up_vis[l] && vis) {
            if (walls_cap < cells * 8) { walls_cap = cells * 8; walls = lx_realloc(walls, walls_cap); }
            for (size_t i = 0; i < cells; i++)
                for (int k = 0; k < 8; k++) {
                    float dd = k < 6 ? vis[i * 6 + (size_t)k] : 1.5f;
                    walls[(k < 4 ? 0 : cells * 4) + i * 4 + (size_t)(k & 3)] = (uint8_t)lx_clampf(dd / 1.5f * 255.0f + 0.5f, 0, 255);
                }
            for (int c = 0; c < 2; c++) gi_copy(cmd, fr, &off, far ? &R.gi_vis[c] : &R.gc_vis[c], w, h, d, z, walls + (size_t)c * cells * 4, cells * 4);
            R.gi_up_vis[l] = vver;
        }
    }
    /* the camera levels' distance fields, in half cells, for the shader's contact shadows */
    for (int l = 0; l < LX_GI_CASCADES; l++) {
        if (!gv->casc[l].on || gv->casc[l].res != R.gc_res || !gv->casc[l].sdf || gv->casc[l].sdf_version == R.gi_up_sdf[l]) continue;
        int r2 = R.gc_res * 2;
        size_t n = (size_t)r2 * r2 * r2;
        if (half_cap < n) { half_cap = n; half = lx_realloc(half, half_cap * 2); }
        float inv = 2.0f / gv->casc[l].cell;
        for (size_t i = 0; i < n; i++) half[i] = f2h(lx_clampf(gv->casc[l].sdf[i] * inv, -3.0f, 3.0f));
        gi_copy(cmd, fr, &off, &R.gsdf, r2, r2, r2, l * r2, half, n * 2);
        R.gi_up_sdf[l] = gv->casc[l].sdf_version;
    }
    for (int l = 0; l < LX_GI_CASCADES; l++) {
        R.gc_ubo[l][3] = 0;
        if (!gv->casc[l].on || gv->casc[l].res != R.gc_res || R.gi_up_sh[l] == 0) continue;
        R.gc_ubo[l][0] = gv->casc[l].origin.x; R.gc_ubo[l][1] = gv->casc[l].origin.y; R.gc_ubo[l][2] = gv->casc[l].origin.z;
        R.gc_ubo[l][3] = gv->casc[l].cell;
    }
}

static void make_crowd_matrix(float *m, LxVec3 p, LxVec3 vel, float scale) {
    float yaw = (lx_len3sq(vel) > 1e-4f) ? atan2f(vel.x, vel.z) : 0.0f;
    float c = cosf(yaw) * scale, s = sinf(yaw) * scale;
    memset(m, 0, 64);
    m[0] = c; m[2] = -s;
    m[5] = scale;
    m[8] = s; m[10] = c;
    m[12] = p.x; m[13] = p.y; m[14] = p.z; m[15] = 1;
}

/* ------------------------------------------------------------------ reflection probes: per frame */
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static bool entity_hidden(LxScene *s, const LxEntityData *d) {
    for (LxEntity p = d->parent; p; p = lx_entity_data(s, p)->parent)
        if (!lx_entity_data(s, p)->active) return true;
    return false;
}

/* Everything that changes what a probe sees (geometry, materials, lights, sky) - not the camera.
 * Probes only re-capture when this changes, so a static scene costs nothing after the first capture. */
static uint64_t scene_look_hash(LxScene *s) {
    uint64_t h = 1469598103934665603ull;
    LxEnvironment eff;
    lx_env_effective(s, &eff);
    const LxEnvironment *e = &eff;
    bool dn = s->env.sky_mode == LX_SKY_DAY_NIGHT;
    float env[14] = {e->sky_zenith.x, e->sky_zenith.y, e->sky_zenith.z, e->sky_horizon.x, e->sky_horizon.y, e->sky_horizon.z,
                     e->ground_color.x, e->ground_color.y, e->ground_color.z, e->ambient_intensity, e->exposure, e->fog_density,
                     e->fog_color.x, e->fog_color.y};
    if (dn) for (int i = 0; i < 14; i++) env[i] = roundf(logf(1e-4f + fabsf(env[i])) * 25.0f);   /* a moving sky: only clear changes count */
    h = fnv(h, env, sizeof env);
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & (LX_COMP_MESH | LX_COMP_LIGHT)) || entity_hidden(s, d)) continue;
        h = fnv(h, &i, sizeof i);
        if (dn && (d->comps & LX_COMP_LIGHT) && d->light.type == LX_LIGHT_DIRECTIONAL) {
            /* the Day-night sun moves all the time: re-capture once it has moved a couple of degrees or changed colour */
            LxVec3 f = lx_norm3(lx_mat4_dir(d->world, lx_v3(0, 0, 1)));
            float q[7] = {roundf(f.x * 30), roundf(f.y * 30), roundf(f.z * 30), roundf(logf(1e-3f + d->light.intensity) * 20),
                          roundf(d->light.color.x * 20), roundf(d->light.color.y * 20), roundf(d->light.color.z * 20)};
            h = fnv(h, q, sizeof q);
            continue;
        }
        h = fnv(h, d->world.m, 64);
        if (d->comps & LX_COMP_MESH) {
            const LxMesh *mesh = entity_mesh(d);
            const LxMaterial *m = lx_asset_material(d->mesh.material);
            float mf[10] = {m->albedo.x, m->albedo.y, m->albedo.z, m->albedo.w, m->metallic, m->roughness,
                            m->emissive.x, m->emissive.y, m->emissive.z, m->emissive_strength};
            h = fnv(h, &mesh, sizeof mesh);
            h = fnv(h, &m, sizeof m);
            h = fnv(h, mf, sizeof mf);
        }
        if (d->comps & LX_COMP_LIGHT) {
            const LxLight *l = &d->light;
            float lf[7] = {(float)l->type, l->color.x, l->color.y, l->color.z, l->intensity, l->range, l->spot_angle_deg};
            h = fnv(h, lf, sizeof lf);
        }
    }
    return h;
}

/* Slot 0 is the automatic environment probe: it captures around the camera, snapped to a 4 m grid,
 * so it only re-captures after the camera has moved a few metres. Slots 1-4 hold the Reflection
 * Probe objects nearest the camera. */
static void probes_assign(LxScene *s, const LxRenderView *view, uint64_t look) {
    if (!R.settings.reflections_enabled) {
        for (int i = 0; i < PROBE_SLOTS; i++) R.probes[i].used = false;
        return;
    }
    const float cell = 4.0f;
    LxVec3 snap = lx_v3(floorf(view->eye.x / cell) * cell + cell * 0.5f, floorf(view->eye.y / cell) * cell + cell * 0.5f,
                        floorf(view->eye.z / cell) * cell + cell * 0.5f);
    ProbeSlot *g = &R.probes[0];
    g->used = true;
    g->entity = 0;
    g->pos = snap;
    g->bmin = lx_v3(-1e9f, -1e9f, -1e9f);
    g->bmax = lx_v3(1e9f, 1e9f, 1e9f);
    g->box = false;
    g->intensity = 1.0f;
    g->blend = 1.0f;
    g->realtime = false;
    g->want_hash = fnv(look, &snap, sizeof snap);

    typedef struct Cand { LxEntity e; float dist; LxVec3 c, half; const LxReflectionProbe *p; } Cand;
    Cand best[PROBE_SLOTS - 1];
    int nb = 0;
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_REFLECTION_PROBE) || d->probe.intensity <= 0.0f || entity_hidden(s, d)) continue;
        Cand c;
        c.e = lx_entity_make((uint32_t)i, d->generation & ((1u << (32 - LX_INDEX_BITS)) - 1u));
        c.p = &d->probe;
        c.c = lx_v3(d->world.m[12], d->world.m[13], d->world.m[14]);
        LxVec3 sc = lx_v3(lx_len3(lx_v3(d->world.m[0], d->world.m[1], d->world.m[2])), lx_len3(lx_v3(d->world.m[4], d->world.m[5], d->world.m[6])),
                          lx_len3(lx_v3(d->world.m[8], d->world.m[9], d->world.m[10])));
        c.half = lx_v3(fabsf(d->probe.size.x * sc.x) * 0.5f, fabsf(d->probe.size.y * sc.y) * 0.5f, fabsf(d->probe.size.z * sc.z) * 0.5f);
        LxVec3 o = lx_v3(lx_maxf(fabsf(view->eye.x - c.c.x) - c.half.x, 0), lx_maxf(fabsf(view->eye.y - c.c.y) - c.half.y, 0),
                         lx_maxf(fabsf(view->eye.z - c.c.z) - c.half.z, 0));
        c.dist = lx_len3(o);
        int at;
        if (nb < PROBE_SLOTS - 1) at = nb++;
        else if (c.dist < best[PROBE_SLOTS - 2].dist) at = PROBE_SLOTS - 2;   /* replaces the farthest */
        else continue;
        best[at] = c;
        for (int k = at; k > 0 && best[k].dist < best[k - 1].dist; k--) { Cand t = best[k]; best[k] = best[k - 1]; best[k - 1] = t; }
    }
    /* keep probes in the slot they already occupy (no re-capture), free the rest */
    bool placed[PROBE_SLOTS - 1] = {false}, taken[PROBE_SLOTS] = {true, false, false, false, false};
    for (int sl = 1; sl < PROBE_SLOTS; sl++) {
        ProbeSlot *p = &R.probes[sl];
        int found = -1;
        for (int k = 0; k < nb && p->used; k++)
            if (!placed[k] && best[k].e == p->entity) { found = k; break; }
        if (found < 0) { p->used = p->valid = p->cycling = false; p->entity = 0; continue; }
        placed[found] = true;
        taken[sl] = true;
    }
    for (int k = 0; k < nb; k++) {
        if (placed[k]) continue;
        for (int sl = 1; sl < PROBE_SLOTS; sl++)
            if (!taken[sl]) {
                ProbeSlot *p = &R.probes[sl];
                p->used = true; p->valid = p->cycling = false; p->entity = best[k].e; p->captured_hash = 0;
                taken[sl] = true;
                break;
            }
    }
    for (int sl = 1; sl < PROBE_SLOTS; sl++) {
        ProbeSlot *p = &R.probes[sl];
        if (!p->used) continue;
        for (int k = 0; k < nb; k++) {
            if (best[k].e != p->entity) continue;
            p->pos = best[k].c;
            p->bmin = lx_sub3(best[k].c, best[k].half);
            p->bmax = lx_add3(best[k].c, best[k].half);
            p->intensity = best[k].p->intensity;
            p->blend = lx_maxf(best[k].p->blend_distance, 0.01f);
            p->box = best[k].p->box_projection;
            p->realtime = best[k].p->realtime;
            float key[7] = {p->pos.x, p->pos.y, p->pos.z, best[k].half.x, best[k].half.y, best[k].half.z, p->box ? 1.0f : 0.0f};
            p->want_hash = fnv(look, key, sizeof key);
        }
    }
}

static void probes_fill_ubo(SceneUBO *u) {
    bool on = R.settings.reflections_enabled;
    for (int i = 0; i < PROBE_SLOTS; i++) {
        const ProbeSlot *p = &R.probes[i];
        bool live = on && p->used && p->valid;
        u->probe_pos[i][0] = p->pos.x; u->probe_pos[i][1] = p->pos.y; u->probe_pos[i][2] = p->pos.z;
        u->probe_pos[i][3] = live ? p->intensity : 0.0f;
        u->probe_min[i][0] = p->bmin.x; u->probe_min[i][1] = p->bmin.y; u->probe_min[i][2] = p->bmin.z; u->probe_min[i][3] = p->blend;
        u->probe_max[i][0] = p->bmax.x; u->probe_max[i][1] = p->bmax.y; u->probe_max[i][2] = p->bmax.z; u->probe_max[i][3] = p->box ? 1.0f : 0.0f;
    }
    u->refl[0] = (float)R.probe_mips;
    u->refl[1] = on ? 1.0f : 0.0f;
}

/* Renders one cube face of a probe: the scene as seen from the capture point, 90 degree field of view.
 * Cube faces use a left-handed convention, so the projection is mirrored (and culling flipped). */
static bool probe_render_face(LxScene *s, ProbeSlot *p, int face, VkCommandBuffer cmd, FrameData *fr, const SceneUBO *base) {
    static const LxVec3 fwd[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const LxVec3 up[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    if (fr->ubo_used >= MAX_VIEWS_PER_FRAME) return false;
    SceneUBO ubo = *base;
    LxVec3 eye = p->cap_pos;
    float far_clip = lx_maxf(R.settings.shadow_distance * 2.0f, 150.0f);
    LxMat4 view = lx_mat4_look_at(eye, lx_add3(eye, fwd[face]), up[face]);
    LxMat4 proj = lx_mat4_perspective(90.0f * LX_DEG2RAD, 1.0f, 0.05f, far_clip);
    proj.m[0] = -proj.m[0];
    ubo.view = view;
    ubo.proj = proj;
    ubo.view_proj = lx_mat4_mul(proj, view);
    ubo.inv_view_proj = lx_mat4_inverse(ubo.view_proj);
    ubo.camera_pos[0] = eye.x; ubo.camera_pos[1] = eye.y; ubo.camera_pos[2] = eye.z;
    for (int i = 0; i < PROBE_SLOTS; i++) ubo.probe_pos[i][3] = 0.0f;   /* probes never see other probes */
    ubo.refl[1] = 0.0f;
    /* shadows: the cascades follow the main camera; use the widest one for every pixel */
    if (ubo.counts[1] > 0) {
        for (int c = 0; c < ubo.counts[1] - 1; c++) ubo.cascade_splits[c] = 0.0f;
        ubo.cascade_splits[ubo.counts[1] - 1] = 1e9f;
    }
    uint32_t off = fr->ubo_used * 4096;
    memcpy((uint8_t *)fr->ubo.mapped + off, &ubo, sizeof ubo);
    fr->ubo_used++;

    LxFrustum fru = lx_frustum_from_matrix(ubo.view_proj);
    dl_reset(&R.probe_list);
    LxVec4 white = lx_v4(1, 1, 1, 1);
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_MESH) || entity_hidden(s, d)) continue;
        LxMesh *mesh = entity_mesh(d);
        LxAabb wb = lx_aabb_transform(mesh->bounds, d->world);
        /* skip whatever the capture point is inside of (it would only show its back faces) */
        if (eye.x > wb.min.x && eye.x < wb.max.x && eye.y > wb.min.y && eye.y < wb.max.y && eye.z > wb.min.z && eye.z < wb.max.z) continue;
        if (!lx_frustum_test_aabb(&fru, wb)) continue;
        LxVec3 c = lx_aabb_center(wb);
        float radius = lx_len3(lx_aabb_extent(wb));
        float dist = lx_maxf(lx_len3(lx_sub3(c, eye)), 1e-3f);
        float lmb[4];
        const float *lm = entity_lightmap(d, mesh, lmb);
        LxMesh *dm = lm[3] >= 0 ? lx_mesh_lightmap_variant(mesh, lx_mat4_scale_of(&d->world)) : mesh;
        const LxMaterial *pm = lx_asset_material(d->mesh.material);
        if (lx_blend_see_through(pm->blend_mode)) continue;   /* reflections show the solid surroundings */
        dl_add_lm(&R.probe_list, dm, pick_lod(dm, radius * 0.5f / dist), pm, d->world.m, white, lm);
    }
    if (!dl_upload(&R.probe_list, fr)) return false;

    image_barrier_layers(cmd, p->cube.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, 1, (uint32_t)face, 1);
    image_barrier(cmd, R.probe_depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0, 1, 1);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = p->face_view[0][face];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   /* the sky covers every pixel */
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = R.probe_depth.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent.width = ri.renderArea.extent.height = (uint32_t)R.probe_res;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {0, 0, (float)R.probe_res, (float)R.probe_res, 0, 1};
    VkRect2D sc = {{0, 0}, {(uint32_t)R.probe_res, (uint32_t)R.probe_res}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.sky);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.scene_only_layout, 0, 1, &fr->scene_set, 1, &off);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pbr_probe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pbr_layout, 2, 1, &R.probe_dummy_set, 0, NULL);
    draw_batches(cmd, &R.probe_list, fr->inst.buf, DRAW_PROBE, fr->scene_set, off, NULL);
    vkCmdEndRendering(cmd);
    image_barrier_layers(cmd, p->cube.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_SHADER_READ_BIT, 0, 1, (uint32_t)face, 1);
    R.stats.probe_faces_rendered++;
    return true;
}

/* GGX pre-filter: mip m holds the probe as seen by a surface of roughness m / (mips - 1). */
static void probe_prefilter(ProbeSlot *p, VkCommandBuffer cmd) {
    for (int m = 1; m < R.probe_mips; m++) {
        uint32_t size = (uint32_t)(R.probe_res >> m);
        if (size < 1) size = 1;
        float pc[4] = {0, (float)m / (float)(R.probe_mips - 1), (float)R.probe_res, (float)(m - 1)};
        for (int f = 0; f < 6; f++) {
            image_barrier_layers(cmd, p->cube.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, (uint32_t)m, 1, (uint32_t)f, 1);
            VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ca.imageView = p->face_view[m][f];
            ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea.extent.width = ri.renderArea.extent.height = size;
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &ca;
            vkCmdBeginRendering(cmd, &ri);
            VkViewport vp = {0, 0, (float)size, (float)size, 0, 1};
            VkRect2D sc = {{0, 0}, {size, size}};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.probe_filter);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.filter_layout, 0, 1, &p->src_set[m], 0, NULL);
            pc[0] = (float)f;
            vkCmdPushConstants(cmd, R.filter_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            image_barrier_layers(cmd, p->cube.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                                 (uint32_t)m, 1, (uint32_t)f, 1);
        }
    }
}

void lx_reflections_refresh(void) {
    for (int i = 0; i < PROBE_SLOTS; i++) R.probes[i].captured_hash = 0;
}

/* Time-sliced capture: a new probe captures all six faces at once (up to two probes a frame);
 * a probe whose surroundings changed refreshes a couple of faces per frame and keeps showing
 * its previous capture until the new one is complete. */
static void probes_update(LxScene *s, VkCommandBuffer cmd, FrameData *fr, const SceneUBO *base) {
    if (!R.settings.reflections_enabled) return;
    int budget_new = 12, budget_refresh = 2;
    for (int k = 0; k < PROBE_SLOTS; k++) {
        ProbeSlot *p = &R.probes[(R.probe_rr + k) % PROBE_SLOTS];
        if (!p->used) continue;
        if (!p->cycling && (!p->valid || p->want_hash != p->captured_hash || p->realtime)) {
            p->cycling = true;
            p->next_face = 0;
            p->cycle_hash = p->want_hash;
            p->cap_pos = p->pos;
        }
        if (!p->cycling) continue;
        int *budget = p->valid ? &budget_refresh : &budget_new;
        while (p->next_face < 6 && *budget > 0) {
            if (!probe_render_face(s, p, p->next_face, cmd, fr, base)) return;
            p->next_face++;
            (*budget)--;
        }
        if (p->next_face == 6) {
            probe_prefilter(p, cmd);
            p->cycling = false;
            p->valid = true;
            p->captured_hash = p->cycle_hash;
        }
    }
    R.probe_rr = (R.probe_rr + 1) % PROBE_SLOTS;
}

/* Characters, animated models, physics objects (and whatever they carry) don't keep rain and snow off the ground:
 * the precipitation map would leave a dry outline under them that stays where they stood a moment ago. */
static bool moves_about(LxScene *s, const LxEntityData *d) {
    const uint32_t MOVING = LX_COMP_RIGIDBODY | LX_COMP_CHARACTER | LX_COMP_ANIMATOR | LX_COMP_STATE_GRAPH | LX_COMP_BEHAVIOUR;
    for (int depth = 0; d && depth < 64; depth++) {
        if (d->skin || (d->comps & MOVING)) return true;
        d = d->parent ? lx_entity_data(s, d->parent) : NULL;
    }
    return false;
}

/* the Day-night sky, clouds and weather for the shaders (zeros: the Gradient sky) */
static void sky_fill_ubo(LxScene *s, const LxGraphicsSettings *gs, const LxRenderView *view, SceneUBO *u) {
    const LxEnvironment *e = &s->env;
    if (e->sky_mode != LX_SKY_DAY_NIGHT) return;
    const LxSkyEval *v = lx_sky_eval(s);
    float ph = e->moon_phase - floorf(e->moon_phase);
    float lit = 0.5f - 0.5f * cosf(2.0f * LX_PI * ph);
    u->sky_params[0] = 1.0f; u->sky_params[1] = lx_maxf(0.0f, e->sun_intensity); u->sky_params[2] = v->haze; u->sky_params[3] = e->star_intensity;
    u->sky_sun[0] = v->sun_dir.x; u->sky_sun[1] = v->sun_dir.y; u->sky_sun[2] = v->sun_dir.z;
    u->sky_sun[3] = lx_clampf(view->eye.y, 0.0f, 50000.0f) + 60.0f;
    u->sky_moon[0] = v->moon_dir.x; u->sky_moon[1] = v->moon_dir.y; u->sky_moon[2] = v->moon_dir.z;
    u->sky_moon[3] = lx_maxf(0.0f, e->moon_intensity) * lit;
    u->sky_misc[0] = ph; u->sky_misc[1] = v->night; u->sky_misc[2] = v->flash;
    u->sky_misc[3] = (e->time_of_day - 12.0f) * 15.0f * LX_DEG2RAD;   /* the stars turn once a day */
    {   /* the celestial pole: due north, as high as the latitude */
        float la = lx_clampf(e->latitude, -89.9f, 89.9f) * LX_DEG2RAD, hd = e->sun_heading * LX_DEG2RAD;
        float x = 0.0f, z = -cosf(la), y = sinf(la);   /* north is -Z before the heading turns it */
        u->sky_pole[0] = x * cosf(hd) - z * sinf(hd); u->sky_pole[1] = y; u->sky_pole[2] = x * sinf(hd) + z * cosf(hd);
    }
    /* clouds: steps by quality (ultra 48 .. low 10) */
    static const int STEPS[4] = {10, 18, 28, 44}, LSTEPS[4] = {2, 3, 4, 6};
    int q = gs->quality_preset < 0 ? 0 : gs->quality_preset > 3 ? 3 : gs->quality_preset;
    u->cloud_a[0] = v->coverage; u->cloud_a[1] = v->density; u->cloud_a[2] = v->ctype;
    u->cloud_a[3] = v->coverage > 0.001f ? (float)STEPS[q] : 0.0f;
    u->cloud_b[0] = lx_maxf(e->cloud_height, 50.0f); u->cloud_b[1] = lx_maxf(e->cloud_thickness, 50.0f);
    u->cloud_b[2] = v->wind_offset[0]; u->cloud_b[3] = v->wind_offset[1];
    u->cloud_c[0] = (float)LSTEPS[q]; u->cloud_c[1] = v->cloud_shade;
    u->weather_a[0] = v->rain; u->weather_a[1] = v->snow; u->weather_a[2] = v->wetness; u->weather_a[3] = v->snow_cover;
    float wa = v->wind_dir * LX_DEG2RAD;
    u->weather_b[0] = cosf(wa) * v->wind_speed; u->weather_b[1] = sinf(wa) * v->wind_speed; u->weather_b[2] = v->mist;
    u->bolt[0] = v->bolt_dir.x; u->bolt[1] = v->bolt_dir.y; u->bolt[2] = v->bolt_dir.z; u->bolt[3] = v->bolt_age;
    u->bolt_b[0] = v->bolt_seed; u->bolt_b[1] = s->sky.bolt_dist;
}

void lx_render_scene(LxScene *s, const LxRenderView *view, LxRenderTarget *target) {
    if (s) lx_scene_update_skins(s);   /* skinned meshes follow their bones */
    if (!R.frame_active || !s) return;
    double t_start = lx_time();
    FrameData *fr = &R.frames[R.frame];
    VkCommandBuffer cmd = fr->cmd;
    if (fr->ubo_used >= MAX_VIEWS_PER_FRAME) return;
    /* internal back buffer when drawing straight to the window */
    if (!target) {
        if (!R.backbuffer) {
            R.backbuffer = lx_calloc(1, sizeof *R.backbuffer);
            R.backbuffer->w = (int)R.swap_extent.width;
            R.backbuffer->h = (int)R.swap_extent.height;
            target_alloc(R.backbuffer, false);
        } else if (R.backbuffer->w != (int)R.swap_extent.width || R.backbuffer->h != (int)R.swap_extent.height ||
                   R.backbuffer->iw != (int)((float)R.swap_extent.width * lx_clampf(R.settings.render_scale, 0.25f, 2.0f))) {
            vkDeviceWaitIdle(R.device);
            target_free(R.backbuffer);
            R.backbuffer->w = (int)R.swap_extent.width;
            R.backbuffer->h = (int)R.swap_extent.height;
            target_alloc(R.backbuffer, false);
        }
    } else lx_render_target_resize(target, target->w, target->h); /* picks up render-scale changes */
    LxRenderTarget *rt = target ? target : R.backbuffer;
    LxGraphicsSettings *gs = &R.settings, plain;
    if (R.preview) {   /* asset thumbnails: no lens on them */
        plain = R.settings;
        plain.vignette = plain.film_grain = plain.chromatic_aberration = 0;
        plain.godrays_enabled = false;
        gs = &plain;
    }
    if (gs->shadow_resolution != R.shadow_res || gs->shadow_cascades != R.shadow_layers) {
        vkDeviceWaitIdle(R.device);
        int layers = gs->shadow_cascades < 1 ? 1 : (gs->shadow_cascades > MAX_CASCADES ? MAX_CASCADES : gs->shadow_cascades);
        gs->shadow_cascades = layers;
        create_shadow_map(gs->shadow_resolution < 256 ? 256 : gs->shadow_resolution, layers);
        if (local_shadow_size(gs->shadow_resolution) != R.local_shadow_res) create_local_shadow(local_shadow_size(gs->shadow_resolution));
        write_scene_sets();
    }
    if (!R.preview) {
        lx_sky_prepare(s);   /* Day-night: places the sun light before anything reads it */
        lx_terrain_update(s, lx_terrain_focus(s, view->eye), !s->simulating);
    }
    lx_scene_update_transforms(s);
    if (!R.preview) lightmaps_sync(s, fr);

    /* ---- GI ---- */
    if (R.preview) {
        /* a thumbnail: no GI of its own (the real views own it) */
    } else if (R.gi_frame != R.frame_serial) {   /* once per frame: the first view's camera places the levels */
        R.gi_frame = R.frame_serial;
        lx_gi_update(s, gs, view->eye);
        upload_gi(cmd, fr);
    }
    R.stats.gi_ms = lx_gi_last_ms();
    R.stats.gi_rays_traced = gs->gi_enabled ? lx_gi_last_ray_count() : 0;

    /* ---- lights ---- */
    SceneUBO ubo;
    memset(&ubo, 0, sizeof ubo);
    float best_sun = -1.0f;
    LxVec3 sun_dir = lx_norm3(lx_v3(-0.4f, -1.0f, -0.3f));
    bool sun_shadows = false;
    int nl = 0;
    bool light_casts[MAX_LIGHTS];
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_LIGHT)) continue;
        const LxLight *l = &d->light;
        LxVec3 dir = lx_norm3(lx_mat4_dir(d->world, lx_v3(0, 0, 1)));
        LxVec3 col = lx_scale3(l->color, l->intensity);
        if (l->type == LX_LIGHT_DIRECTIONAL) {
            if (l->intensity > best_sun) {
                best_sun = l->intensity;
                sun_dir = dir;
                sun_shadows = l->cast_shadows;
                /* w: 1 the sun, 2 a Baked sun (lightmapped surfaces already have its light) */
                ubo.sun_color[0] = col.x; ubo.sun_color[1] = col.y; ubo.sun_color[2] = col.z; ubo.sun_color[3] = l->mode == LX_LIGHT_BAKED ? 2.0f : 1.0f;
                ubo.bvol_params[2] = l->mode == LX_LIGHT_MIXED ? (float)l->mask_channel : -1.0f;
            }
        } else if (nl < MAX_LIGHTS) {
            light_casts[nl] = l->cast_shadows;
            float *L = ubo.lights[nl++];
            L[0] = d->world.m[12]; L[1] = d->world.m[13]; L[2] = d->world.m[14]; L[3] = l->range > 0.01f ? l->range : 0.01f;
            /* type 1 point, 2 spot; + 10 for a Baked light (lightmapped surfaces already have its light) */
            /* + 100 * (shadowmask channel + 1) for a Mixed light baked into lighting volumes */
            L[4] = col.x; L[5] = col.y; L[6] = col.z;
            L[7] = (l->type == LX_LIGHT_SPOT ? 2.0f : 1.0f) + (l->mode == LX_LIGHT_BAKED ? 10.0f : 0.0f) +
                   (l->mode == LX_LIGHT_MIXED && l->mask_channel >= 0 ? 100.0f * (float)(l->mask_channel + 1) : 0.0f);
            L[8] = dir.x; L[9] = dir.y; L[10] = dir.z; L[11] = cosf(l->spot_angle_deg * 0.5f * LX_DEG2RAD);
        }
    }
    ubo.view = view->view;
    ubo.proj = view->proj;
    ubo.view_proj = lx_mat4_mul(view->proj, view->view);
    ubo.inv_view_proj = lx_mat4_inverse(ubo.view_proj);
    ubo.camera_pos[0] = view->eye.x; ubo.camera_pos[1] = view->eye.y; ubo.camera_pos[2] = view->eye.z;
    ubo.camera_pos[3] = (float)(lx_time() - R.start_time);
    ubo.sun_dir[0] = sun_dir.x; ubo.sun_dir[1] = sun_dir.y; ubo.sun_dir[2] = sun_dir.z;
    ubo.sun_dir[3] = (sun_shadows && ubo.sun_color[3] > 0.5f) ? 1.0f : 0.0f;
    LxEnvironment env_eff;
    lx_env_effective(s, &env_eff);
    const LxEnvironment *env = &env_eff;
    sky_fill_ubo(s, gs, view, &ubo);
    ubo.sky_zenith[0] = env->sky_zenith.x; ubo.sky_zenith[1] = env->sky_zenith.y; ubo.sky_zenith[2] = env->sky_zenith.z;
    ubo.sky_zenith[3] = env->ambient_intensity;
    ubo.sky_horizon[0] = env->sky_horizon.x; ubo.sky_horizon[1] = env->sky_horizon.y; ubo.sky_horizon[2] = env->sky_horizon.z;
    ubo.sky_horizon[3] = env->exposure;
    ubo.ground_color[0] = env->ground_color.x; ubo.ground_color[1] = env->ground_color.y; ubo.ground_color[2] = env->ground_color.z;
    ubo.ground_color[3] = env->fog_density;
    ubo.fog_color[0] = env->fog_color.x; ubo.fog_color[1] = env->fog_color.y; ubo.fog_color[2] = env->fog_color.z;
    ubo.fog_color[3] = gs->gi_intensity;
    int cascades = ubo.sun_dir[3] > 0.5f ? R.shadow_layers : 0;
    LxMat4 cvp[MAX_CASCADES];
    float splits[MAX_CASCADES] = {0};
    if (cascades) compute_cascades(view, sun_dir, cascades, gs->shadow_distance, cvp, splits);
    for (int c = 0; c < cascades; c++) { ubo.cascade_vp[c] = cvp[c]; ubo.cascade_splits[c] = splits[c]; }
    const LxGiVolume *gv = lx_gi_volume();
    bool gi_on = !R.preview && gs->gi_enabled && gv->valid && R.gi_res[0] == gv->res[0] && R.gi_res[1] == gv->res[1] && R.gi_res[2] == gv->res[2];
    ubo.gi_origin[0] = gv->origin.x; ubo.gi_origin[1] = gv->origin.y; ubo.gi_origin[2] = gv->origin.z; ubo.gi_origin[3] = gi_on ? 1.0f : 0.0f;
    ubo.gi_cell[0] = gv->cell_size.x; ubo.gi_cell[1] = gv->cell_size.y; ubo.gi_cell[2] = gv->cell_size.z;
    ubo.gi_res[0] = (float)gv->res[0]; ubo.gi_res[1] = (float)gv->res[1]; ubo.gi_res[2] = (float)gv->res[2];
    for (int l = 0; l < LX_GI_CASCADES; l++) memcpy(ubo.gi_casc[l], R.gc_ubo[l], sizeof R.gc_ubo[l]);
    if (!gi_on) for (int l = 0; l < LX_GI_CASCADES; l++) ubo.gi_casc[l][3] = 0;
    ubo.gi_casc_res[0] = R.gc_res;
    ubo.counts[0] = nl; ubo.counts[1] = cascades; ubo.counts[2] = R.shadow_res;
    /* rain and snow: how many drops and flakes, and the map that keeps them (and wetness, and snow) off what has a roof */
    int precip_drops = 0, precip_flakes = 0;
    bool precip_map = false;
    LxMat4 precip_vp = lx_mat4_identity();
    if (!R.preview && ubo.sky_params[0] > 0.5f) {
        static const float SCALE[4] = {0.35f, 0.6f, 1.0f, 1.4f};
        float q = SCALE[gs->quality_preset < 0 ? 0 : gs->quality_preset > 3 ? 3 : gs->quality_preset];
        if (ubo.weather_a[0] > 0.003f) precip_drops = (int)((1500.0f + 9000.0f * ubo.weather_a[0]) * q);
        if (ubo.weather_a[1] > 0.003f) precip_flakes = (int)((1200.0f + 7000.0f * ubo.weather_a[1]) * q);
        precip_map = precip_drops || precip_flakes || ubo.weather_a[2] > 0.002f || ubo.weather_a[3] > 0.002f;
        if (precip_map) {   /* 80 m around the eye, looking straight down, snapped so it doesn't swim */
            const float HALF = 40.0f, SNAP = 2.0f;
            LxVec3 c = lx_v3(floorf(view->eye.x / SNAP) * SNAP, view->eye.y, floorf(view->eye.z / SNAP) * SNAP);
            LxVec3 top = lx_v3(c.x, c.y + 150.0f, c.z);
            precip_vp = lx_mat4_mul(lx_mat4_ortho(-HALF, HALF, -HALF, HALF, 0.1f, 400.0f), lx_mat4_look_at(top, c, lx_v3(0, 0, -1)));
        }
    }
    ubo.precip_vp = precip_vp;
    ubo.counts[3] = R.shadow_layers;
    ubo.weather_b[3] = precip_map ? 1.0f : 0.0f;
    ubo.cloud_c[2] = (float)precip_drops; ubo.cloud_c[3] = (float)precip_flakes;
    /* point and spot shadows: the shadow-casting lights nearest the camera whose reach is in view get atlas tiles */
    int local_n = 0, local_light[LOCAL_SHADOW_LIGHTS];
    LxMat4 local_vp[LOCAL_SHADOW_LIGHTS][6];
    int local_faces[LOCAL_SHADOW_LIGHTS];
    {
        LxFrustum vf = lx_frustum_from_matrix(lx_mat4_mul(view->proj, view->view));
        float score[MAX_LIGHTS];
        int order[MAX_LIGHTS], no = 0;
        for (int i = 0; i < nl; i++) {
            ubo.light_shadow[i][0] = -1.0f;
            const float *L = ubo.lights[i];
            LxVec3 lp = lx_v3(L[0], L[1], L[2]);
            if (!light_casts[i] || !lx_frustum_test_sphere(&vf, lp, L[3])) continue;
            score[i] = lx_maxf(0.0f, lx_len3(lx_sub3(lp, view->eye)) - L[3]) / L[3];
            int k = no++;
            while (k > 0 && score[order[k - 1]] > score[i]) { order[k] = order[k - 1]; k--; }
            order[k] = i;
        }
        int tile = 0, tile_res = R.local_shadow_res / ATLAS_TILES;
        for (int o = 0; o < no && local_n < LOCAL_SHADOW_LIGHTS; o++) {
            int i = order[o];
            const float *L = ubo.lights[i];
            bool spot = fmodf(L[7], 10.0f) > 1.5f;
            int faces = spot ? 1 : 6;
            if (tile + faces > ATLAS_TILES * ATLAS_TILES) break;
            LxVec3 lp = lx_v3(L[0], L[1], L[2]);
            float zn = lx_clampf(L[3] * 0.005f, 0.02f, 0.2f), zf = L[3];
            float th = POINT_SHADOW_TAN;
            LxVec3 up = lx_v3(0, 1, 0);
            if (spot) {
                LxVec3 f = lx_norm3(lx_v3(L[8], L[9], L[10]));
                float half = acosf(lx_clampf(L[11], -1.0f, 1.0f));
                th = tanf(lx_clampf(half * 1.08f, 0.02f, 1.45f));   /* a little wider than the cone */
                LxVec3 ref = fabsf(f.y) < 0.99f ? lx_v3(0, 1, 0) : lx_v3(1, 0, 0);
                up = lx_norm3(lx_cross3(lx_cross3(f, ref), f));   /* ref with its part along f removed */
                local_vp[local_n][0] = lx_mat4_mul(lx_mat4_perspective(2.0f * atanf(th), 1.0f, zn, zf), lx_mat4_look_at(lp, lx_add3(lp, f), up));
            } else
                for (int fc = 0; fc < 6; fc++) {
                    const float *c = CUBE_FACES[fc];
                    local_vp[local_n][fc] = lx_mat4_mul(lx_mat4_perspective(2.0f * atanf(th), 1.0f, zn, zf),
                                                        lx_mat4_look_at(lp, lx_add3(lp, lx_v3(c[0], c[1], c[2])), lx_v3(c[3], c[4], c[5])));
                }
            float *S = ubo.light_shadow[i];
            S[0] = (float)tile; S[1] = zn; S[2] = zf; S[3] = th;
            S[4] = up.x; S[5] = up.y; S[6] = up.z; S[7] = (float)tile_res;
            local_light[local_n] = i;
            local_faces[local_n] = faces;
            tile += faces;
            local_n++;
        }
    }
    /* reflection probes: the first view rendered each frame owns probe placement and capture */
    bool probe_owner = !R.preview && R.probe_frame != R.frame_serial;
    if (probe_owner) {
        R.probe_frame = R.frame_serial;
        int want_res = gs->reflection_resolution <= 64 ? 64 : gs->reflection_resolution <= 128 ? 128 : gs->reflection_resolution <= 256 ? 256 : 512;
        if (want_res != R.probe_res) {
            vkDeviceWaitIdle(R.device);
            probes_create(want_res);
        }
        probes_assign(s, view, gs->reflections_enabled ? scene_look_hash(s) : 0);
    }
    probes_fill_ubo(&ubo);
    if (!R.preview) volumes_fill_ubo(s, &ubo);
    if (R.preview) { ubo.refl[1] = 0; for (int i = 0; i < PROBE_SLOTS; i++) ubo.probe_pos[i][3] = 0; }   /* the sky only */
    uint32_t ubo_off = fr->ubo_used * 4096;
    memcpy((uint8_t *)fr->ubo.mapped + ubo_off, &ubo, sizeof ubo);
    fr->ubo_used++;

    /* ---- culling, LOD and batching ---- */
    double t_cull = lx_time();
    LxFrustum frustum = lx_frustum_from_matrix(ubo.view_proj);
    LxFrustum cfr[MAX_CASCADES];
    for (int c = 0; c < cascades; c++) { cfr[c] = lx_frustum_from_matrix(cvp[c]); dl_reset(&R.shadow_lists[c]); }
    dl_reset(&R.main_list);
    dl_reset(&R.trans_list);
    dl_reset(&R.precip_list);
    LxFrustum pfr = lx_frustum_from_matrix(precip_vp);
    for (int k = 0; k < local_n; k++) dl_reset(&R.local_lists[k]);
    float proj_scale = view->proj.m[5] * 0.5f; /* screen-height fraction per unit at distance 1 */
    LxVec4 white = lx_v4(1, 1, 1, 1);
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active) continue;
        /* inactive parents hide children */
        bool hidden = false;
        for (LxEntity p = d->parent; p; p = lx_entity_data(s, p)->parent)
            if (!lx_entity_data(s, p)->active) { hidden = true; break; }
        if (hidden) continue;
        if (d->comps & LX_COMP_MESH) {
            LxMesh *mesh = entity_mesh(d);
            const LxMaterial *mat = lx_asset_material(d->mesh.material);
            LxAabb wb = lx_aabb_transform(mesh->bounds, d->world);
            LxVec3 c = lx_aabb_center(wb);
            float radius = lx_len3(lx_aabb_extent(wb));
            if (lx_frustum_test_aabb(&frustum, wb)) {
                float dist = lx_maxf(lx_len3(lx_sub3(c, view->eye)), 1e-3f);
                float lmb[4];
                const float *lm = entity_lightmap(d, mesh, lmb);
                LxMesh *dm = lm[3] >= 0 ? lx_mesh_lightmap_variant(mesh, lx_mat4_scale_of(&d->world)) : mesh;   /* stretched: its own unwrap */
                if (lx_blend_see_through(mat->blend_mode)) {   /* after the solid world, furthest first */
                    g_dl_key = dist;
                    dl_add_lm(&R.trans_list, dm, pick_lod(dm, radius * proj_scale / dist), mat, d->world.m, white, lm);
                    g_dl_key = -1;
                } else dl_add_lm(&R.main_list, dm, pick_lod(dm, radius * proj_scale / dist), mat, d->world.m, white, lm);
            } else R.stats.instances_culled++;
            if (precip_map && d->generated != LX_GEN_GRASS && lx_frustum_test_sphere(&pfr, c, radius) && !moves_about(s, d))   /* roofs: what stays put */
                dl_add(&R.precip_list, mesh, mesh->lod_count > 1 ? 1 : 0, mat, d->world.m, white);
            if (d->mesh.cast_shadows) {
                for (int k = 0; k < cascades; k++)
                    if (lx_frustum_test_sphere(&cfr[k], c, radius))
                        dl_add(&R.shadow_lists[k], mesh, mesh->lod_count > 1 ? 1 : 0, mat, d->world.m, white);
                for (int k = 0; k < local_n; k++) {   /* within the light's reach */
                    const float *L = ubo.lights[local_light[k]];
                    float reach = L[3] + radius;
                    if (lx_len3sq(lx_sub3(c, lx_v3(L[0], L[1], L[2]))) < reach * reach)
                        dl_add(&R.local_lists[k], mesh, mesh->lod_count > 1 ? 1 : 0, mat, d->world.m, white);
                }
            }
        }
        if ((d->comps & LX_COMP_CROWD) && d->crowd_sim) {
            LxMesh *mesh = lx_asset_mesh(d->crowd.mesh);
            const LxMaterial *mat = lx_asset_material(d->crowd.material);
            const LxVec3 *pos, *vel;
            int n = lx_crowd_positions(d->crowd_sim, &pos, &vel);
            LxVec3 origin = lx_v3(d->world.m[12], d->world.m[13], d->world.m[14]);
            float r = lx_len3(lx_aabb_extent(mesh->bounds)) * 0.6f;
            float m[16];
            for (int a = 0; a < n; a++) {
                LxVec3 p = lx_add3(origin, pos[a]);
                bool vis = lx_frustum_test_sphere(&frustum, p, r);
                if (!vis) { R.stats.instances_culled++; }
                bool shadow = false;
                for (int k = 0; k < cascades && !shadow; k++) shadow = k == 0 && lx_frustum_test_sphere(&cfr[k], p, r);
                if (!vis && !shadow) continue;
                make_crowd_matrix(m, p, vel[a], 0.6f);
                uint32_t h = (uint32_t)a * 2654435761u;
                LxVec4 tint = lx_v4(0.55f + 0.45f * (float)((h >> 8) & 255) / 255.0f, 0.55f + 0.45f * (float)((h >> 16) & 255) / 255.0f,
                                    0.55f + 0.45f * (float)((h >> 24) & 255) / 255.0f, 1);
                if (vis) {
                    float dist = lx_maxf(lx_len3(lx_sub3(p, view->eye)), 1e-3f);
                    bool see = lx_blend_see_through(mat->blend_mode);
                    if (see) g_dl_key = dist;
                    dl_add(see ? &R.trans_list : &R.main_list, mesh, pick_lod(mesh, r * proj_scale / dist), mat, m, tint);
                    g_dl_key = -1;
                }
                if (shadow) dl_add(&R.shadow_lists[0], mesh, mesh->lod_count - 1, mat, m, tint);
            }
        }
    }
    bool ok = dl_upload(&R.main_list, fr);
    if (R.trans_list.nb) {
        ok = dl_upload(&R.trans_list, fr) && ok;
        qsort(R.trans_list.batches, (size_t)R.trans_list.nb, sizeof(Batch), batch_far_first);
    }
    for (int c = 0; c < cascades; c++) ok = dl_upload(&R.shadow_lists[c], fr) && ok;
    for (int k = 0; k < local_n; k++) ok = dl_upload(&R.local_lists[k], fr) && ok;
    if (precip_map) ok = dl_upload(&R.precip_list, fr) && ok;
    R.stats.cull_ms = (float)((lx_time() - t_cull) * 1000.0);
    if (!ok) {
        /* instance stream full: draw nothing extra this frame, buffer grows next frame */
        dl_reset(&R.main_list);
        dl_reset(&R.trans_list);
        for (int c = 0; c < cascades; c++) dl_reset(&R.shadow_lists[c]);
        for (int k = 0; k < local_n; k++) dl_reset(&R.local_lists[k]);
        dl_reset(&R.precip_list);
    }

    /* ---- shadow pass ---- */
    if (cascades) {
        image_barrier(cmd, R.shadow_map.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0, 1, (uint32_t)R.shadow_layers);
        for (int c = 0; c < cascades; c++) {
            VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            da.imageView = R.shadow_map.layer_views[c];
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.clearValue.depthStencil.depth = 1.0f;
            VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea.extent.width = (uint32_t)R.shadow_res;
            ri.renderArea.extent.height = (uint32_t)R.shadow_res;
            ri.layerCount = 1;
            ri.pDepthAttachment = &da;
            vkCmdBeginRendering(cmd, &ri);
            VkViewport vp = {0, 0, (float)R.shadow_res, (float)R.shadow_res, 0, 1};
            VkRect2D sc = {{0, 0}, {(uint32_t)R.shadow_res, (uint32_t)R.shadow_res}};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            draw_batches(cmd, &R.shadow_lists[c], fr->inst.buf, DRAW_DEPTH, VK_NULL_HANDLE, 0, cvp[c].m);
            vkCmdEndRendering(cmd);
        }
        image_barrier(cmd, R.shadow_map.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1,
                      (uint32_t)R.shadow_layers);
    }

    /* ---- the precipitation map: the shadow map's last layer, from straight above ---- */
    if (precip_map) {
        uint32_t layer = (uint32_t)R.shadow_layers;
        image_barrier_layers(cmd, R.shadow_map.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0, 1, layer, 1);
        VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = R.shadow_map.layer_views[layer];
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent.width = (uint32_t)R.shadow_res;
        ri.renderArea.extent.height = (uint32_t)R.shadow_res;
        ri.layerCount = 1;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport vp = {0, 0, (float)R.shadow_res, (float)R.shadow_res, 0, 1};
        VkRect2D sc = {{0, 0}, {(uint32_t)R.shadow_res, (uint32_t)R.shadow_res}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        draw_batches(cmd, &R.precip_list, fr->inst.buf, DRAW_DEPTH, VK_NULL_HANDLE, 0, precip_vp.m);
        vkCmdEndRendering(cmd);
        image_barrier_layers(cmd, R.shadow_map.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, layer, 1);
    }

    /* ---- point / spot light shadows: one pass over the atlas, a viewport per tile ---- */
    R.stats.local_shadows = local_n;
    if (local_n) {
        image_barrier(cmd, R.local_shadow.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0, 1, 1);
        VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = R.local_shadow.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent.width = (uint32_t)R.local_shadow_res;
        ri.renderArea.extent.height = (uint32_t)R.local_shadow_res;
        ri.layerCount = 1;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        int tres = R.local_shadow_res / ATLAS_TILES;
        for (int k = 0; k < local_n; k++) {
            int first = (int)ubo.light_shadow[local_light[k]][0];
            for (int fc = 0; fc < local_faces[k]; fc++) {
                int t = first + fc, tx = t % ATLAS_TILES, ty = t / ATLAS_TILES;
                VkViewport vp = {(float)(tx * tres), (float)(ty * tres), (float)tres, (float)tres, 0, 1};
                VkRect2D sc = {{tx * tres, ty * tres}, {(uint32_t)tres, (uint32_t)tres}};
                vkCmdSetViewport(cmd, 0, 1, &vp);
                vkCmdSetScissor(cmd, 0, 1, &sc);
                draw_batches(cmd, &R.local_lists[k], fr->inst.buf, DRAW_DEPTH, VK_NULL_HANDLE, 0, local_vp[k][fc].m);
            }
        }
        vkCmdEndRendering(cmd);
        image_barrier(cmd, R.local_shadow.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
    }

    /* ---- reflection probe capture (time-sliced) ---- */
    if (probe_owner) probes_update(s, cmd, fr, &ubo);
    for (int i = 0; i < PROBE_SLOTS; i++) R.stats.probes_active += (gs->reflections_enabled && R.probes[i].used && R.probes[i].valid) ? 1 : 0;

    /* ---- main HDR pass ---- */
    bool persp = view->proj.m[15] < 0.5f;
    bool ssr = gs->ssr_enabled && persp;              /* perspective views only */
    bool ao = gs->ao_enabled && gs->ao_intensity > 0.0f && persp;
    bool mrt = ssr || ao;                               /* the extra targets: normals, reflection weight, ambient light */
    bool precip = precip_drops + precip_flakes > 0;
    bool shafts_on = gs->godrays_enabled && gs->godrays_intensity > 0.0f && persp;   /* they read the depth buffer too */
    bool see_through = R.trans_list.nb > 0;   /* drawn in the overlay pass, over the finished solid world */
    bool post = mrt || shafts_on || see_through || view->highlight || view->draw_grid || precip;
    if (mrt) {
        GpuImage *g[3] = {&rt->gnormal, &rt->gspec, &rt->gamb};
        for (int i = 0; i < 3; i++)
            image_barrier(cmd, g[i]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, 1, 1);
    }
    image_barrier(cmd, rt->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, 0, 1, 1);
    image_barrier(cmd, rt->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0, 1, 1);
    VkRenderingAttachmentInfo cas[4];
    memset(cas, 0, sizeof cas);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = rt->hdr.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[0] = view->clear_color.x;
    ca.clearValue.color.float32[1] = view->clear_color.y;
    ca.clearValue.color.float32[2] = view->clear_color.z;
    ca.clearValue.color.float32[3] = 1.0f;
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = rt->depth.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = post ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil.depth = 1.0f;
    cas[0] = ca;
    for (int i = 1; i < 4; i++) {
        cas[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        cas[i].imageView = i == 1 ? rt->gnormal.view : i == 2 ? rt->gspec.view : rt->gamb.view;
        cas[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        cas[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   /* zero = "no reflection" (sky) */
        cas[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent.width = (uint32_t)rt->iw;
    ri.renderArea.extent.height = (uint32_t)rt->ih;
    ri.layerCount = 1;
    ri.colorAttachmentCount = mrt ? 4 : 1;
    ri.pColorAttachments = cas;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {0, 0, (float)rt->iw, (float)rt->ih, 0, 1};
    VkRect2D sc = {{0, 0}, {(uint32_t)rt->iw, (uint32_t)rt->ih}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mrt ? R.pbr_mrt : R.pbr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pbr_layout, 2, 1, &R.probe_set, 0, NULL);
    draw_batches(cmd, &R.main_list, fr->inst.buf, mrt ? DRAW_MRT : DRAW_MAIN, fr->scene_set, ubo_off, NULL);
    /* the sky last, at the far plane: its pixels are the ones nothing covered */
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mrt ? R.sky_mrt : R.sky);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.scene_only_layout, 0, 1, &fr->scene_set, 1, &ubo_off);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    if (post) {
        /* depth becomes read-only: sampled by SSR and still depth-tested by the overlays */
        image_barrier(cmd, rt->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0, 1, 1);
        float ssr_pc[8] = {gs->ssr_max_distance > 0.5f ? gs->ssr_max_distance : 30.0f, gs->ssr_thickness > 0.01f ? gs->ssr_thickness : 0.35f,
                           (float)(gs->ssr_steps > 4 ? gs->ssr_steps : 24), gs->ssr_max_roughness > 0.05f ? gs->ssr_max_roughness : 0.6f,
                           (float)rt->iw, (float)rt->ih, (float)rt->sw, (float)rt->sh};
        if (mrt) {
            GpuImage *in[3] = {&rt->gnormal, &rt->gspec, &rt->gamb};
            for (int i = 0; i < 3; i++)
                image_barrier(cmd, in[i]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
        }
        float ao_pc[16] = {lx_clampf(gs->ao_radius, 0.05f, 10.0f), 0, 0, 0};
        if (ao)   /* ---- ambient occlusion (half resolution) ---- */
            post_pass(cmd, &rt->ao, rt->sw, rt->sh, false, R.ao, fr->scene_set, ubo_off, rt->ao_set, ao_pc);
        if (ssr) {
            /* ---- SSR trace (half resolution) ---- */
            image_barrier(cmd, rt->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
            image_barrier(cmd, rt->ssr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, 1, 1);
            VkRenderingAttachmentInfo sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            sa.imageView = rt->ssr.view;
            sa.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            sa.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo si = {VK_STRUCTURE_TYPE_RENDERING_INFO};
            si.renderArea.extent.width = (uint32_t)rt->sw;
            si.renderArea.extent.height = (uint32_t)rt->sh;
            si.layerCount = 1;
            si.colorAttachmentCount = 1;
            si.pColorAttachments = &sa;
            vkCmdBeginRendering(cmd, &si);
            VkViewport svp = {0, 0, (float)rt->sw, (float)rt->sh, 0, 1};
            VkRect2D ssc = {{0, 0}, {(uint32_t)rt->sw, (uint32_t)rt->sh}};
            vkCmdSetViewport(cmd, 0, 1, &svp);
            vkCmdSetScissor(cmd, 0, 1, &ssc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.ssr_trace);
            VkDescriptorSet tsets[3] = {fr->scene_set, rt->ssr_trace_set, R.probe_set};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.ssr_pipe_layout, 0, 3, tsets, 1, &ubo_off);
            vkCmdPushConstants(cmd, R.ssr_pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, ssr_pc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            image_barrier(cmd, rt->ssr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
            image_barrier(cmd, rt->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, 0, 1, 1);
        }
        /* ---- post pass: SSR composite, then editor overlays ---- */
        VkRenderingAttachmentInfo pa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pa.imageView = rt->hdr.view;
        pa.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        pa.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        pa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo pd = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pd.imageView = rt->depth.view;
        pd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        pd.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        pd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo pi = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        pi.renderArea.extent.width = (uint32_t)rt->iw;
        pi.renderArea.extent.height = (uint32_t)rt->ih;
        pi.layerCount = 1;
        pi.colorAttachmentCount = 1;
        pi.pColorAttachments = &pa;
        pi.pDepthAttachment = &pd;
        vkCmdBeginRendering(cmd, &pi);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        if (ssr) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.ssr_composite);
            VkDescriptorSet csets[3] = {fr->scene_set, rt->ssr_comp_set, R.probe_set};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.ssr_pipe_layout, 0, 3, csets, 1, &ubo_off);
            vkCmdPushConstants(cmd, R.ssr_pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, ssr_pc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        if (ao) {   /* the occlusion taken off the ambient light */
            float pc[16] = {lx_clampf(gs->ao_intensity, 0.0f, 4.0f), 1.0f / (float)rt->sw, 1.0f / (float)rt->sh, 0};
            VkDescriptorSet sets[2] = {fr->scene_set, rt->ao_apply_set};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.ao_apply);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.post_layout, 0, 2, sets, 1, &ubo_off);
            vkCmdPushConstants(cmd, R.post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        if (see_through) {   /* glass, water, smoke, fire: furthest first, over everything solid (and its AO / reflections) */
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pbr_blend[0]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pbr_layout, 2, 1, &R.probe_set, 0, NULL);
            draw_batches(cmd, &R.trans_list, fr->inst.buf, DRAW_SEE_THROUGH, fr->scene_set, ubo_off, NULL);
        }
        if (precip) {   /* rain and snow, depth-tested against the scene, blended over it */
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.precip);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.scene_only_layout, 0, 1, &fr->scene_set, 1, &ubo_off);
            vkCmdDraw(cmd, 6, (uint32_t)(precip_drops + precip_flakes), 0, 0);
        }
        if (view->highlight) {
            LxEntityData *d = lx_entity_data(s, view->highlight);
            if (d && (d->comps & LX_COMP_MESH) && fr->inst_used + INST_BYTES <= fr->inst.size) {
                LxMesh *mesh = entity_mesh(d);
                float *dst = (float *)((uint8_t *)fr->inst.mapped + fr->inst_used);
                memcpy(dst, d->world.m, 64);
                dst[16] = dst[17] = dst[18] = dst[19] = 1.0f;
                memcpy(dst + 20, NO_LIGHTMAP, 16);
                uint32_t first = (uint32_t)(fr->inst_used / INST_BYTES);
                fr->inst_used += INST_BYTES;
                MeshGpu *g = mesh_gpu(mesh);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.outline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.outline_layout, 0, 1, &fr->scene_set, 1, &ubo_off);
                float col[4] = {1.0f, 0.55f, 0.1f, 1.0f};
                vkCmdPushConstants(cmd, R.outline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, col);
                VkBuffer vbs[2] = {g->vb[0].buf, fr->inst.buf};
                VkDeviceSize offs[2] = {0, 0};
                vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
                vkCmdBindIndexBuffer(cmd, g->ib[0].buf, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, (uint32_t)mesh->lods[0].index_count, 1, 0, 0, first);
            }
        }
        if (view->draw_grid) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.grid);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.scene_only_layout, 0, 1, &fr->scene_set, 1, &ubo_off);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
        vkCmdEndRendering(cmd);
    }
    image_barrier(cmd, rt->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);

    /* ---- bloom: down to small levels and back up, each level adding the wider glow below it ---- */
    bool bloom = gs->bloom_enabled && gs->bloom_intensity > 0.0f && rt->bloom_n > 0;
    if (bloom) {
        for (int i = 0; i < rt->bloom_n; i++) {
            int sw = i ? rt->bw[i - 1] : rt->iw, sh = i ? rt->bh[i - 1] : rt->ih;
            float pc[16] = {1.0f / (float)sw, 1.0f / (float)sh, i == 0 ? 1.0f : 0.0f, 0};
            post_pass(cmd, &rt->bloom[i], rt->bw[i], rt->bh[i], false, R.bloom_down, fr->scene_set, ubo_off, rt->bloom_down_set[i], pc);
        }
        for (int i = rt->bloom_n - 1; i > 0; i--) {
            float pc[16] = {1.0f / (float)rt->bw[i], 1.0f / (float)rt->bh[i], 0, 0};
            post_pass(cmd, &rt->bloom[i - 1], rt->bw[i - 1], rt->bh[i - 1], true, R.bloom_up, fr->scene_set, ubo_off, rt->bloom_up_set[i], pc);
        }
    }
    /* ---- sun shafts: where the sun is on screen, and how much of it shows ---- */
    float shafts = 0;
    float sun_uv[2] = {0, 0};
    if (shafts_on && (ubo.sun_color[3] > 0.5f || ubo.sky_params[0] > 0.5f)) {
        LxVec3 to_sun = ubo.sky_params[0] > 0.5f ? lx_v3(ubo.sky_sun[0], ubo.sky_sun[1], ubo.sky_sun[2])
                                                 : lx_v3(-ubo.sun_dir[0], -ubo.sun_dir[1], -ubo.sun_dir[2]);
        to_sun = lx_norm3(to_sun);
        const float *m = ubo.view_proj.m;
        float cx = m[0] * to_sun.x + m[4] * to_sun.y + m[8] * to_sun.z;
        float cy = m[1] * to_sun.x + m[5] * to_sun.y + m[9] * to_sun.z;
        float cw = m[3] * to_sun.x + m[7] * to_sun.y + m[11] * to_sun.z;
        LxVec3 fwd = lx_v3(-view->view.m[2], -view->view.m[6], -view->view.m[10]);
        float facing = lx_dot3(lx_norm3(fwd), to_sun);
        if (cw > 1e-4f && facing > 0.0f) {
            sun_uv[0] = cx / cw * 0.5f + 0.5f;
            sun_uv[1] = 0.5f - cy / cw * 0.5f;
            float out = fmaxf(fmaxf(-sun_uv[0], sun_uv[0] - 1.0f), fmaxf(-sun_uv[1], sun_uv[1] - 1.0f));   /* how far off screen */
            float vis = lx_clampf(1.0f - out / 0.35f, 0.0f, 1.0f) * lx_clampf(facing * 3.0f, 0.0f, 1.0f);
            vis *= lx_clampf(to_sun.y * 12.0f + 0.3f, 0.0f, 1.0f);   /* fades as it sets */
            shafts = vis * lx_clampf(gs->godrays_intensity, 0.0f, 4.0f) * 0.25f;
        }
    }
    if (shafts > 0.0f) {
        float pc[16] = {sun_uv[0], sun_uv[1], shafts, (float)rt->iw / (float)(rt->ih > 0 ? rt->ih : 1)};
        post_pass(cmd, &rt->rays, rt->sw, rt->sh, false, R.godrays, fr->scene_set, ubo_off, rt->rays_set, pc);
    }

    /* ---- the final image ---- */
    float tone_pc[16] = {gs->exposure * env->exposure, bloom ? lx_clampf(gs->bloom_intensity, 0.0f, 1.0f) * 0.15f : 0.0f,
                         shafts > 0.0f ? 1.0f : 0.0f, (float)fmod(lx_time() - R.start_time, 1000.0),
                         lx_clampf(gs->vignette, 0.0f, 1.0f), lx_clampf(gs->film_grain, 0.0f, 1.0f),
                         lx_clampf(gs->chromatic_aberration, 0.0f, 1.0f), (float)rt->w / (float)(rt->h > 0 ? rt->h : 1),
                         gs->contrast > 0.0f ? lx_clampf(gs->contrast, 0.5f, 2.0f) : 1.0f, lx_clampf(gs->saturation, 0.0f, 2.0f),
                         lx_clampf(gs->temperature, -1.0f, 1.0f), 0, 0, 0, 0, 0};
    if (target) {
        image_barrier(cmd, rt->ldr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, 1, 1);
        VkRenderingAttachmentInfo la = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        la.imageView = rt->ldr.view;
        la.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        la.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        la.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo li = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        li.renderArea.extent.width = (uint32_t)rt->w;
        li.renderArea.extent.height = (uint32_t)rt->h;
        li.layerCount = 1;
        li.colorAttachmentCount = 1;
        li.pColorAttachments = &la;
        vkCmdBeginRendering(cmd, &li);
        VkViewport lvp = {0, 0, (float)rt->w, (float)rt->h, 0, 1};
        VkRect2D lsc = {{0, 0}, {(uint32_t)rt->w, (uint32_t)rt->h}};
        vkCmdSetViewport(cmd, 0, 1, &lvp);
        vkCmdSetScissor(cmd, 0, 1, &lsc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.tonemap_ldr);
        VkDescriptorSet tsets[2] = {fr->scene_set, rt->tone_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.post_layout, 0, 2, tsets, 1, &ubo_off);
        vkCmdPushConstants(cmd, R.post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, tone_pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        image_barrier(cmd, rt->ldr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
    } else {
        begin_swap_pass(cmd, false, lx_v4(0, 0, 0, 1));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.tonemap_swap);
        VkDescriptorSet tsets[2] = {fr->scene_set, rt->tone_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.post_layout, 0, 2, tsets, 1, &ubo_off);
        vkCmdPushConstants(cmd, R.post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, tone_pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }
    R.stats.frame_ms += (float)((lx_time() - t_start) * 1000.0);
}

/* ------------------------------------------------------------------ UI */
void lx_renderer_draw_ui(const LxUiDrawList *list) {
    if (!R.frame_active) return;
    FrameData *fr = &R.frames[R.frame];
    VkCommandBuffer cmd = fr->cmd;
    size_t vb_bytes = sizeof(LxUiVertex) * (size_t)list->vertex_count, ib_bytes = sizeof(uint32_t) * (size_t)list->index_count;
    if (vb_bytes > fr->ui_vb.size || ib_bytes > fr->ui_ib.size) {
        vkDeviceWaitIdle(R.device);
        VkDeviceSize vs = fr->ui_vb.size, is = fr->ui_ib.size;
        while (vs < vb_bytes) vs *= 2;
        while (is < ib_bytes) is *= 2;
        for (int f = 0; f < FRAMES; f++) {
            /* only the current frame's buffers are guaranteed idle without a wait; we waited above */
            buffer_destroy(&R.frames[f].ui_vb);
            buffer_destroy(&R.frames[f].ui_ib);
            buffer_create(&R.frames[f].ui_vb, vs, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
            buffer_create(&R.frames[f].ui_ib, is, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        }
    }
    if (vb_bytes) memcpy(fr->ui_vb.mapped, list->vertices, vb_bytes);
    if (ib_bytes) memcpy(fr->ui_ib.mapped, list->indices, ib_bytes);
    begin_swap_pass(cmd, true, list->clear_color);
    if (list->cmd_count) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.ui);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &fr->ui_vb.buf, &off);
        vkCmdBindIndexBuffer(cmd, fr->ui_ib.buf, 0, VK_INDEX_TYPE_UINT32);
        float sw = (float)R.swap_extent.width, sh = (float)R.swap_extent.height;
        uint32_t bound = UINT32_MAX;
        for (int i = 0; i < list->cmd_count; i++) {
            const LxUiCmd *c = &list->cmds[i];
            if (!c->index_count) continue;
            uint32_t tex = (c->texture < MAX_UI_TEXTURES && R.ui_tex[c->texture].used && R.ui_tex[c->texture].set) ? c->texture : 0;
            if (!R.ui_tex[tex].set) continue;
            if (tex != bound) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.ui_layout, 0, 1, &R.ui_tex[tex].set, 0, NULL);
                float pc[4] = {2.0f / sw, 2.0f / sh, (float)R.ui_tex[tex].mode, 0};
                vkCmdPushConstants(cmd, R.ui_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
                bound = tex;
            }
            float x0 = lx_clampf(c->clip[0], 0, sw), y0 = lx_clampf(c->clip[1], 0, sh);
            float x1 = lx_clampf(c->clip[0] + c->clip[2], 0, sw), y1 = lx_clampf(c->clip[1] + c->clip[3], 0, sh);
            if (x1 <= x0 || y1 <= y0) continue;
            VkRect2D sc = {{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdDrawIndexed(cmd, c->index_count, 1, c->index_offset, 0, 0);
        }
    }
    vkCmdEndRendering(cmd);
}

bool lx_camera_view(LxScene *s, LxEntity cam, int w, int h, LxRenderView *out) {
    LxEntityData *d = lx_entity_data(s, cam);
    if (!d || !(d->comps & LX_COMP_CAMERA)) return false;
    LxMat4 world = lx_entity_world_matrix(s, cam);
    memset(out, 0, sizeof *out);
    /* cameras look along their +Z; the view looks down -Z: turn the camera half round first */
    LxMat4 turned = world;
    for (int k = 0; k < 3; k++) { turned.m[k] = -turned.m[k]; turned.m[8 + k] = -turned.m[8 + k]; }
    out->view = lx_mat4_inverse(turned);
    float aspect = h > 0 ? (float)w / (float)h : 1.0f;
    const LxCamera *c = &d->camera;
    if (c->orthographic) {
        float hs = c->ortho_size;
        out->proj = lx_mat4_ortho(-hs * aspect, hs * aspect, -hs, hs, c->near_clip, c->far_clip);
    } else out->proj = lx_mat4_perspective(c->fov_deg * LX_DEG2RAD, aspect, c->near_clip, c->far_clip);
    out->eye = lx_v3(world.m[12], world.m[13], world.m[14]);
    out->width = w;
    out->height = h;
    out->near_clip = c->near_clip;
    out->far_clip = c->far_clip;
    out->clear_color = c->clear_color;
    return true;
}
