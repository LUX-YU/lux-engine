#pragma once

#include <lux/engine/render/graph/RenderPlan.hpp>
#include <optional>

namespace lux::render::detail
{
    // Physical handle values for one command-buffer recording. Reset at BeginCommandBuffer; it never
    // outlives the physical bindings in that recording and owns no GPU objects or heap storage.
    struct BindingState final
    {
        void invalidate() noexcept { *this = {}; }

        nri::PipelineLayout* layout = nullptr;
        nri::DescriptorPool* descriptor_pool = nullptr;
        nri::Pipeline* pipeline = nullptr;
        nri::BindPoint point = nri::BindPoint::INHERIT;
        std::array<nri::DescriptorSet*, 4> descriptors{};
        std::array<nri::DescriptorSet*, 3> shared_descriptors{};
        std::span<const VertexSource> vertices;
        nri::Buffer* index = nullptr;
        std::uint64_t index_offset = 0;
        nri::IndexType index_type = nri::IndexType::UINT16;
        std::optional<nri::Rect> scissor;
        std::optional<nri::DepthBiasDesc> depth_bias;
    };
} // namespace lux::render::detail
