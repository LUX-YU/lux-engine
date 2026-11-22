#pragma once

#include <NRI.h>
#include <cstdint>
#include <span>
#include <optional>

namespace lux::render::detail
{
    struct VertexSource;

    // Geometry and raster state for one physical command buffer. Dynamic shader bindings live in their draw encoder.
    struct BindingState final
    {
        void invalidate() noexcept { *this = {}; }

        std::span<const VertexSource> vertices;
        nri::Buffer* index = nullptr;
        std::uint64_t index_offset = 0;
        nri::IndexType index_type = nri::IndexType::UINT16;
        std::optional<nri::Rect> scissor;
        std::optional<nri::DepthBiasDesc> depth_bias;
    };
} // namespace lux::render::detail
