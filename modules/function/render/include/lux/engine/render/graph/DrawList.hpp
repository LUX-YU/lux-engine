#pragma once

#include <lux/engine/render/graph/RasterViewport.hpp>
#include <lux/engine/render/graph/Draw.hpp>
#include <lux/engine/render/runtime/TargetId.hpp>
#include <optional>
#include <span>

namespace nri { struct Texture; struct Buffer; }

namespace lux::rdesc { struct ShaderBlock; }

namespace lux::render
{
    struct RasterSampledImageRead final
    {
        const nri::Texture* image;
        GraphImageRange range;
    };

    struct RasterRecordLayout final
    {
        const rdesc::ShaderBlock& block;
        std::uint32_t frame_slot;
        bool reads_images;
    };

    struct RasterRecordInput final
    {
        const nri::Buffer* buffer;
        GraphBufferRange range;
        std::span<const RasterSampledImageRead> images;
    };

    // Final stream and operand slots for one compiled graph.
    struct DrawStreamSlot final { std::uint32_t index = kGraphInvalidIndex; };
    struct RasterProgramSlot final
    {
        std::uint32_t stream = kGraphInvalidIndex, index = kGraphInvalidIndex;
        std::uint64_t revision = 0;
    };
    struct RasterBufferSlot final
    {
        std::uint32_t index = kGraphInvalidIndex;
        // Graph operands index a resource access group. Selected geometry also carries a frame revision.
        std::uint32_t stream = kGraphInvalidIndex;
        std::uint64_t selection = 0;
    };
    struct RasterImageSlot final { std::uint32_t index = kGraphInvalidIndex, stream = kGraphInvalidIndex; };
    struct RasterSamplerSlot final
    {
        std::uint32_t stream = kGraphInvalidIndex, index = kGraphInvalidIndex;
    };

    struct RasterBufferSlice final
    {
        // The complete current binding of a graph buffer declared as a whole-resource access.
        static constexpr std::uint64_t whole = UINT64_MAX;
        RasterBufferSlot buffer;
        std::uint64_t offset = 0, size = 0;
    };

    struct RasterIndexedGeometry final
    {
        RasterBufferSlice vertices, indices;
        EIndexType index_type;
        // Bases for indices and vertices when indirect commands use the shared backing buffer.
        std::uint32_t arena_first_index;
        std::int32_t arena_vertex_offset;
    };

    struct RasterImageSlice final
    {
        RasterImageSlot image;
        GraphImageRange range;
    };

    // A sampled, single-mip offscreen publication selected before Job ordering.
    struct RasterTargetImage final
    {
        TargetId target;
        EProductRead read;
        bool operator==(const RasterTargetImage&) const noexcept = default;
    };

    using VRasterBindingSource = std::variant<RasterBufferSlice, RasterImageSlice, RasterSamplerSlot, RasterTargetImage>;

    struct RasterStreamBinding final
    {
        std::uint32_t argument = 0;
        VRasterBindingSource source;
        std::uint32_t element = 0;
    };

    struct RasterStreamVertex final
    {
        std::uint32_t binding = 0;
        RasterBufferSlice source;
    };

    struct RasterStreamIndexed final
    {
        RasterBufferSlice indices;
        EIndexType type = EIndexType::UINT32;
        std::uint32_t count = 0, instances = 1, first = 0, first_instance = 0;
        std::int32_t vertex_offset = 0;
    };

    enum class ERasterIndexOrigin : std::uint8_t
    {
        SLICE,
        BUFFER_BASE
    };

    struct RasterStreamIndexBinding final
    {
        RasterBufferSlice source;
        EIndexType type = EIndexType::UINT32;
        ERasterIndexOrigin origin = ERasterIndexOrigin::SLICE;
    };

    // The argument slice contains tightly packed draw records when stride is zero.
    // The optional count slice starts with a GPU-written uint32_t clamped to maximum_count.
    struct RasterStreamIndirect final
    {
        RasterBufferSlice arguments;
        std::optional<RasterBufferSlice> count_buffer;
        std::optional<RasterStreamIndexBinding> index;
        std::uint32_t maximum_count = 1, stride = 0;
    };

    using VRasterStreamDraw = std::variant<DrawVertices, RasterStreamIndexed, RasterStreamIndirect>;

    struct RasterStreamRange final
    {
        std::uint32_t first = 0, count = 0;

        [[nodiscard]] constexpr bool fits(std::size_t size) const noexcept
        {
            return first <= size && count <= size - first;
        }
    };

    // Half-open edges in framebuffer pixels. Freeze intersects them with the selected section viewport.
    struct RasterClipRect final
    {
        std::int32_t left, top, right, bottom;
    };

    struct RasterStreamDraw final
    {
        RasterProgramSlot program;
        RasterStreamRange bindings, vertices, constants;
        VRasterStreamDraw draw;
        std::optional<RasterClipRect> clip{};
    };

    struct RasterDepthBias final
    {
        float constant = 0, slope = 0;
    };

    // Layer is relative to each declared attachment and resolve range. Draw intervals must cover the stream in order.
    // Each layer must occur once. A zero-length interval still applies the graph-authored attachment operations.
    // For runtime-layer attachments the section count selects the physical layer count for the writable frame.
    struct RasterStreamSection final
    {
        RasterStreamRange draws;
        std::uint32_t layer = 0;
        std::optional<RasterViewport> viewport;
        std::optional<RasterDepthBias> depth_bias;
    };

    // A Resource lends these arrays during Freeze. A producer submitting temporary arrays lends them during its call.
    struct RasterStreamData final
    {
        std::span<const RasterStreamDraw> draws;
        std::span<const RasterStreamBinding> bindings;
        std::span<const RasterStreamVertex> vertices;
        std::span<const std::byte> constants;
        // An empty section table uses the pass's original scope. Runtime-layer attachments require sections.
        std::span<const RasterStreamSection> sections = {};

        [[nodiscard]] bool fitsSlotRanges() const noexcept
        {
            return draws.size() < kGraphInvalidIndex && bindings.size() < kGraphInvalidIndex &&
                vertices.size() < kGraphInvalidIndex && constants.size() < kGraphInvalidIndex &&
                sections.size() < kGraphInvalidIndex;
        }
    };
} // namespace lux::render
