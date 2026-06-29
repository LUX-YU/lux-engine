// ============================================================================
//  hzb_pyramid_test.cpp  —  P2 HZB Stage A self-check
//
//  Numerically verifies the Hi-Z (max-Z) pyramid build:
//    1. Upload a known depth pattern (a unique per-texel ramp) into an R32
//       source image.
//    2. HzbResources::recordBuild downsamples it into the R32 mip chain
//       (mip0 = copy of depth, mip k = 2x2 max of mip k-1).
//    3. Read every mip back and assert each texel == the max of its source
//       region — i.e. the CPU reference pyramid built with the same 2x2-max +
//       NPOT-clamp rule the shader uses. The 1x1 tail must equal the global max.
//
//  This pins the riskiest part of Stage A: the per-mip barriers + the max-Z
//  direction (a min-Z mistake would fail every level). Uses render's
//  DeviceContext for the VMA allocator (HzbResources allocates through it, so
//  the allocator must be the SAME VMA build — a raw test-side allocator is UB).
//  The compute pipeline is built raw from an embedded snapshot of
//  hzb/downsample.comp (the live render build validates the real shader).
//
//  Self-checking: 0 = PASS (or skip if no Vulkan device), 1 = FAIL.
// ============================================================================

#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/resources/hzb/HzbResources.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace lux::render;

