#pragma once

#include <lux/engine/function/render/graph/TextureAccess.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::render
{
    struct RGTextureSubresourceRange
    {
        std::uint32_t base_mip_level{0};
        std::uint32_t level_count{1};
        std::uint32_t base_array_layer{0};
        std::uint32_t layer_count{1};
    };

    struct RGPassTextureRef
    {
        RGResourceHandle resource;
        lux::render::ETextureRole role;
        ERGResourceUsage usage;
        RGTextureSubresourceRange range;
        std::uint32_t input_attachment_index{std::numeric_limits<std::uint32_t>::max()};
    };

    struct RGPassBufferRef
    {
        RGResourceHandle resource;
        ERGResourceUsage usage;
        ERGBufferRole role;
        std::uint64_t offset{0};
        std::uint64_t size{0};
    };

    // A non-owning, compile-time view of the graph information needed for
    // dependency analysis. Backend payloads, pipelines and record callbacks do
    // not cross this boundary.
    struct RGLogicalPassView
    {
        std::string_view name;
        phase_mask_t phase_mask{0};
        ERenderStage stage{ERenderStage::Default};
        std::span<const RGPassTextureRef> textures;
        std::span<const RGPassBufferRef> buffers;
        std::span<const std::string> after_passes;
        std::span<const std::string> before_passes;
    };

    struct RGLogicalGraphView
    {
        std::uint32_t resource_count{0};
        std::span<const RGLogicalPassView> passes;
    };

    struct RGTransientLifetime
    {
        RGResourceHandle resource;
        std::uint32_t first_pass{0};
        std::uint32_t last_pass{0};
    };

    struct RGDependencyInfo
    {
        std::vector<std::uint32_t> pass_topological_order;
        std::vector<RGTransientLifetime> lifetime;
        bool has_cycle{false};
    };
} // namespace lux::render
