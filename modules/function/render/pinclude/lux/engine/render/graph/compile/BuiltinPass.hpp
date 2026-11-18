#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
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
        static RenderResult<TransferCommands> compile(const TransferWork&, PassCompileContext&) noexcept;
        static std::optional<PassNativeAccess>
        access(const TransferWork&, const ResourceAccess&, const VGraphResourceDesc&, nri::GraphicsAPI) noexcept;
    };
} // namespace lux::render::detail