// Snapshot of hzb/downsample.comp, glslc --target-env=vulkan1.2 -O -mfmt=c.
// Regenerate if the shader changes; the live render build validates the real one.
static const uint32_t kHzbDownsampleSpv[] = {
0x07230203,0x00010500,0x000d000b,0x0000009d,0x00000000,0x00020011,0x00000001,0x0006000b,
0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
0x000a000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x0000000c,0x00000016,0x00000036,
0x0000005c,0x00000085,0x00060010,0x00000004,0x00000011,0x00000008,0x00000008,0x00000001,
0x00040047,0x0000000c,0x0000000b,0x0000001c,0x00030047,0x00000014,0x00000002,0x00050048,
0x00000014,0x00000000,0x00000023,0x00000000,0x00050048,0x00000014,0x00000001,0x00000023,
0x00000004,0x00050048,0x00000014,0x00000002,0x00000023,0x00000008,0x00050048,0x00000014,
0x00000003,0x00000023,0x0000000c,0x00050048,0x00000014,0x00000004,0x00000023,0x00000010,
0x00050048,0x00000014,0x00000005,0x00000023,0x00000014,0x00040047,0x00000036,0x00000021,
0x00000000,0x00040047,0x00000036,0x00000022,0x00000001,0x00040047,0x0000005c,0x00000021,
0x00000000,0x00040047,0x0000005c,0x00000022,0x00000000,0x00030047,0x00000085,0x00000019,
0x00040047,0x00000085,0x00000021,0x00000001,0x00040047,0x00000085,0x00000022,0x00000000,
0x00040047,0x0000008c,0x0000000b,0x00000019,0x00020013,0x00000002,0x00030021,0x00000003,
0x00000002,0x00040015,0x00000006,0x00000020,0x00000000,0x00040017,0x00000007,0x00000006,
0x00000002,0x00040017,0x0000000a,0x00000006,0x00000003,0x00040020,0x0000000b,0x00000001,
0x0000000a,0x0004003b,0x0000000b,0x0000000c,0x00000001,0x00020014,0x0000000f,0x0004002b,
0x00000006,0x00000010,0x00000000,0x0008001e,0x00000014,0x00000006,0x00000006,0x00000006,
0x00000006,0x00000006,0x00000006,0x00040020,0x00000015,0x00000009,0x00000014,0x0004003b,
0x00000015,0x00000016,0x00000009,0x00040015,0x00000017,0x00000020,0x00000001,0x0004002b,
0x00000017,0x00000018,0x00000000,0x00040020,0x00000019,0x00000009,0x00000006,0x0004002b,
0x00000006,0x00000020,0x00000001,0x0004002b,0x00000017,0x00000023,0x00000001,0x0004002b,
0x00000017,0x0000002b,0x00000004,0x00030016,0x00000031,0x00000020,0x00090019,0x00000034,
0x00000031,0x00000001,0x00000000,0x00000000,0x00000000,0x00000001,0x00000000,0x00040020,
0x00000035,0x00000000,0x00000034,0x0004003b,0x00000035,0x00000036,0x00000000,0x00040017,
0x00000039,0x00000017,0x00000002,0x00040017,0x0000003b,0x00000031,0x00000004,0x0004002b,
0x00000006,0x00000042,0x00000002,0x0004002b,0x00000017,0x0000004b,0x00000002,0x0004002b,
0x00000017,0x00000055,0x00000003,0x0004003b,0x00000035,0x0000005c,0x00000000,0x00090019,
0x00000083,0x00000031,0x00000001,0x00000000,0x00000000,0x00000000,0x00000002,0x00000003,
0x00040020,0x00000084,0x00000000,0x00000083,0x0004003b,0x00000084,0x00000085,0x00000000,
0x0004002b,0x00000006,0x0000008b,0x00000008,0x0006002c,0x0000000a,0x0000008c,0x0000008b,
0x0000008b,0x00000020,0x0005002c,0x00000007,0x0000009c,0x00000042,0x00000042,0x00050036,
0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x000300f7,0x0000008d,
0x00000000,0x000300fb,0x00000010,0x0000008e,0x000200f8,0x0000008e,0x0004003d,0x0000000a,
0x0000000d,0x0000000c,0x0007004f,0x00000007,0x0000000e,0x0000000d,0x0000000d,0x00000000,
0x00000001,0x00050051,0x00000006,0x00000013,0x0000000d,0x00000000,0x00050041,0x00000019,
0x0000001a,0x00000016,0x00000018,0x0004003d,0x00000006,0x0000001b,0x0000001a,0x000500ae,
0x0000000f,0x0000001c,0x00000013,0x0000001b,0x000400a8,0x0000000f,0x0000001d,0x0000001c,
0x000300f7,0x0000001f,0x00000000,0x000400fa,0x0000001d,0x0000001e,0x0000001f,0x000200f8,
0x0000001e,0x00050051,0x00000006,0x00000022,0x0000000d,0x00000001,0x00050041,0x00000019,
0x00000024,0x00000016,0x00000023,0x0004003d,0x00000006,0x00000025,0x00000024,0x000500ae,
0x0000000f,0x00000026,0x00000022,0x00000025,0x000200f9,0x0000001f,0x000200f8,0x0000001f,
0x000700f5,0x0000000f,0x00000027,0x0000001c,0x0000008e,0x00000026,0x0000001e,0x000300f7,
0x00000029,0x00000000,0x000400fa,0x00000027,0x00000028,0x00000029,0x000200f8,0x00000028,
0x000200f9,0x0000008d,0x000200f8,0x00000029,0x00050041,0x00000019,0x0000002c,0x00000016,
0x0000002b,0x0004003d,0x00000006,0x0000002d,0x0000002c,0x000500ab,0x0000000f,0x0000002e,
0x0000002d,0x00000010,0x000300f7,0x00000030,0x00000000,0x000400fa,0x0000002e,0x0000002f,
0x0000003e,0x000200f8,0x0000002f,0x0004003d,0x00000034,0x00000037,0x00000036,0x0004007c,
0x00000039,0x0000003a,0x0000000e,0x0007005f,0x0000003b,0x0000003c,0x00000037,0x0000003a,
0x00000002,0x00000018,0x00050051,0x00000031,0x0000003d,0x0000003c,0x00000000,0x000200f9,
0x00000030,0x000200f8,0x0000003e,0x00050084,0x00000007,0x00000044,0x0000000e,0x0000009c,
0x0004007c,0x00000039,0x00000045,0x00000044,0x00050051,0x00000017,0x00000049,0x00000045,
0x00000000,0x00050080,0x00000017,0x0000004a,0x00000049,0x00000023,0x00050041,0x00000019,
0x0000004c,0x00000016,0x0000004b,0x0004003d,0x00000006,0x0000004d,0x0000004c,0x0004007c,
0x00000017,0x0000004e,0x0000004d,0x00050082,0x00000017,0x0000004f,0x0000004e,0x00000023,
0x0007000c,0x00000017,0x00000050,0x00000001,0x00000027,0x0000004a,0x0000004f,0x00050051,
0x00000017,0x00000053,0x00000045,0x00000001,0x00050080,0x00000017,0x00000054,0x00000053,
0x00000023,0x00050041,0x00000019,0x00000056,0x00000016,0x00000055,0x0004003d,0x00000006,
0x00000057,0x00000056,0x0004007c,0x00000017,0x00000058,0x00000057,0x00050082,0x00000017,
0x00000059,0x00000058,0x00000023,0x0007000c,0x00000017,0x0000005a,0x00000001,0x00000027,
0x00000054,0x00000059,0x0004003d,0x00000034,0x0000005d,0x0000005c,0x0007005f,0x0000003b,
0x00000063,0x0000005d,0x00000045,0x00000002,0x00000018,0x00050051,0x00000031,0x00000064,
0x00000063,0x00000000,0x00050050,0x00000039,0x0000006a,0x00000050,0x00000053,0x0007005f,
0x0000003b,0x0000006b,0x0000005d,0x0000006a,0x00000002,0x00000018,0x00050051,0x00000031,
0x0000006c,0x0000006b,0x00000000,0x00050050,0x00000039,0x00000072,0x00000049,0x0000005a,
0x0007005f,0x0000003b,0x00000073,0x0000005d,0x00000072,0x00000002,0x00000018,0x00050051,
0x00000031,0x00000074,0x00000073,0x00000000,0x00050050,0x00000039,0x00000079,0x00000050,
0x0000005a,0x0007005f,0x0000003b,0x0000007a,0x0000005d,0x00000079,0x00000002,0x00000018,
0x00050051,0x00000031,0x0000007b,0x0000007a,0x00000000,0x0007000c,0x00000031,0x0000007e,
0x00000001,0x00000028,0x00000064,0x0000006c,0x0007000c,0x00000031,0x00000081,0x00000001,
0x00000028,0x00000074,0x0000007b,0x0007000c,0x00000031,0x00000082,0x00000001,0x00000028,
0x0000007e,0x00000081,0x000200f9,0x00000030,0x000200f8,0x00000030,0x000700f5,0x00000031,
0x0000009b,0x0000003d,0x0000002f,0x00000082,0x0000003e,0x0004003d,0x00000083,0x00000086,
0x00000085,0x0004007c,0x00000039,0x00000088,0x0000000e,0x00070050,0x0000003b,0x0000008a,
0x0000009b,0x0000009b,0x0000009b,0x0000009b,0x00040063,0x00000086,0x00000088,0x0000008a,
0x000200f9,0x0000008d,0x000200f8,0x0000008d,0x000100fd,0x00010038
};

