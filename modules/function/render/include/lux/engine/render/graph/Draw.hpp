#pragma once

#include <cstdint>
#include <lux/engine/description/Image.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>
#include <variant>
#include <vector>

namespace lux::render
{
    enum class EVertexRate : std::uint8_t
    {
        VERTEX,
        INSTANCE
    };
    enum class EIndexType : std::uint8_t
    {
        UINT16,
        UINT32
    };

    enum class EDepthCompare : std::uint8_t
    {
        NEVER,
        LESS,
        EQUAL,
        LESS_EQUAL,
        GREATER,
        NOT_EQUAL,
        GREATER_EQUAL,
        ALWAYS
    };

    struct VertexBinding final
    {
        std::uint32_t binding = 0, stride = 0;
        EVertexRate rate = EVertexRate::VERTEX;
        bool operator==(const VertexBinding&) const noexcept = default;
    };

    struct VertexAttribute final
    {
        std::uint32_t location = 0, binding = 0, offset = 0;
        rdesc::ETextureFormat format = rdesc::ETextureFormat::UNDEFINED;
        bool operator==(const VertexAttribute&) const noexcept = default;
    };

    enum class ECullMode : std::uint8_t
    {
        NONE,
        FRONT,
        BACK
    };

    // Pipeline state excludes resource names, Buffer offsets and draw counts.
    enum class EBlendFactor : std::uint8_t
    {
        ZERO,
        ONE,
        SOURCE_ALPHA,
        ONE_MINUS_SOURCE_ALPHA
    };

    struct ColorBlendState final
    {
        bool enabled = false;
        EBlendFactor source_color = EBlendFactor::ONE, destination_color = EBlendFactor::ZERO;
        EBlendFactor source_alpha = EBlendFactor::ONE, destination_alpha = EBlendFactor::ZERO;
        bool operator==(const ColorBlendState&) const noexcept = default;
    };

    struct GraphicsState final
    {
        std::vector<VertexBinding> vertex_bindings;
        std::vector<VertexAttribute> vertex_attributes;
        bool depth_test = false, depth_write = false;
        EDepthCompare depth_compare = EDepthCompare::LESS;
        ECullMode cull_mode = ECullMode::NONE;
        bool front_counter_clockwise = true;
        float depth_bias_constant = 0, depth_bias_slope = 0;
        ColorBlendState blend;
        bool operator==(const GraphicsState&) const noexcept = default;
    };

    struct ProgramVertexBuffer final
    {
        ResourceVersion resource;
        std::uint32_t binding = 0;
        std::uint64_t offset = 0;
    };

    struct DrawVertices final
    {
        std::uint32_t count = 3, instances = 1, first = 0, first_instance = 0;
    };

    struct DrawIndexed final
    {
        ResourceVersion index_resource;
        std::uint64_t offset = 0;
        EIndexType type = EIndexType::UINT32;
        std::uint32_t count = 0, instances = 1, first = 0, first_instance = 0;
        std::int32_t vertex_offset = 0;
    };

    struct DrawIndexedIndirect final
    {
        ResourceVersion index_resource;
        std::uint64_t index_offset = 0;
        EIndexType type = EIndexType::UINT32;
        ResourceVersion commands_resource;
        std::uint64_t commands_offset = 0;
        std::uint32_t count = 1, stride = 20;
        std::optional<ResourceVersion> count_resource;
        std::uint64_t count_offset = 0;
    };

    // Cold recipe. Geometry and draw ranges are supplied to this pass in a complete frame.
    // One interleaved vertex stream; selection_parameter names a scalar uint push constant.
    struct DrawIndexedList final
    {
        ResourceVersion index_resource;
        std::uint64_t index_offset = 0;
        EIndexType type = EIndexType::UINT32;
        std::string selection_parameter;
        std::uint32_t selection_count = 0;
    };

    struct IndexedDrawRange final
    {
        std::uint32_t first_index = 0, index_count = 0;
        std::int32_t vertex_offset = 0;
        std::uint32_t x = 0, y = 0, width = 0, height = 0;
        std::uint32_t selection = 0;
    };

    // Borrowed through the upload terminal. Indices name vertices in the supplied interleaved stream;
    // their contents are authored geometry, just as for an ordinary DrawIndexed resource.
    struct DrawListData final
    {
        std::span<const std::byte> vertices, indices, parameters;
        std::span<const IndexedDrawRange> draws;
    };

    using VDrawCommand = std::variant<DrawVertices, DrawIndexed, DrawIndexedIndirect, DrawIndexedList>;
} // namespace lux::render
