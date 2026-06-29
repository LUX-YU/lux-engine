#pragma once

#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/renderer/RenderTargetLayout.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <functional>
#include <vulkan/vulkan.h>

#include <lux/cxx/algorithm/hash.hpp>

namespace lux::render
{
    // ========== External Resource Getters ==========

    /// Output-style getter: write up to `capacity` handles into `out_*`.
    /// Return value is the number of valid entries written.
    using ExternalImageGetter  = std::function<uint32_t(VkImage* out_images, uint32_t capacity)>;
    using ExternalBufferGetter = std::function<uint32_t(VkBuffer* out_buffers, uint32_t capacity)>;

    // ========== Imported Resource Info ==========

    struct RGImportedResourceInfo
    {
        ExternalImageGetter image_getter = nullptr;
        uint32_t update_group = static_cast<uint32_t>(RGUpdateGroup::GROUP_NONE);

        VkImageLayout       initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageLayout       final_layout   = VK_IMAGE_LAYOUT_UNDEFINED;

        VkAccessFlags2       initial_access = 0;
        VkAccessFlags2       final_access   = 0;

        VkPipelineStageFlags2 initial_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags2 final_stage   = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;

        /// Slot assignment — when set, the compiler registers this resource in
        /// RGCompiledGraph::slot_resource_idx so the recorder can inject per-frame
        /// images from RGFrameContext::imported_slots without name-based lookups.
        std::optional<TargetSlot> slot;

        /// When true, the first render-pass group that writes this resource uses
        /// LOAD_OP_LOAD instead of LOAD_OP_CLEAR — preserving content written by
        /// a prior render pass outside of this graph (e.g. scene → overlay layering).
        bool preserve_content = false;
    };

    struct RGImportedBufferInfo
    {
        ExternalBufferGetter buffer_getter = nullptr;
        uint32_t update_group = static_cast<uint32_t>(RGUpdateGroup::GROUP_NONE);

        VkAccessFlags2        initial_access = 0;
        VkAccessFlags2        final_access   = 0;

        VkPipelineStageFlags2 initial_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        VkPipelineStageFlags2 final_stage   = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    };

    // ========== Texture Description ==========

    struct RGTextureDescription
    {
        ERGSizeMode             sizing_mode  = ERGSizeMode::ABSOLUTE_MODE;
        float                   width_scale  = 1.0f;
        float                   height_scale = 1.0f;

        uint32_t                width        = 1;
        uint32_t                height       = 1;
        uint32_t                depth        = 1;
        uint32_t                mip_levels   = 1;
        uint32_t                array_layers = 1;

        lux::common::ETextureDimension     dimension{ lux::common::ETextureDimension::TEX_2D };
        lux::common::ETextureFormat        format{ lux::common::ETextureFormat::RGBA8_UNORM };

        uint32_t                sample{ 1 };
        ERGTextureUsageFlags    usage{ 0 };

        bool                    allow_aliasing = true;

        static RGTextureDescription Absolute(uint32_t w, uint32_t h, lux::common::ETextureFormat fmt = lux::common::ETextureFormat::RGBA8_UNORM) {
            RGTextureDescription d;
            d.width = w; d.height = h; d.format = fmt;
            d.sizing_mode = ERGSizeMode::ABSOLUTE_MODE;
            return d;
        }

        static RGTextureDescription Relative(float scale_w, float scale_h, lux::common::ETextureFormat fmt = lux::common::ETextureFormat::RGBA8_UNORM) {
            RGTextureDescription d;
            d.width_scale = scale_w; d.height_scale = scale_h; d.format = fmt;
            d.sizing_mode = ERGSizeMode::RELATIVE_MODE;
            return d;
        }
    };

    // ========== Buffer Description ==========

    struct RGBufferDescription
    {
        uint64_t                size;
        uint32_t                stride;
        uint32_t                element_count;

        ERGBufferUsageFlags     usage;
        ERGMemoryUsage          memory_usage;
        bool                    allow_aliasing = true;
    };

    // ========== Subresource Range ==========

    struct RGTextureSubresourceRange
    {
        uint32_t base_mip_level   = 0;
        uint32_t level_count      = 1;
        uint32_t base_array_layer = 0;
        uint32_t layer_count      = 1;
    };

    // ========== Aggregate Resource Description ==========

    using VRGResourceDescription = std::variant<RGTextureDescription, RGBufferDescription>;

    struct RGResourceDescription
    {
        std::string             name;
        uint64_t                name_hash{ 0 };
        ERGResourceType         type{ ERGResourceType::TEXTURE };
        ERGResourceLifetime     lifetime{ ERGResourceLifetime::PERSISTENT };

        VRGResourceDescription  desc;

        std::unique_ptr<RGImportedResourceInfo> import_info;
        std::unique_ptr<RGImportedBufferInfo>   import_buffer_info;

        // PING_PONG ring info (M1). The CURRENT resource (ring_phase 0) owns the
        // physical copies; a PREVIOUS/history resource (ring_phase>0) allocates
        // nothing and resolves to pingpong_peer's copy `ring_phase` frames earlier.
        uint32_t ring_size{1};
        uint32_t ring_phase{0};
        int32_t  pingpong_peer{-1};

        // For FORWARD_REFERENCE resources: required vs optional dependency. A
        // required reference left unresolved at compile time (no producer with this
        // name) fails compilation fast instead of degrading to a null view. (M2)
        ERGReference reference_mode{ERGReference::Optional};

        RGResourceDescription() = default;
        RGResourceDescription(RGResourceDescription&&) = default;
        RGResourceDescription& operator=(RGResourceDescription&&) = default;

        RGResourceDescription(const RGResourceDescription& other)
            : name(other.name)
            , name_hash(other.name_hash)
            , type(other.type)
            , lifetime(other.lifetime)
            , desc(other.desc)
            , import_info(other.import_info ? std::make_unique<RGImportedResourceInfo>(*other.import_info) : nullptr)
            , import_buffer_info(other.import_buffer_info ? std::make_unique<RGImportedBufferInfo>(*other.import_buffer_info) : nullptr)
            , ring_size(other.ring_size)
            , ring_phase(other.ring_phase)
            , pingpong_peer(other.pingpong_peer)
            , reference_mode(other.reference_mode)
        {}

        RGResourceDescription& operator=(const RGResourceDescription& other)
        {
            if (this != &other) {
                name = other.name;
                name_hash = other.name_hash;
                type = other.type;
                lifetime = other.lifetime;
                desc = other.desc;
                import_info = other.import_info ? std::make_unique<RGImportedResourceInfo>(*other.import_info) : nullptr;
                import_buffer_info = other.import_buffer_info ? std::make_unique<RGImportedBufferInfo>(*other.import_buffer_info) : nullptr;
                ring_size = other.ring_size;
                ring_phase = other.ring_phase;
                pingpong_peer = other.pingpong_peer;
                reference_mode = other.reference_mode;
            }
            return *this;
        }
    };

} // namespace lux::render