static int   g_fail = 0;
static void check(bool c, const char* n)
{ std::printf(c ? "  [PASS] %s\n" : "  [FAIL] %s\n", n); if (!c) ++g_fail; }

static uint32_t findMemoryType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== hzb_pyramid_test (P2 HZB Stage A) ===\n");

    std::unique_ptr<InstanceContext> inst;
    try { inst = std::make_unique<InstanceContext>(std::vector<const char*>{}, nullptr); }
    catch (...) { std::printf("No Vulkan instance. Skipping.\n"); return 0; }

    DeviceContext dev{*inst};
    if (auto r = dev.init(EPhysicalDeviceSelectionPolicy::INTEGRATED_GPU_PREFERRED); !r)
    { std::printf("No Vulkan device. Skipping.\n"); return 0; }

    VkDevice         device = dev.logicalDevice();
    VkPhysicalDevice phys   = dev.physicalDevice();
    VkQueue          queue  = dev.graphicsQueue();
    const uint32_t   qfi    = dev.graphicsQueueFamilyIndex();

    constexpr uint32_t W = 64, H = 64;
    const uint32_t     N = W * H;

    // ── Known depth pattern: a unique per-texel ramp (so max(block) is exact) ──
    std::vector<float> pattern(N);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
            pattern[y * W + x] = static_cast<float>(x + y * W);

    // ── HZB pyramid (R32 mip chain, allocated through render's VMA) ──
    HzbResources hzb;
    {
        HzbResources::InitInfo ri{};
        ri.device = device; ri.allocator = dev.vmaAllocator(); ri.width = W; ri.height = H;
        check(hzb.init(ri), "HzbResources::init");
        if (!hzb.initialized()) return 1;
    }
    const uint32_t mips = hzb.mipCount();

    // ── Depth source image (R32, SAMPLED|TRANSFER_DST) via core Vulkan ──
    VkImage depth_img = VK_NULL_HANDLE; VkDeviceMemory depth_mem = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    {
        VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R32_SFLOAT;
        ci.extent = { W, H, 1 }; ci.mipLevels = 1; ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(device, &ci, nullptr, &depth_img);
        VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(device, depth_img, &mr);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &ai, nullptr, &depth_mem);
        vkBindImageMemory(device, depth_img, depth_mem, 0);
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = depth_img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R32_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(device, &vi, nullptr, &depth_view);
    }

    // ── Staging (upload) + readback host buffers via core Vulkan ──
    auto makeHostBuffer = [&](VkDeviceSize bytes, VkBufferUsageFlags usage,
                              VkBuffer& buf, VkDeviceMemory& mem)
    {
        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = bytes; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bi, nullptr, &buf);
        VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(device, buf, &mr);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(phys, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &ai, nullptr, &mem);
        vkBindBufferMemory(device, buf, mem, 0);
    };

    VkBuffer staging = VK_NULL_HANDLE; VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    makeHostBuffer(N * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, staging_mem);
    { void* p = nullptr; vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0, &p);
      std::memcpy(p, pattern.data(), N * sizeof(float)); vkUnmapMemory(device, staging_mem); }

    // Readback buffer holds every mip, packed at running offsets.
    std::vector<uint32_t> mip_off(mips), mip_w(mips), mip_h(mips);
    uint32_t total = 0;
    for (uint32_t k = 0; k < mips; ++k)
    { mip_w[k] = hzb.mipWidth(k); mip_h[k] = hzb.mipHeight(k); mip_off[k] = total; total += mip_w[k] * mip_h[k]; }
    VkBuffer readback = VK_NULL_HANDLE; VkDeviceMemory readback_mem = VK_NULL_HANDLE;
    makeHostBuffer(total * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback, readback_mem);

    // ── Descriptor layouts / pipeline (raw, from the embedded SPIR-V) ──
    auto makeLayout = [&](std::vector<VkDescriptorSetLayoutBinding> b) -> VkDescriptorSetLayout
    {
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = static_cast<uint32_t>(b.size()); ci.pBindings = b.data();
        VkDescriptorSetLayout l = VK_NULL_HANDLE; vkCreateDescriptorSetLayout(device, &ci, nullptr, &l); return l;
    };
    VkDescriptorSetLayout set0 = makeLayout({
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } });
    VkDescriptorSetLayout set1 = makeLayout({
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } });

    VkShaderModule mod = VK_NULL_HANDLE;
    { VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
      ci.codeSize = sizeof(kHzbDownsampleSpv); ci.pCode = kHzbDownsampleSpv;
      vkCreateShaderModule(device, &ci, nullptr, &mod); }

    VkPipelineLayout pl = VK_NULL_HANDLE;
    { const VkDescriptorSetLayout sls[2] = { set0, set1 };
      VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0,
          static_cast<uint32_t>(sizeof(HzbResources::BuildPushConstants)) };
      VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
      ci.setLayoutCount = 2; ci.pSetLayouts = sls; ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pc;
      vkCreatePipelineLayout(device, &ci, nullptr, &pl); }

    VkPipeline pipe = VK_NULL_HANDLE;
    { VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
      ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
      ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = mod; ci.stage.pName = "main";
      ci.layout = pl;
      check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) == VK_SUCCESS,
            "vkCreateComputePipelines(hzb downsample)"); }

    // ── Descriptor pool + sets (mips set-0 + one set-1 depth) ──
    VkDescriptorPool pool = VK_NULL_HANDLE;
    { VkDescriptorPoolSize sizes[2] = {        // 2 slots of mip set-0 + 1 depth
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2u * mips + 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2u * mips } };
      VkDescriptorPoolCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
      ci.maxSets = 2u * mips + 1u; ci.poolSizeCount = 2; ci.pPoolSizes = sizes;
      vkCreateDescriptorPool(device, &ci, nullptr, &pool); }

    std::vector<VkDescriptorSet> mip_sets(mips, VK_NULL_HANDLE);
    for (uint32_t k = 0; k < mips; ++k)
    { VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
      ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &set0;
      vkAllocateDescriptorSets(device, &ai, &mip_sets[k]); }
    hzb.writeBuildDescriptors(device, 0u, mip_sets.data(), mips);

    VkDescriptorSet depth_set = VK_NULL_HANDLE;
    { VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
      ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &set1;
      vkAllocateDescriptorSets(device, &ai, &depth_set);
      VkDescriptorImageInfo di{}; di.imageView = depth_view; di.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
      w.dstSet = depth_set; w.dstBinding = 0; w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      w.descriptorCount = 1; w.pImageInfo = &di;
      vkUpdateDescriptorSets(device, 1, &w, 0, nullptr); }

    // ── Command buffer: upload depth → build pyramid → copy every mip out ──
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    { VkCommandPoolCreateInfo ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
      ci.queueFamilyIndex = qfi; vkCreateCommandPool(device, &ci, nullptr, &cmd_pool); }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    { VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
      ai.commandPool = cmd_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
      vkAllocateCommandBuffers(device, &ai, &cmd); }

    auto imgBarrier = [](VkImage img, uint32_t base, uint32_t count,
                         VkImageLayout o, VkImageLayout n, VkAccessFlags sa, VkAccessFlags da)
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = o; b.newLayout = n;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1 };
        return b;
    };

    { VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; vkBeginCommandBuffer(cmd, &bi); }

    { VkImageMemoryBarrier b = imgBarrier(depth_img, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &b); }
    { VkBufferImageCopy c{}; c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      c.imageExtent = { W, H, 1 };
      vkCmdCopyBufferToImage(cmd, staging, depth_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c); }
    { VkImageMemoryBarrier b = imgBarrier(depth_img, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &b); }

    hzb.recordBuild(cmd, pl, 0u, mip_sets.data(), mips, pipe, depth_set);

    { VkImageMemoryBarrier b = imgBarrier(hzb.image(0u), 0, mips, VK_IMAGE_LAYOUT_GENERAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &b); }
    for (uint32_t k = 0; k < mips; ++k)
    { VkBufferImageCopy c{}; c.bufferOffset = static_cast<VkDeviceSize>(mip_off[k]) * sizeof(float);
      c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, k, 0, 1 };
      c.imageExtent = { mip_w[k], mip_h[k], 1 };
      vkCmdCopyImageToBuffer(cmd, hzb.image(0u), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &c); }

    vkEndCommandBuffer(cmd);
    { VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
      vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue); }

    // ── Verify against the CPU reference pyramid (same 2x2-max + NPOT clamp) ──
    std::vector<float> gpu(total);
    { void* p = nullptr; vkMapMemory(device, readback_mem, 0, VK_WHOLE_SIZE, 0, &p);
      std::memcpy(gpu.data(), p, total * sizeof(float)); vkUnmapMemory(device, readback_mem); }

    std::vector<std::vector<float>> ref(mips);
    ref[0] = pattern;
    for (uint32_t k = 1; k < mips; ++k)
    {
        ref[k].resize(mip_w[k] * mip_h[k]);
        const uint32_t pw = mip_w[k - 1], ph = mip_h[k - 1];
        for (uint32_t y = 0; y < mip_h[k]; ++y)
            for (uint32_t x = 0; x < mip_w[k]; ++x)
            {
                const uint32_t x0 = x * 2, y0 = y * 2;
                const uint32_t x1 = std::min(x0 + 1, pw - 1), y1 = std::min(y0 + 1, ph - 1);
                const auto& s = ref[k - 1];
                ref[k][y * mip_w[k] + x] = std::max(std::max(s[y0 * pw + x0], s[y0 * pw + x1]),
                                                    std::max(s[y1 * pw + x0], s[y1 * pw + x1]));
            }
    }

    uint32_t mismatches = 0;
    for (uint32_t k = 0; k < mips; ++k)
        for (uint32_t i = 0; i < mip_w[k] * mip_h[k]; ++i)
            if (gpu[mip_off[k] + i] != ref[k][i]) ++mismatches;

    check(mismatches == 0, "every mip texel == CPU max-Z reference (all levels)");
    check(gpu[mip_off[0]] == pattern[0], "mip0 is the depth copy");
    const float global_max = pattern[N - 1];   // ramp peak = bottom-right texel
    check(gpu[mip_off[mips - 1]] == global_max, "1x1 tail == global max depth");
    std::printf("  mips=%u  total_texels=%u  mismatches=%u  global_max=%.0f\n",
                mips, total, mismatches, global_max);

    // ── Double-buffer check: build the OTHER ping-pong slot and verify it ──
    // produces the same correct pyramid INDEPENDENTLY (proves both images are
    // wired, not just slot 0). Same depth input ⇒ same reference pyramid.
    {
        std::vector<VkDescriptorSet> mip_sets1(mips, VK_NULL_HANDLE);
        for (uint32_t k = 0; k < mips; ++k)
        { VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
          ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &set0;
          vkAllocateDescriptorSets(device, &ai, &mip_sets1[k]); }
        hzb.writeBuildDescriptors(device, 1u, mip_sets1.data(), mips);

        VkCommandBuffer cmd1 = VK_NULL_HANDLE;
        { VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
          ai.commandPool = cmd_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
          vkAllocateCommandBuffers(device, &ai, &cmd1); }
        { VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
          bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; vkBeginCommandBuffer(cmd1, &bi); }
        // depth is already GENERAL from the slot-0 pass; reuse depth_set.
        hzb.recordBuild(cmd1, pl, 1u, mip_sets1.data(), mips, pipe, depth_set);
        { VkImageMemoryBarrier b = imgBarrier(hzb.image(1u), 0, mips, VK_IMAGE_LAYOUT_GENERAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
          vkCmdPipelineBarrier(cmd1, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              0, 0, nullptr, 0, nullptr, 1, &b); }
        for (uint32_t k = 0; k < mips; ++k)
        { VkBufferImageCopy c{}; c.bufferOffset = static_cast<VkDeviceSize>(mip_off[k]) * sizeof(float);
          c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, k, 0, 1 };
          c.imageExtent = { mip_w[k], mip_h[k], 1 };
          vkCmdCopyImageToBuffer(cmd1, hzb.image(1u), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &c); }
        vkEndCommandBuffer(cmd1);
        { VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmd1;
          vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue); }

        std::vector<float> gpu1(total);
        { void* p = nullptr; vkMapMemory(device, readback_mem, 0, VK_WHOLE_SIZE, 0, &p);
          std::memcpy(gpu1.data(), p, total * sizeof(float)); vkUnmapMemory(device, readback_mem); }
        uint32_t mismatches1 = 0;
        for (uint32_t k = 0; k < mips; ++k)
            for (uint32_t i = 0; i < mip_w[k] * mip_h[k]; ++i)
                if (gpu1[mip_off[k] + i] != ref[k][i]) ++mismatches1;
        check(mismatches1 == 0, "second ping-pong slot builds the same pyramid independently");
        std::printf("  slot1 mismatches=%u\n", mismatches1);
    }

    // ── Teardown ──
    vkDeviceWaitIdle(device);
    vkDestroyCommandPool(device, cmd_pool, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipeline(device, pipe, nullptr);
    vkDestroyPipelineLayout(device, pl, nullptr);
    vkDestroyShaderModule(device, mod, nullptr);
    vkDestroyDescriptorSetLayout(device, set0, nullptr);
    vkDestroyDescriptorSetLayout(device, set1, nullptr);
    vkDestroyBuffer(device, readback, nullptr); vkFreeMemory(device, readback_mem, nullptr);
    vkDestroyBuffer(device, staging, nullptr);  vkFreeMemory(device, staging_mem, nullptr);
    vkDestroyImageView(device, depth_view, nullptr);
    vkDestroyImage(device, depth_img, nullptr); vkFreeMemory(device, depth_mem, nullptr);
    hzb.destroy();

    std::printf("=== hzb_pyramid_test %s (fails=%d) ===\n", g_fail == 0 ? "PASSED" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
