#pragma once

#include <lux/engine/render/graph/RenderGraphExecutable.hpp>

namespace lux::render::detail
{
    // Attachment formats and samples used during native pipeline preparation.
    struct PassRenderingInfo final
    {
        nri::Format depth = nri::Format::UNKNOWN;
        std::uint8_t samples = 1;
        bool dynamic_depth_bias = false;
    };

    struct PassCompileContext final
    {
        const nri::DeviceDesc& limits;
        const rdesc::ShaderInfo* shader;
        std::span<const GraphResource> resources;
        std::uint32_t* raster_list_count = nullptr;
        std::span<const std::uint8_t> materialized_resources;
    };

    struct PassNativeAccess final
    {
        nri::AccessBits access;
        nri::StageBits stages;
        nri::Layout layout = nri::Layout::UNDEFINED;
    };

    struct RasterPass final
    {
        static RenderResult<RasterCommands> compile(const RasterWork&, PassCompileContext&) noexcept;
    };

    struct ComputePass final
    {
        static RenderResult<ComputeCommands> compile(const ComputeWork&, PassCompileContext&) noexcept;
    };

    struct TransferPass final
    {
        static RenderResult<TransferCommands>
        compile(const TransferWork&, std::span<const ResourceAccess> storage_fills, PassCompileContext&) noexcept;
        static std::optional<PassNativeAccess>
        access(
            const TransferWork&,
            std::span<const ResourceAccess> storage_fills,
            const ResourceAccess&,
            const VGraphResourceDesc&,
            nri::GraphicsAPI
        ) noexcept;
    };
} // namespace lux::render::detail
