#include <lux/engine/render/resources/TextureResources.hpp>
#include <cassert>

namespace lux::render
{
    // ------------------------------------------------------------------
    // Shared descriptor pool + set for both sampler2D[] (binding 0) and
    // samplerCube[] (binding 1) in the same descriptor set (set 2).
    // ------------------------------------------------------------------
    void TextureResources::createSharedPoolAndSet(
        VkDescriptorSetLayout layout,
        uint32_t tex2d_max,
        uint32_t cube_max)
    {
        auto& device = dc_->logicalDevice();
        // Exactly ONE set is ever allocated here — both BCS instances run in
        // external-set mode where the set never reallocates (retired-set arrays
        // were replaced by the DeferredDestroyQueue; the old kRetiredSetsMax
        // constant no longer exists). The previous 5x reservation
        // ((tex2d_max+cube_max)*5, maxSets=5) was dead budget. (P-1)
        constexpr uint32_t kRetiredMax = 0;

        // Pool sized for the single active set's two bindings.
        VkDescriptorPoolSize ps{};
        ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps.descriptorCount = (tex2d_max + cube_max) * (kRetiredMax + 1);

        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
                  | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets      = kRetiredMax + 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes    = &ps;
        VK_CHECK(vkCreateDescriptorPool(device, &pci, nullptr, &shared_pool_));

        // Allocate one set — variable descriptor count applies to binding 1 (cube)
        uint32_t var_count = cube_max;
        VkDescriptorSetVariableDescriptorCountAllocateInfo vci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
        vci.descriptorSetCount = 1;
        vci.pDescriptorCounts  = &var_count;

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool     = shared_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &layout;
        ai.pNext              = &vci;
        VK_CHECK(vkAllocateDescriptorSets(device, &ai, &shared_set_));
    }

    bool TextureResources::init(
            const InitInfo &info
    )
    {
        assert(info.device_context && info.graphics_queue && info.upload_cmd_pool);

        dc_ = info.device_context;
        queue_ = info.graphics_queue;
        upload_pool_ = info.upload_cmd_pool;
        slices_ = info.slices;

        combined_ci_ = info.combined_ci;

        default_sampler_ci_ = info.default_sampler_ci;
        if (default_sampler_ci_.sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO)
        {
            default_sampler_ci_ = {};
            default_sampler_ci_.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            default_sampler_ci_.magFilter = VK_FILTER_LINEAR;
            default_sampler_ci_.minFilter = VK_FILTER_LINEAR;
            default_sampler_ci_.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            default_sampler_ci_.addressModeU = default_sampler_ci_.addressModeV = default_sampler_ci_.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            default_sampler_ci_.maxLod = VK_LOD_CLAMP_NONE;
        }

        // -- Create shared pool + set for dual-binding texture set --
        const uint32_t tex2d_max = combined_ci_.layout_max_capacity;
        const uint32_t cube_max  = info.cube_max_capacity;
        createSharedPoolAndSet(combined_ci_.descriptor_set_layout, tex2d_max, cube_max);

        // -- Init 2D texture bindless set (binding 0, external set) --
        BCInitInfo ci_2d          = combined_ci_;
        ci_2d.external_pool       = shared_pool_;
        ci_2d.external_set        = shared_set_;
        ci_2d.initial_capacity    = tex2d_max;   // fixed (no reallocation in external mode)
        ci_2d.layout_max_capacity = tex2d_max;
        ci_2d.frames_in_flight    = slices_;
        if (!combined_.init(ci_2d))
            return false;

        // -- Init cube texture bindless set (binding 1, external set) --
        BCInitInfo ci_cube            = combined_ci_;
        ci_cube.binding               = static_cast<uint32_t>(ETextureSetBindings::CUBE_TEXTURES);
        ci_cube.view_type             = VK_IMAGE_VIEW_TYPE_CUBE;
        ci_cube.generate_mipmaps      = false;
        ci_cube.initial_capacity      = cube_max;
        ci_cube.layout_max_capacity   = cube_max;
        ci_cube.external_pool         = shared_pool_;
        ci_cube.external_set          = shared_set_;
        ci_cube.frames_in_flight      = slices_;
        if (!combined_cube_.init(ci_cube))
            return false;

        // Fallback texture (2D)
        lux::rdesc::Texture fb = info.fallback_pixel.value_or(makeDefaultWhite());
        SlotHandle fallback = combined_.addTexture(fb, &default_sampler_ci_);
        // Flush immediately so the fallback texture is ready before any rendering
        combined_.flushUploads();

        fallback_bindless_index_ = fallback.index;

        initialized_ = true;
        return true;
    }

    void TextureResources::shutdown()
    {
        if (!initialized_) return;
        initialized_ = false;

        combined_.shutdown();
        combined_cube_.shutdown();

        // Destroy shared pool (after BindlessCombinedSet instances release references)
        if (shared_pool_ && dc_)
        {
            auto& device = dc_->logicalDevice();
            if (shared_set_)
            {
                vkFreeDescriptorSets(device, shared_pool_, 1, &shared_set_);
                shared_set_ = VK_NULL_HANDLE;
            }
            vkDestroyDescriptorPool(device, shared_pool_, nullptr);
            shared_pool_ = VK_NULL_HANDLE;
        }
    }

    Expected<TextureHandle> 
    TextureResources::submit(const lux::rdesc::Texture &cpu,
        const VkSamplerCreateInfo *opt_sampler,
        VkFormat fmt,
        bool generate_mips)
    {
        const VkSamplerCreateInfo& sci = opt_sampler ? *opt_sampler : default_sampler_ci_;

        SlotHandle sh = combined_.addTexture(cpu, &sci, fmt, generate_mips);
        if (!sh.valid())
            return lux::cxx::unexpected(make_error_code(ERenderError::SsboFull));
        TextureHandle h{sh.index, sh.gen};
        return h;
    }

    bool TextureResources::remove(TextureHandle h)
    {
        return combined_.removeTexture(SlotHandle{h.index, h.gen});
    }

    bool TextureResources::removeCube(TextureHandle h)
    {
        return combined_cube_.removeTexture(SlotHandle{h.index, h.gen});
    }

    Expected<TextureHandle>
    TextureResources::submitCube(
        const lux::rdesc::Texture faces[6],
        const VkSamplerCreateInfo *opt_sampler,
        VkFormat fmt)
    {
        const VkSamplerCreateInfo& sci = opt_sampler ? *opt_sampler : default_sampler_ci_;

        SlotHandle sh = combined_cube_.addCubeTexture(faces, &sci, fmt);
        if (!sh.valid())
            return lux::cxx::unexpected(make_error_code(ERenderError::SsboFull));
        TextureHandle h{sh.index, sh.gen};
        return h;
    }

    lux::rdesc::Texture TextureResources::makeDefaultWhite()
    {
        static uint8_t white[4] = {0xFF, 0xFF, 0xFF, 0xFF};

        lux::rdesc::TextureInfo info{};
        info.width = 1;
        info.height = 1;
        info.channel = 4;
        info.pixel_format = lux::rdesc::ETexturePixelFormat::RGBA8_UNORM;
        info.color_space = lux::rdesc::ETextureColorSpace::LINEAR;
        info.layers = 1;
        info.mip_count = 1;
        info.copy = false;
        info.owns_data = false;
        info.mip_ranges[0] = {
            .offset = 0,
            .size = 4,
            .width = 1,
            .height = 1,
        };

        lux::rdesc::Texture t{info, white, 4};
        return t;
    }

}