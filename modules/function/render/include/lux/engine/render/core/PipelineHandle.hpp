#pragma once
/**
 * @file PipelineHandle.hpp
 * @brief Public opaque pipeline handles (graphics + compute).
 *
 * Moved here from the internal sinclude GraphicsPipelineTemplate.hpp (SDK P1) so
 * RGPassBuilder::setPipeline/setComputePipeline and RGPassDescription can be named
 * from the install tree. Depends only on the already-public core/ResourceHandle.hpp
 * (TypedHandle) — no Vulkan, no SmallVector, no heavy pipeline-template struct.
 */

#include <cstdint>
#include <lux/engine/render/core/ResourceHandle.hpp>   // TypedHandle

namespace lux::render
{
    struct GraphicsPipelineTag {};
    using GraphicsPipelineHandle = TypedHandle<GraphicsPipelineTag>;

    /// @brief Sentinel value indicating no valid pipeline handle.
    inline constexpr GraphicsPipelineHandle kInvalidPipelineHandle{};

    // -- Compute pipeline handle --
    struct ComputePipelineHandle
    {
        uint32_t index{ ~0u };
        [[nodiscard]] bool valid() const noexcept { return index != ~0u; }
        bool operator==(const ComputePipelineHandle& o) const noexcept { return index == o.index; }
        bool operator!=(const ComputePipelineHandle& o) const noexcept { return index != o.index; }
    };

    inline constexpr ComputePipelineHandle kInvalidComputePipelineHandle{};

} // namespace lux::render
