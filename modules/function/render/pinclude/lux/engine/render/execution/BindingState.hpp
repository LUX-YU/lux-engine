#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    // Borrowed state for one command-buffer recording. Descriptor sets remain conservatively rebound.
    struct BindingState final
    {
        nri::PipelineLayout* layout = nullptr;
        nri::Pipeline* pipeline = nullptr;
        nri::BindPoint point = nri::BindPoint::INHERIT;
        std::span<const VertexSource> vertices;
        nri::Buffer* index = nullptr;
        std::uint64_t index_offset = 0;
        nri::IndexType index_type = nri::IndexType::UINT16;
        bool prepared_depth_bias = false;
    };
} // namespace lux::render::detail
