#pragma once

#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render
{
    struct RasterWork;
    struct ComputeWork;
    struct GraphTransferCommand;
}

namespace lux::render::detail
{
    // Attachment formats and samples used during native pipeline preparation.
    struct PassRenderingInfo final
    {
        nri::Format depth = nri::Format::UNKNOWN;
        std::uint8_t samples = 1;
        bool dynamic_depth_bias = false;
    };

    struct PassNativeAccess final
    {
        nri::AccessBits access;
        nri::StageBits stages;
        nri::Layout layout = nri::Layout::UNDEFINED;
    };

    [[nodiscard]] RenderResult<RasterCommands>
    compileRasterCommands(const RasterWork&, std::span<const CompiledResource>, const nri::DeviceDesc&, RecordingProgram&) noexcept;

    [[nodiscard]] RenderResult<ComputeCommands>
    compileComputeCommands(const ComputeWork&, std::span<const CompiledResource>, const nri::DeviceDesc&) noexcept;

    [[nodiscard]] RenderResult<TransferCommands>
    compileTransferCommands(
        const GraphTransferCommand&,
        std::span<const CompiledResource>,
        nri::GraphicsAPI,
        TransferProgram&
    ) noexcept;

    [[nodiscard]] std::optional<PassNativeAccess>
    transferAccess(
        const GraphTransferCommand&,
        const ResourceAccess&,
        const VGraphResourceDesc&,
        nri::GraphicsAPI
    ) noexcept;
} // namespace lux::render::detail
