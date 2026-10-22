#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>

namespace lux::render::detail
{
    // Borrowed state for one command-buffer recording. Reset at BeginCommandBuffer; it never
    // outlives the physical bindings in that recording and owns no GPU objects or heap storage.
    struct BindingState final
    {
        nri::PipelineLayout* layout = nullptr;
        nri::Pipeline* pipeline = nullptr;
        nri::BindPoint point = nri::BindPoint::INHERIT;
        std::span<nri::DescriptorSet* const> descriptors;
        std::span<const VertexSource> vertices;
        nri::Buffer* index = nullptr;
        std::uint64_t index_offset = 0;
        nri::IndexType index_type = nri::IndexType::UINT16;
        bool prepared_depth_bias = false;
    };
} // namespace lux::render::detail
