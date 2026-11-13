/* Luxora Vulkan renderer.
 *
 * Vulkan 1.3 with dynamic rendering. Per frame (two frames in flight):
 *   1. CPU culling + LOD selection + batching into instanced draws (one draw per mesh/LOD/material)
 *   2. virtual-photon GI volume upload (L1 SH, three RGBA16F 3D textures) when it changes
 *   3. cascaded shadow maps (depth array, one layer per cascade)
 *   4. HDR forward PBR pass: sky, opaque geometry, editor grid and selection outline
 *   5. ACES tonemap into an LDR target (editor viewport) or straight into the swapchain
 *   6. UI pass on the swapchain (editor)
 */
#include "../lx_internal.h"
#include "vk.h"
#include "shaders_spv.h"

#define FRAMES 2
#define MAX_CASCADES 4
#define MAX_LIGHTS 16
#define MAX_VIEWS_PER_FRAME 8
#define MAX_UI_TEXTURES 1024
#define SPV(name) name, sizeof(name)

/* ------------------------------------------------------------------ types */
typedef struct GpuBuffer { VkBuffer buf; VkDeviceMemory mem; VkDeviceSize size; void *mapped; } GpuBuffer;
typedef struct GpuImage {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;      /* full view (array/3D as appropriate) */
    VkImageView layer_views[MAX_CASCADES];
    VkFormat fmt;
    uint32_t w, h, d, layers, mips;
} GpuImage;

typedef struct MeshGpu { GpuBuffer vb[LX_MAX_LODS], ib[LX_MAX_LODS]; } MeshGpu;
typedef struct TexGpu { GpuImage img; } TexGpu;

struct LxRenderTarget {
    int w, h;              /* output size */
    int iw, ih;            /* internal (render-scaled) size */
    GpuImage hdr, depth, ldr;
    VkDescriptorSet hdr_set;
    uint32_t ui_tex;
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
    GpuBuffer gi_staging;
    VkDescriptorSet scene_set;
    uint32_t gi_uploaded_version;
} FrameData;

typedef struct MatSet { const LxTexture *albedo, *normal; VkDescriptorSet set; } MatSet;

typedef struct SceneUBO {
    LxMat4 view, proj, view_proj, inv_view_proj;
    float camera_pos[4], sun_dir[4], sun_color[4], sky_zenith[4], sky_horizon[4], ground_color[4], fog_color[4];
    LxMat4 cascade_vp[MAX_CASCADES];
    float cascade_splits[4];
    float gi_origin[4], gi_cell[4], gi_res[4];
    int32_t counts[4];
    float lights[MAX_LIGHTS][12];
} SceneUBO;

typedef struct Batch { LxMesh *mesh; int lod; const LxMaterial *mat; uint32_t first, count; } Batch;
typedef struct DrawList {
    Batch *batches; int nb, cb;
    float *inst; int *item_batch; int ni, ci;
} DrawList;

/* ------------------------------------------------------------------ state */
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
    VkDescriptorSetLayout scene_layout, material_layout, tex_layout;
    VkPipelineLayout pbr_layout, shadow_layout, scene_only_layout, outline_layout, tonemap_layout, ui_layout;
    VkSampler linear_repeat, linear_clamp, shadow_sampler;
    /* pipelines */
    VkPipeline pbr, shadow, sky, grid, outline, tonemap_ldr, tonemap_swap, ui;
    /* images */
    GpuImage shadow_map;
    int shadow_res, shadow_layers;
    GpuImage gi[3];
    int gi_res[3];
    GpuImage white, flat_normal;
    UiTex ui_tex[MAX_UI_TEXTURES];
    MatSet *mat_sets; int mat_set_count, mat_set_cap;
    LxRenderTarget *backbuffer;
    /* settings & stats */
    LxGraphicsSettings settings;
    LxRenderStats stats;
    DrawList main_list, shadow_lists[MAX_CASCADES];
    char screenshot_path[LX_PATH_MAX];
    int validation_errors;
    double start_time;
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

