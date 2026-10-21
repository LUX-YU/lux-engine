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
    struct GraphicsState final
    {
        std::vector<VertexBinding> vertex_bindings;
        std::vector<VertexAttribute> vertex_attributes;
        bool depth_test = false, depth_write = false;
        EDepthCompare depth_compare = EDepthCompare::LESS;
        ECullMode cull_mode = ECullMode::NONE;
        bool front_counter_clockwise = true;
        float depth_bias_constant = 0, depth_bias_slope = 0;
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

    using VDrawCommand = std::variant<DrawVertices, DrawIndexed, DrawIndexedIndirect>;
} // namespace lux::render
