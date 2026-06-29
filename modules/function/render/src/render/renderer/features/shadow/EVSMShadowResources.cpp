#include <lux/engine/render/renderer/features/shadow/EVSMShadowResources.hpp>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cassert>
#include <cstring>

namespace lux::render
{
    void EVSMShadowResources::init(const InitInfo& info)
    {
        if (initialized_)
            return;

        device_                = info.device;
        allocator_             = info.allocator;
        atlas_page_resolution_ = std::max(info.atlas_page_resolution, 1u);
        atlas_page_count_      = std::max(info.atlas_page_count, 1u);
        frames_in_flight_      = std::min(std::max(info.frames_in_flight, 1u),
                                          static_cast<uint32_t>(config_ubos_.size()));

        // Moment image is the caster's color target → COLOR_ATTACHMENT + SAMPLED,
        // and also the FINAL blur target (separable blur writes back into it).
        createImage(moment_image_,  moment_alloc_,  moment_view_,  true);
        // Scratch is the blur ping-pong intermediate (STORAGE) + SAMPLED for the
        // next pass's read. The old third "blurred" image is gone — blur_v writes
        // its result back into moment_image_, halving-plus the atlas footprint
        // (3 images → 2, −⅓ VRAM) with no quality change.
        createImage(scratch_image_, scratch_alloc_, scratch_view_, false);

        createSampler();

        // Per-FIF ConfigUBO (small, persistently mapped).
        for (uint32_t fi = 0; fi < frames_in_flight_; ++fi)
        {
            VkBufferCreateInfo buf_ci{};
            buf_ci.sType  = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buf_ci.size   = sizeof(ConfigGPU);
            buf_ci.usage  = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

            VmaAllocationCreateInfo alloc_ci{};
            alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo alloc_info{};
            const VkResult br = vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci,
                                                &config_ubos_[fi],
                                                &config_ubo_allocs_[fi],
                                                &alloc_info);
            assert(br == VK_SUCCESS && "vmaCreateBuffer for EVSM config UBO failed");
            (void)br;
            config_ubo_mapped_[fi] = alloc_info.pMappedData;
        }

        // Seed defaults — caller can overwrite via writeConfig().
        // RGBA16F-safe exponents: see shadow_evsm_caster.frag. RGBA32F atlas
        // can push these higher (Frostbite uses 40/5).
        ConfigGPU seed{};
        seed.pos_exponent    = 5.0f;
        seed.neg_exponent    = 5.0f;
        seed.bleed_reduction = 0.2f;
        writeConfig(seed);

        initialized_ = true;
    }

    void EVSMShadowResources::shutdown()
    {
        if (!initialized_)
            return;

        for (uint32_t fi = 0; fi < frames_in_flight_; ++fi)
        {
            if (config_ubos_[fi] != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator_, config_ubos_[fi], config_ubo_allocs_[fi]);
        }
        config_ubos_       = {};
        config_ubo_allocs_ = {};
        config_ubo_mapped_ = {};

        if (sampler_       != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
        if (moment_view_   != VK_NULL_HANDLE) vkDestroyImageView(device_, moment_view_, nullptr);
        if (scratch_view_  != VK_NULL_HANDLE) vkDestroyImageView(device_, scratch_view_, nullptr);
        if (moment_image_  != VK_NULL_HANDLE) vmaDestroyImage(allocator_, moment_image_,  moment_alloc_);
        if (scratch_image_ != VK_NULL_HANDLE) vmaDestroyImage(allocator_, scratch_image_, scratch_alloc_);

        sampler_       = VK_NULL_HANDLE;
        moment_view_   = scratch_view_  = VK_NULL_HANDLE;
        moment_image_  = scratch_image_ = VK_NULL_HANDLE;
        moment_alloc_  = scratch_alloc_ = VK_NULL_HANDLE;

        frames_in_flight_ = 0;
        initialized_      = false;
    }

    VkBuffer EVSMShadowResources::configUBO(uint32_t frame_slot) const noexcept
    {
        if (frames_in_flight_ == 0) return VK_NULL_HANDLE;
        return config_ubos_[frame_slot % frames_in_flight_];
    }

    void EVSMShadowResources::writeConfig(const ConfigGPU& cfg)
    {
        for (uint32_t fi = 0; fi < frames_in_flight_; ++fi)
        {
            if (config_ubo_mapped_[fi] == nullptr) continue;
            std::memcpy(config_ubo_mapped_[fi], &cfg, sizeof(ConfigGPU));
            vmaFlushAllocation(allocator_, config_ubo_allocs_[fi], 0, sizeof(ConfigGPU));
        }
    }

    void EVSMShadowResources::createImage(VkImage& image,
                                          VmaAllocation& alloc,
                                          VkImageView& view,
                                          bool is_color_attachment_target)
    {
        VkImageCreateInfo img_ci{};
        img_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.imageType     = VK_IMAGE_TYPE_2D;
        img_ci.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        img_ci.extent        = {atlas_page_resolution_, atlas_page_resolution_, 1};
        img_ci.mipLevels     = 1;
        img_ci.arrayLayers   = atlas_page_count_;
        img_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        if (is_color_attachment_target)
            img_ci.usage    |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        img_ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        const VkResult ir = vmaCreateImage(allocator_, &img_ci, &alloc_ci,
                                           &image, &alloc, nullptr);
        assert(ir == VK_SUCCESS && "vmaCreateImage for EVSM atlas failed");
        (void)ir;

        VkImageViewCreateInfo view_ci{};
        view_ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image                           = image;
        view_ci.viewType                        = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view_ci.format                          = VK_FORMAT_R16G16B16A16_SFLOAT;
        view_ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        view_ci.subresourceRange.baseMipLevel   = 0;
        view_ci.subresourceRange.levelCount     = 1;
        view_ci.subresourceRange.baseArrayLayer = 0;
        view_ci.subresourceRange.layerCount     = atlas_page_count_;

        const VkResult vr = vkCreateImageView(device_, &view_ci, nullptr, &view);
        assert(vr == VK_SUCCESS && "vkCreateImageView for EVSM atlas failed");
        (void)vr;
    }

    void EVSMShadowResources::createSampler()
    {
        // EVSM samples regular RGBA16F moments, not depth. No compare op.
        // Linear filtering integrates beautifully with the pre-filtered
        // moments — that's the whole VSM/EVSM point.
        VkSamplerCreateInfo samp_ci{};
        samp_ci.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samp_ci.magFilter     = VK_FILTER_LINEAR;
        samp_ci.minFilter     = VK_FILTER_LINEAR;
        samp_ci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_ci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_ci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_ci.borderColor   = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        samp_ci.compareEnable = VK_FALSE;
        samp_ci.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;

        const VkResult r = vkCreateSampler(device_, &samp_ci, nullptr, &sampler_);
        assert(r == VK_SUCCESS && "vkCreateSampler for EVSM sampler failed");
        (void)r;
    }

} // namespace lux::render