static void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                          VkAccessFlags dst_access, uint32_t base_mip, uint32_t mips, uint32_t layers) {
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
    b.subresourceRange.layerCount = layers;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

typedef struct ImageDesc {
    uint32_t w, h, d, layers, mips;
    VkFormat fmt;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
    VkImageViewType view_type;
    bool layer_views;
} ImageDesc;

static bool image_create(GpuImage *im, const ImageDesc *d) {
    memset(im, 0, sizeof *im);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
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
        for (uint32_t l = 0; l < ci.arrayLayers && l < MAX_CASCADES; l++) {
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
    for (int l = 0; l < MAX_CASCADES; l++)
        if (im->layer_views[l]) vkDestroyImageView(R.device, im->layer_views[l], NULL);
    vkDestroyImageView(R.device, im->view, NULL);
    vkDestroyImage(R.device, im->img, NULL);
    vkFreeMemory(R.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

/* upload pixels into a 2D image (and optionally generate mips), leaving it SHADER_READ_ONLY */
static void image_upload_2d(GpuImage *im, const void *pixels, size_t bytes, bool gen_mips) {
    GpuBuffer staging;
    buffer_create(&staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(staging.mapped, pixels, bytes);
    VkCommandBuffer cmd = one_shot_begin();
    image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, im->mips, 1);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof region);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = im->w;
    region.imageExtent.height = im->h;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, staging.buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    if (gen_mips && im->mips > 1) {
        int32_t mw = (int32_t)im->w, mh = (int32_t)im->h;
        for (uint32_t m = 1; m < im->mips; m++) {
            image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, m - 1, 1, 1);
            VkImageBlit blit;
            memset(&blit, 0, sizeof blit);
            blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = m - 1;
            blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
            blit.dstSubresource.mipLevel = m;
            blit.srcOffsets[1].x = mw; blit.srcOffsets[1].y = mh; blit.srcOffsets[1].z = 1;
            mw = mw > 1 ? mw / 2 : 1;
            mh = mh > 1 ? mh / 2 : 1;
            blit.dstOffsets[1].x = mw; blit.dstOffsets[1].y = mh; blit.dstOffsets[1].z = 1;
            vkCmdBlitImage(cmd, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, m - 1, 1, 1);
        }
        image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, im->mips - 1, 1, 1);
    } else {
        image_barrier(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, im->mips, 1);
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
    VkVertexInputAttributeDescription attrs[9];
    uint32_t nb = 0, na = 0;
    if (d->vertex == VTX_MESH || d->vertex == VTX_MESH_POS_ONLY || d->vertex == VTX_MESH_POS_NORMAL) {
        bindings[nb++] = (VkVertexInputBindingDescription){0, sizeof(LxVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        bindings[nb++] = (VkVertexInputBindingDescription){1, 80, VK_VERTEX_INPUT_RATE_INSTANCE};
        attrs[na++] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        if (d->vertex == VTX_MESH_POS_NORMAL) attrs[na++] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12};
        if (d->vertex == VTX_MESH) {
            attrs[na++] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12};
            attrs[na++] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R32G32_SFLOAT, 24};
            attrs[na++] = (VkVertexInputAttributeDescription){3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32};
        }
        for (uint32_t c = 0; c < 4; c++) attrs[na++] = (VkVertexInputAttributeDescription){4 + c, 1, VK_FORMAT_R32G32B32A32_SFLOAT, c * 16};
        if (d->vertex == VTX_MESH) attrs[na++] = (VkVertexInputAttributeDescription){8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 64};
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
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE; /* GL-style CCW meshes become CW after the Y flip */
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
    VkPipelineColorBlendAttachmentState cb;
    memset(&cb, 0, sizeof cb);
    cb.colorWriteMask = 0xf;
    if (d->blend) {
        cb.blendEnable = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.colorBlendOp = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = d->color ? 1 : 0;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = d->color ? 1 : 0;
    ri.pColorAttachmentFormats = &d->color;
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
            if (score > best_score) { best_score = score; R.gpu = devs[i]; R.queue_family = q; R.props = p; }
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
static void create_shadow_map(int res, int layers) {
    image_destroy(&R.shadow_map);
    ImageDesc d = {(uint32_t)res, (uint32_t)res, 1, (uint32_t)layers, 1, R.depth_format,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                   VK_IMAGE_VIEW_TYPE_2D_ARRAY, true};
    image_create(&R.shadow_map, &d);
    VkCommandBuffer cmd = one_shot_begin();
    image_barrier(cmd, R.shadow_map.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, (uint32_t)layers);
    one_shot_end(cmd);
    R.shadow_res = res;
    R.shadow_layers = layers;
}

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
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_3D, false};
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
    R.gi_res[0] = rx; R.gi_res[1] = ry; R.gi_res[2] = rz;
    for (int f = 0; f < FRAMES; f++) {
        buffer_destroy(&R.frames[f].gi_staging);
        buffer_create(&R.frames[f].gi_staging, bytes * 3, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        R.frames[f].gi_uploaded_version = 0;
    }
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
        for (int c = 0; c < 3; c++) write_image(s, (uint32_t)(2 + c), R.gi[c].view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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

static void create_static_resources(void) {
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 4096;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(R.device, &pi, NULL, &R.desc_pool));

    VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding sb[5] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, vf, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    R.scene_layout = set_layout(sb, 5);
    VkDescriptorSetLayoutBinding mb[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
                                          {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    R.material_layout = set_layout(mb, 2);
    VkDescriptorSetLayoutBinding tb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    R.tex_layout = set_layout(&tb, 1);

    VkDescriptorSetLayout pbr_sets[2] = {R.scene_layout, R.material_layout};
    R.pbr_layout = pipe_layout(pbr_sets, 2, 48, VK_SHADER_STAGE_FRAGMENT_BIT);
    R.shadow_layout = pipe_layout(NULL, 0, 64, VK_SHADER_STAGE_VERTEX_BIT);
    R.scene_only_layout = pipe_layout(&R.scene_layout, 1, 0, 0);
    R.outline_layout = pipe_layout(&R.scene_layout, 1, 16, VK_SHADER_STAGE_FRAGMENT_BIT);
    R.tonemap_layout = pipe_layout(&R.tex_layout, 1, 16, VK_SHADER_STAGE_FRAGMENT_BIT);
    R.ui_layout = pipe_layout(&R.tex_layout, 1, 16, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    R.linear_repeat = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, false, 16.0f);
    R.linear_clamp = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false, 0.0f);
    R.shadow_sampler = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, true, 0.0f);

    PipeDesc p;
    memset(&p, 0, sizeof p);
    p.vs = lx_spv_pbr_vert; p.vs_size = sizeof lx_spv_pbr_vert;
    p.fs = lx_spv_pbr_frag; p.fs_size = sizeof lx_spv_pbr_frag;
    p.layout = R.pbr_layout; p.vertex = VTX_MESH; p.color = R.hdr_format; p.depth = R.depth_format;
    p.depth_test = p.depth_write = true; p.compare = VK_COMPARE_OP_LESS_OR_EQUAL; p.cull = VK_CULL_MODE_BACK_BIT;
    R.pbr = pipeline_create(&p);

    memset(&p, 0, sizeof p);
    p.vs = lx_spv_shadow_vert; p.vs_size = sizeof lx_spv_shadow_vert;
    p.layout = R.shadow_layout; p.vertex = VTX_MESH_POS_ONLY; p.depth = R.depth_format;
    p.depth_test = p.depth_write = true; p.depth_bias = true; p.cull = VK_CULL_MODE_NONE;
    R.shadow = pipeline_create(&p);

    memset(&p, 0, sizeof p);
    p.vs = lx_spv_fullscreen_vert; p.vs_size = sizeof lx_spv_fullscreen_vert;
    p.fs = lx_spv_sky_frag; p.fs_size = sizeof lx_spv_sky_frag;
    p.layout = R.scene_only_layout; p.color = R.hdr_format; p.depth = R.depth_format;
    R.sky = pipeline_create(&p);

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
    p.layout = R.tonemap_layout; p.color = VK_FORMAT_R8G8B8A8_UNORM;
    R.tonemap_ldr = pipeline_create(&p);

    /* white + flat normal defaults */
    ImageDesc d = {1, 1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false};
    uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255};
    image_create(&R.white, &d);
    image_upload_2d(&R.white, white, 4, false);
    image_create(&R.flat_normal, &d);
    image_upload_2d(&R.flat_normal, flat, 4, false);

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
    create_gi_images(2, 2, 2);
    write_scene_sets();
}

static void create_swap_pipelines(void) {
    if (R.tonemap_swap) vkDestroyPipeline(R.device, R.tonemap_swap, NULL);
    if (R.ui) vkDestroyPipeline(R.device, R.ui, NULL);
    PipeDesc p;
    memset(&p, 0, sizeof p);
    p.vs = lx_spv_fullscreen_vert; p.vs_size = sizeof lx_spv_fullscreen_vert;
    p.fs = lx_spv_tonemap_frag; p.fs_size = sizeof lx_spv_tonemap_frag;
    p.layout = R.tonemap_layout; p.color = R.swap_format;
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
    for (int c = 0; c < 3; c++) image_destroy(&R.gi[c]);
    image_destroy(&R.white);
    image_destroy(&R.flat_normal);
    VkPipeline pipes[] = {R.pbr, R.shadow, R.sky, R.grid, R.outline, R.tonemap_ldr, R.tonemap_swap, R.ui};
    for (int i = 0; i < 8; i++) if (pipes[i]) vkDestroyPipeline(R.device, pipes[i], NULL);
    VkPipelineLayout layouts[] = {R.pbr_layout, R.shadow_layout, R.scene_only_layout, R.outline_layout, R.tonemap_layout, R.ui_layout};
    for (int i = 0; i < 6; i++) vkDestroyPipelineLayout(R.device, layouts[i], NULL);
    vkDestroyDescriptorSetLayout(R.device, R.scene_layout, NULL);
    vkDestroyDescriptorSetLayout(R.device, R.material_layout, NULL);
    vkDestroyDescriptorSetLayout(R.device, R.tex_layout, NULL);
    vkDestroyDescriptorPool(R.device, R.desc_pool, NULL);
    vkDestroySampler(R.device, R.linear_repeat, NULL);
    vkDestroySampler(R.device, R.linear_clamp, NULL);
    vkDestroySampler(R.device, R.shadow_sampler, NULL);
    destroy_swapchain();
    vkDestroyCommandPool(R.device, R.pool, NULL);
    vkDestroyDevice(R.device, NULL);
    vkDestroySurfaceKHR(R.instance, R.surface, NULL);
    if (R.messenger) vkDestroyDebugUtilsMessengerEXT(R.instance, R.messenger, NULL);
    vkDestroyInstance(R.instance, NULL);
    lx_free(R.mat_sets);
    for (int i = 0; i < MAX_CASCADES; i++) { lx_free(R.shadow_lists[i].batches); lx_free(R.shadow_lists[i].inst); lx_free(R.shadow_lists[i].item_batch); }
    lx_free(R.main_list.batches); lx_free(R.main_list.inst); lx_free(R.main_list.item_batch);
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

static MeshGpu *mesh_gpu(LxMesh *m) {
    if (m->gpu) return m->gpu;
    MeshGpu *g = lx_calloc(1, sizeof *g);
    for (int l = 0; l < m->lod_count; l++) {
        g->vb[l] = upload_buffer(m->lods[l].vertices, sizeof(LxVertex) * (VkDeviceSize)m->lods[l].vertex_count, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        g->ib[l] = upload_buffer(m->lods[l].indices, sizeof(uint32_t) * (VkDeviceSize)m->lods[l].index_count, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }
    m->gpu = g;
    LX_VEC_PUSH(g_live_meshes, g_live_mesh_count, g_live_mesh_cap, m);
    return g;
}

void lx_renderer_mesh_release(LxMesh *m) {
    if (!m || !m->gpu || !R.device) return;
    vkDeviceWaitIdle(R.device);
    MeshGpu *g = m->gpu;
    for (int l = 0; l < LX_MAX_LODS; l++) { buffer_destroy(&g->vb[l]); buffer_destroy(&g->ib[l]); }
    lx_free(g);
    m->gpu = NULL;
    for (int i = 0; i < g_live_mesh_count; i++) if (g_live_meshes[i] == m) { g_live_meshes[i] = g_live_meshes[--g_live_mesh_count]; break; }
}

static TexGpu *texture_gpu(LxTexture *t, bool srgb) {
    if (t->gpu) return t->gpu;
    TexGpu *g = lx_calloc(1, sizeof *g);
    VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t mips = 1;
    bool can_blit = format_supports(fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if (can_blit) for (uint32_t s = (uint32_t)(t->width > t->height ? t->width : t->height); s > 1; s >>= 1) mips++;
    ImageDesc d = {(uint32_t)t->width, (uint32_t)t->height, 1, 1, mips, fmt,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false};
    image_create(&g->img, &d);
    image_upload_2d(&g->img, t->pixels, (size_t)t->width * (size_t)t->height * 4, mips > 1);
    t->gpu = g;
    LX_VEC_PUSH(g_live_tex, g_live_tex_count, g_live_tex_cap, t);
    return g;
}

void lx_renderer_texture_release(LxTexture *t) {
    if (!t || !t->gpu || !R.device) return;
    vkDeviceWaitIdle(R.device);
    for (int i = 0; i < R.mat_set_count; i++)
        if (R.mat_sets[i].albedo == t || R.mat_sets[i].normal == t) {
            vkFreeDescriptorSets(R.device, R.desc_pool, 1, &R.mat_sets[i].set);
            R.mat_sets[i--] = R.mat_sets[--R.mat_set_count];
        }
    TexGpu *g = t->gpu;
    image_destroy(&g->img);
    lx_free(g);
    t->gpu = NULL;
    for (int i = 0; i < g_live_tex_count; i++) if (g_live_tex[i] == t) { g_live_tex[i] = g_live_tex[--g_live_tex_count]; break; }
}

static void release_all_meshes_textures(void) {
    while (g_live_mesh_count) lx_renderer_mesh_release(g_live_meshes[0]);
    while (g_live_tex_count) lx_renderer_texture_release(g_live_tex[0]);
}

static VkDescriptorSet material_set(const LxMaterial *m) {
    LxTexture *albedo = m->albedo_map[0] ? lx_asset_texture(m->albedo_map) : NULL;
    LxTexture *normal = m->normal_map[0] ? lx_asset_texture(m->normal_map) : NULL;
    for (int i = 0; i < R.mat_set_count; i++)
        if (R.mat_sets[i].albedo == albedo && R.mat_sets[i].normal == normal) return R.mat_sets[i].set;
    MatSet ms = {albedo, normal, alloc_set(R.material_layout)};
    write_image(ms.set, 0, albedo ? texture_gpu(albedo, true)->img.view : R.white.view, R.linear_repeat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    write_image(ms.set, 1, normal ? texture_gpu(normal, false)->img.view : R.flat_normal.view, R.linear_repeat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
static void target_alloc(LxRenderTarget *rt, bool with_ldr) {
    float scale = lx_clampf(R.settings.render_scale, 0.25f, 2.0f);
    rt->iw = (int)((float)rt->w * scale); if (rt->iw < 1) rt->iw = 1;
    rt->ih = (int)((float)rt->h * scale); if (rt->ih < 1) rt->ih = 1;
    ImageDesc hd = {(uint32_t)rt->iw, (uint32_t)rt->ih, 1, 1, 1, R.hdr_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, false};
    image_create(&rt->hdr, &hd);
    ImageDesc dd = {(uint32_t)rt->iw, (uint32_t)rt->ih, 1, 1, 1, R.depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D, false};
    image_create(&rt->depth, &dd);
    rt->hdr_set = alloc_set(R.tex_layout);
    write_image(rt->hdr_set, 0, rt->hdr.view, R.linear_clamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
    image_destroy(&rt->hdr);
    image_destroy(&rt->depth);
    image_destroy(&rt->ldr);
    if (rt->hdr_set) vkFreeDescriptorSets(R.device, R.desc_pool, 1, &rt->hdr_set);
    rt->hdr_set = VK_NULL_HANDLE;
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

static void dl_add(DrawList *dl, LxMesh *mesh, int lod, const LxMaterial *mat, const float *model, LxVec4 tint) {
    int b = -1;
    for (int i = dl->nb - 1; i >= 0; i--)
        if (dl->batches[i].mesh == mesh && dl->batches[i].lod == lod && dl->batches[i].mat == mat) { b = i; break; }
    if (b < 0) {
        Batch nb = {mesh, lod, mat, 0, 0};
        LX_VEC_PUSH(dl->batches, dl->nb, dl->cb, nb);
        b = dl->nb - 1;
    }
    dl->batches[b].count++;
    if (dl->ni >= dl->ci) {
        dl->ci = dl->ci ? dl->ci * 2 : 1024;
        dl->inst = lx_realloc(dl->inst, sizeof(float) * 20 * (size_t)dl->ci);
        dl->item_batch = lx_realloc(dl->item_batch, sizeof(int) * (size_t)dl->ci);
    }
    float *dst = dl->inst + (size_t)dl->ni * 20;
    memcpy(dst, model, 64);
    dst[16] = tint.x; dst[17] = tint.y; dst[18] = tint.z; dst[19] = tint.w;
    dl->item_batch[dl->ni++] = b;
}

/* Group instances by batch into the frame's instance stream. Returns false if it does not fit. */
static bool dl_upload(DrawList *dl, FrameData *fr) {
    VkDeviceSize need = (VkDeviceSize)dl->ni * 80;
    if (fr->inst_used + need > fr->inst.size) { fr->inst_overflow = true; return false; }
    uint32_t base = (uint32_t)(fr->inst_used / 80);
    uint32_t off = base;
    for (int b = 0; b < dl->nb; b++) {
        dl->batches[b].first = off;
        off += dl->batches[b].count;
        dl->batches[b].count = 0;
    }
    float *stream = (float *)fr->inst.mapped;
    for (int i = 0; i < dl->ni; i++) {
        Batch *bt = &dl->batches[dl->item_batch[i]];
        memcpy(stream + (size_t)(bt->first + bt->count) * 20, dl->inst + (size_t)i * 20, 80);
        bt->count++;
    }
    fr->inst_used += need;
    return true;
}

static int pick_lod(const LxMesh *m, float screen_size) {
    if (!R.settings.lod_enabled) return 0;
    float bias = R.settings.lod_bias > 0.05f ? R.settings.lod_bias : 1.0f;
    for (int l = 0; l < m->lod_count; l++)
        if (screen_size * bias >= m->lods[l].screen_size) return l;
    return m->lod_count - 1;
}

static void draw_batches(VkCommandBuffer cmd, const DrawList *dl, VkBuffer inst, bool shading, VkDescriptorSet scene_set, uint32_t ubo_off) {
    for (int b = 0; b < dl->nb; b++) {
        const Batch *bt = &dl->batches[b];
        if (!bt->count) continue;
        MeshGpu *g = mesh_gpu(bt->mesh);
        VkBuffer vbs[2] = {g->vb[bt->lod].buf, inst};
        VkDeviceSize offs[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
        vkCmdBindIndexBuffer(cmd, g->ib[bt->lod].buf, 0, VK_INDEX_TYPE_UINT32);
        if (shading) {
            const LxMaterial *m = bt->mat;
            VkDescriptorSet sets[2] = {scene_set, material_set(m)};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pbr_layout, 0, 2, sets, 1, &ubo_off);
            float pc[12] = {m->albedo.x, m->albedo.y, m->albedo.z, m->albedo.w,
                            m->metallic, m->roughness, m->uv_scale > 0 ? m->uv_scale : 1.0f, m->normal_map[0] ? 1.0f : 0.0f,
                            m->emissive.x, m->emissive.y, m->emissive.z, m->emissive_strength};
            vkCmdPushConstants(cmd, R.pbr_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pc);
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

static void upload_gi(VkCommandBuffer cmd, FrameData *fr) {
    const LxGiVolume *gv = lx_gi_volume();
    if (!gv->valid || !gv->sh) return;
    if (gv->res[0] != R.gi_res[0] || gv->res[1] != R.gi_res[1] || gv->res[2] != R.gi_res[2]) {
        vkDeviceWaitIdle(R.device);
        create_gi_images(gv->res[0], gv->res[1], gv->res[2]);
        write_scene_sets();
    }
    if (fr->gi_uploaded_version == gv->version) return;
    size_t cells = (size_t)gv->res[0] * (size_t)gv->res[1] * (size_t)gv->res[2];
    uint16_t *dst = fr->gi_staging.mapped;
    for (int c = 0; c < 3; c++)
        for (size_t i = 0; i < cells; i++)
            for (int k = 0; k < 4; k++) dst[(size_t)c * cells * 4 + i * 4 + (size_t)k] = f2h(gv->sh[i * 12 + (size_t)c * 4 + (size_t)k]);
    for (int c = 0; c < 3; c++) {
        image_barrier(cmd, R.gi[c].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, 1, 1);
        VkBufferImageCopy region;
        memset(&region, 0, sizeof region);
        region.bufferOffset = (VkDeviceSize)c * cells * 8;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = (uint32_t)gv->res[0];
        region.imageExtent.height = (uint32_t)gv->res[1];
        region.imageExtent.depth = (uint32_t)gv->res[2];
        vkCmdCopyBufferToImage(cmd, fr->gi_staging.buf, R.gi[c].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        image_barrier(cmd, R.gi[c].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
    }
    fr->gi_uploaded_version = gv->version;
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

void lx_render_scene(LxScene *s, const LxRenderView *view, LxRenderTarget *target) {
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
    LxGraphicsSettings *gs = &R.settings;
    if (gs->shadow_resolution != R.shadow_res || gs->shadow_cascades != R.shadow_layers) {
        vkDeviceWaitIdle(R.device);
        int layers = gs->shadow_cascades < 1 ? 1 : (gs->shadow_cascades > MAX_CASCADES ? MAX_CASCADES : gs->shadow_cascades);
        gs->shadow_cascades = layers;
        create_shadow_map(gs->shadow_resolution < 256 ? 256 : gs->shadow_resolution, layers);
        write_scene_sets();
    }
    lx_scene_update_transforms(s);

    /* ---- GI ---- */
    lx_gi_update(s, gs);
    R.stats.gi_ms = lx_gi_last_ms();
    R.stats.gi_photons_traced = gs->gi_enabled ? lx_gi_last_photon_count() : 0;
    upload_gi(cmd, fr);

    /* ---- lights ---- */
    SceneUBO ubo;
    memset(&ubo, 0, sizeof ubo);
    float best_sun = -1.0f;
    LxVec3 sun_dir = lx_norm3(lx_v3(-0.4f, -1.0f, -0.3f));
    bool sun_shadows = false;
    int nl = 0;
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_LIGHT)) continue;
        const LxLight *l = &d->light;
        LxVec3 dir = lx_norm3(lx_mat4_dir(d->world, lx_v3(0, 0, -1)));
        LxVec3 col = lx_scale3(l->color, l->intensity);
        if (l->type == LX_LIGHT_DIRECTIONAL) {
            if (l->intensity > best_sun) {
                best_sun = l->intensity;
                sun_dir = dir;
                sun_shadows = l->cast_shadows;
                ubo.sun_color[0] = col.x; ubo.sun_color[1] = col.y; ubo.sun_color[2] = col.z; ubo.sun_color[3] = 1.0f;
            }
        } else if (nl < MAX_LIGHTS) {
            float *L = ubo.lights[nl++];
            L[0] = d->world.m[12]; L[1] = d->world.m[13]; L[2] = d->world.m[14]; L[3] = l->range > 0.01f ? l->range : 0.01f;
            L[4] = col.x; L[5] = col.y; L[6] = col.z; L[7] = l->type == LX_LIGHT_SPOT ? 2.0f : 1.0f;
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
    const LxEnvironment *env = &s->env;
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
    bool gi_on = gs->gi_enabled && gv->valid && R.gi_res[0] == gv->res[0] && R.gi_res[1] == gv->res[1] && R.gi_res[2] == gv->res[2];
    ubo.gi_origin[0] = gv->origin.x; ubo.gi_origin[1] = gv->origin.y; ubo.gi_origin[2] = gv->origin.z; ubo.gi_origin[3] = gi_on ? 1.0f : 0.0f;
    ubo.gi_cell[0] = gv->cell_size.x; ubo.gi_cell[1] = gv->cell_size.y; ubo.gi_cell[2] = gv->cell_size.z;
    ubo.gi_res[0] = (float)gv->res[0]; ubo.gi_res[1] = (float)gv->res[1]; ubo.gi_res[2] = (float)gv->res[2];
    ubo.counts[0] = nl; ubo.counts[1] = cascades; ubo.counts[2] = R.shadow_res;
    uint32_t ubo_off = fr->ubo_used * 4096;
    memcpy((uint8_t *)fr->ubo.mapped + ubo_off, &ubo, sizeof ubo);
    fr->ubo_used++;

    /* ---- culling, LOD and batching ---- */
    double t_cull = lx_time();
    LxFrustum frustum = lx_frustum_from_matrix(ubo.view_proj);
    LxFrustum cfr[MAX_CASCADES];
    for (int c = 0; c < cascades; c++) { cfr[c] = lx_frustum_from_matrix(cvp[c]); dl_reset(&R.shadow_lists[c]); }
    dl_reset(&R.main_list);
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
            LxMesh *mesh = lx_asset_mesh(d->mesh.mesh);
            const LxMaterial *mat = lx_asset_material(d->mesh.material);
            LxAabb wb = lx_aabb_transform(mesh->bounds, d->world);
            LxVec3 c = lx_aabb_center(wb);
            float radius = lx_len3(lx_aabb_extent(wb));
            if (lx_frustum_test_aabb(&frustum, wb)) {
                float dist = lx_maxf(lx_len3(lx_sub3(c, view->eye)), 1e-3f);
                dl_add(&R.main_list, mesh, pick_lod(mesh, radius * proj_scale / dist), mat, d->world.m, white);
            } else R.stats.instances_culled++;
            if (d->mesh.cast_shadows)
                for (int k = 0; k < cascades; k++)
                    if (lx_frustum_test_sphere(&cfr[k], c, radius))
                        dl_add(&R.shadow_lists[k], mesh, mesh->lod_count > 1 ? 1 : 0, mat, d->world.m, white);
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
                    dl_add(&R.main_list, mesh, pick_lod(mesh, r * proj_scale / dist), mat, m, tint);
                }
                if (shadow) dl_add(&R.shadow_lists[0], mesh, mesh->lod_count - 1, mat, m, tint);
            }
        }
    }
    bool ok = dl_upload(&R.main_list, fr);
    for (int c = 0; c < cascades; c++) ok = dl_upload(&R.shadow_lists[c], fr) && ok;
    R.stats.cull_ms = (float)((lx_time() - t_cull) * 1000.0);
    if (!ok) {
        /* instance stream full: draw nothing extra this frame, buffer grows next frame */
        dl_reset(&R.main_list);
        for (int c = 0; c < cascades; c++) dl_reset(&R.shadow_lists[c]);
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
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.shadow);
            vkCmdPushConstants(cmd, R.shadow_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, cvp[c].m);
            draw_batches(cmd, &R.shadow_lists[c], fr->inst.buf, false, VK_NULL_HANDLE, 0);
            vkCmdEndRendering(cmd);
        }
        image_barrier(cmd, R.shadow_map.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 1,
                      (uint32_t)R.shadow_layers);
    }

    /* ---- main HDR pass ---- */
    image_barrier(cmd, rt->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, 0, 1, 1);
    image_barrier(cmd, rt->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0, 1, 1);
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
    da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent.width = (uint32_t)rt->iw;
    ri.renderArea.extent.height = (uint32_t)rt->ih;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {0, 0, (float)rt->iw, (float)rt->ih, 0, 1};
    VkRect2D sc = {{0, 0}, {(uint32_t)rt->iw, (uint32_t)rt->ih}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.sky);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.scene_only_layout, 0, 1, &fr->scene_set, 1, &ubo_off);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pbr);
    draw_batches(cmd, &R.main_list, fr->inst.buf, true, fr->scene_set, ubo_off);
    if (view->highlight) {
        LxEntityData *d = lx_entity_data(s, view->highlight);
        if (d && (d->comps & LX_COMP_MESH) && fr->inst_used + 80 <= fr->inst.size) {
            LxMesh *mesh = lx_asset_mesh(d->mesh.mesh);
            float *dst = (float *)((uint8_t *)fr->inst.mapped + fr->inst_used);
            memcpy(dst, d->world.m, 64);
            dst[16] = dst[17] = dst[18] = dst[19] = 1.0f;
            uint32_t first = (uint32_t)(fr->inst_used / 80);
            fr->inst_used += 80;
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
    image_barrier(cmd, rt->hdr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);

    /* ---- tonemap ---- */
    float exposure[4] = {gs->exposure * env->exposure, 0, 0, 0};
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
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.tonemap_layout, 0, 1, &rt->hdr_set, 0, NULL);
        vkCmdPushConstants(cmd, R.tonemap_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, exposure);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        image_barrier(cmd, rt->ldr.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT, 0, 1, 1);
    } else {
        begin_swap_pass(cmd, false, lx_v4(0, 0, 0, 1));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.tonemap_swap);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, R.tonemap_layout, 0, 1, &rt->hdr_set, 0, NULL);
        vkCmdPushConstants(cmd, R.tonemap_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, exposure);
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
    out->view = lx_mat4_inverse(world);
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
