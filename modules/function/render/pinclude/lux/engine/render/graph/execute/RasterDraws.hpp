#pragma once

#include <NRI.h>
#include <lux/engine/render/extensions/graph/PassRecordInput.hpp>
#include <cstddef>
#include <cstdint>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <optional>
#include <variant>
#include <vector>

namespace lux::render::detail
{
    // Physical draw data prepared before recording. Every pointer borrows an object whose accepted
    // use is covered by this frame; the packet owns neither resources nor an execution operation.
    struct RasterDrawPacket final
    {
        struct Indirect final
        {
            nri::Buffer* arguments;
            std::uint64_t offset = 0;
            std::uint32_t count = 0;
            std::uint32_t stride = sizeof(nri::DrawIndexedDesc);
            nri::Buffer* count_buffer = nullptr;
            std::uint64_t count_offset = 0;
        };

        using VDraw = std::variant<nri::DrawDesc, nri::DrawIndexedDesc, Indirect>;
        nri::Pipeline* pipeline = nullptr;
        PassDescriptorBindings bindings;
        std::uint32_t first_descriptor = 0, descriptor_count = 0;
        std::uint32_t first_vertex = 0, vertex_count = 0;
        std::uint32_t parameter_offset = 0, parameter_size = 0, parameter_destination = 0;
        nri::Buffer* index = nullptr;
        std::uint64_t index_offset = 0;
        nri::IndexType index_type = nri::IndexType::UINT32;
        nri::Rect scissor{};
        VDraw draw;
    };

    // One list per dynamic Raster pass and physical FIF slot. Arrays reuse capacity; recording
    // consumes their immutable borrows and never allocates, compiles or mutates another View's list.
    struct RasterDrawList final
    {
        struct Read final
        {
            const void* physical = nullptr;
            VGraphRange range;
            EGraphUse usage = EGraphUse::UNSPECIFIED;
            std::uint32_t stages = 0;
            bool operator==(const Read&) const noexcept = default;
        };

        struct Section final
        {
            std::uint32_t first_color = 0, color_count = 0;
            nri::AttachmentDesc depth{};
            nri::Viewport viewport{};
            std::optional<nri::DepthBiasDesc> depth_bias;
            std::uint32_t first_draw = 0, draw_count = 0;
            // Common constants are applied on section entry and again after a layout change.
            std::uint32_t parameter_offset = 0, parameter_size = 0;
        };

        struct Vertex final
        {
            std::uint32_t binding;
            nri::VertexBufferDesc buffer;
        };

        std::vector<Section> sections;
        std::vector<RasterDrawPacket> packets;
        std::vector<nri::AttachmentDesc> colors;
        std::vector<nri::DescriptorSet*> descriptors;
        std::vector<Vertex> vertices;
        std::vector<std::byte> parameters;
        // Actual external reads, including indirect table resources. Graph-owned visible,
        // indirect and count buffers retain their ordinary ResourceVersion edges instead.
        std::vector<Read> reads;
        std::vector<Read> dependency_reads;
    };
} // namespace lux::render::detail
