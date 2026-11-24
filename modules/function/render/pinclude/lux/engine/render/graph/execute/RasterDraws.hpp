#pragma once

#include <NRI.h>
#include <lux/engine/render/graph/execute/BindingState.hpp>
#include <lux/engine/render/graph/DrawList.hpp>
#include <lux/engine/render/effect/EffectFrameWriter.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/render/graph/IndexedGeometry.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>
#include <lux/engine/render/graph/CommandPackets.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <optional>
#include <variant>
#include <vector>

namespace lux::render::runtime_detail { struct JobExecution; struct ResolvedResource; }

namespace lux::render
{
    namespace detail
    {
        struct FrameAlgorithms;
        struct CpuDrawStorage;

        // Frame callbacks can write only their linked outputs. The installed instance IDs qualify native reads.
        struct FrameRasterOutputs final
        {
            const FrameAlgorithms& plan;
            std::span<const ResourceInstanceId> resources;
            std::span<CpuDrawStorage> streams;
            std::uint32_t call;

            [[nodiscard]] RenderResult<std::uint32_t> outputFor(RasterResourceSlot, EDrawResourceRead) const noexcept;
        };
    }

    // Cold view of this algorithm's declared operands. The installed Job retains their owners.
    class InstalledDrawSources final
    {
    public:
        InstalledDrawSources(const InstalledDrawSources&) = delete;
        InstalledDrawSources& operator=(const InstalledDrawSources&) = delete;

        template <class Storage> struct Source final
        {
            const Storage* owner;
            detail::CpuDrawStorage& output;
        };

        template <class Storage> [[nodiscard]] RenderResult<Source<Storage>>
        bind(RasterResourceSlot slot, DrawResourceReads required) const noexcept
        {
            const auto source = resolve(slot, required, cxx::make_basic_type_info<Storage>());
            if (!source) return cxx::unexpected(source.error());
            return Source<Storage>{static_cast<const Storage*>(source->owner), source->output};
        }

    private:
        friend struct runtime_detail::JobExecution;
        friend struct detail::CpuDrawStorage;
        InstalledDrawSources(
            const detail::FrameRasterOutputs& draws,
            std::span<const runtime_detail::ResolvedResource> resources
        ) noexcept : draws_(draws), resources_(resources) {}

        [[nodiscard]] RenderResult<Source<void>>
        resolve(RasterResourceSlot, DrawResourceReads, cxx::basic_type_info) const noexcept;

        const detail::FrameRasterOutputs& draws_;
        std::span<const runtime_detail::ResolvedResource> resources_;
    };
}

namespace lux::render::detail
{
    // Only the development counters consume per-draw physical read ranges.
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
    constexpr bool kCollectReadMetrics = true;
#else
    constexpr bool kCollectReadMetrics = false;
#endif

    // Borrowed until the synchronous input selection finishes. The source owns the linked rows and their ABI.
    struct RasterProgramPublication final
    {
        const CompiledRasterStream::RuntimeContract& contract;
        std::span<const PublishedRasterProgram> programs;
        std::uint64_t revision;
    };

    using ReadRasterPrograms = cxx::move_only_function<RenderResult<RasterProgramPublication>(const void*) noexcept>;

    // Read-only input to one synchronous Freeze. The Resource or CPU algorithm owns each array.
    struct RasterDrawInput final
    {
        struct GeometryBinding final
        {
            ResourceInstanceId owner;
            std::uint64_t item;
            GeometryBufferSlice vertices, indices;
            bool operator==(const GeometryBinding&) const noexcept = default;
        };

        struct GeometryLayout final
        {
            RasterStreamRange attributes;
            std::uint32_t component_alignment;
        };

        struct Geometry final
        {
            GeometryBinding binding;
            GeometryLayout layout;
        };

        struct RecordRead final
        {
            const nri::Buffer* buffer;
            GraphBufferRange range;
            std::uint32_t program, first_image, image_count;
        };

        RasterStreamData data{};
        std::span<const RasterSampledImageRead> textures;
        std::span<const Geometry> geometry;
        std::span<const GeometryAttribute> geometry_attributes;
        std::span<const RecordRead> records;
        std::span<const RasterSampledImageRead> record_images;
        std::uint64_t selection = 0;
        bool written = false;
        // Zero for streams whose producer cannot prove its native draw operands stayed identical.
        std::uint64_t command_revision = 0;
    };

    struct CompiledRasterProgram;
    struct CompiledRasterStream;

    // Job-owned CPU output. Installed algorithms borrow this row; its storage remains valid through Freeze.
    struct CpuDrawStorage final
    {
        std::vector<RasterStreamDraw> draws;
        std::vector<RasterStreamBinding> bindings;
        std::vector<RasterStreamVertex> vertices;
        std::vector<std::byte> constants;
        std::vector<RasterStreamSection> sections;
        std::vector<RasterSampledImageRead> textures;
        std::vector<RasterDrawInput::Geometry> geometry;
        std::vector<GeometryAttribute> geometry_attributes;
        std::vector<RasterDrawInput::RecordRead> records;
        std::vector<RasterSampledImageRead> record_images;
        std::uint64_t selection = 0;
        bool written = false;
        std::uint64_t command_revision = 0;
        std::uint32_t frame_slot = kGraphInvalidIndex;

        [[nodiscard]] RenderResult<RasterProgramSlot> programAt(std::uint32_t index) const noexcept;
        [[nodiscard]] RenderResult<RasterRecordLayout> recordLayout(RasterProgramSlot) const noexcept;
        [[nodiscard]] RenderResult<void>
        appendRecord(std::uint32_t program, const RasterRecordLayout&, const RasterRecordInput&) noexcept;
        [[nodiscard]] RenderResult<void> qualifyGeometrySlices(const IndexedGeometryInput&) const noexcept;
        void beginGeometrySelection() noexcept;
        void markCommandsChanged() noexcept;
        [[nodiscard]] RasterIndexedGeometry indexedGeometry(std::uint32_t row) const noexcept;
        // Native slice bounds and usages are established by the owning arena or by qualifyGeometrySlices.
        [[nodiscard]] RenderResult<RasterIndexedGeometry> appendGeometry(ResourceInstanceId, const IndexedGeometryInput&) noexcept;
        [[nodiscard]] bool validSampledImage(const RasterSampledImageRead&) const noexcept;

        [[nodiscard]] const CompiledRasterStream& compiledStream() const & noexcept;
        struct ViewInput final
        {
            ResourceInstanceId owner;
            std::uint32_t slot;
        };
        [[nodiscard]] RenderResult<ViewInput>
        viewInput(const InstalledDrawSources&, std::uint32_t argument, cxx::basic_type_info type) const noexcept;
        [[nodiscard]] RenderResult<void> bindProgramInput(ViewInput, ReadRasterPrograms) noexcept;
        [[nodiscard]] RenderResult<RasterProgramSelection> programSlots() const noexcept;

        [[nodiscard]] RasterDrawInput view() const & noexcept
        {
            return {
                {draws, bindings, vertices, constants, sections}, textures, geometry, geometry_attributes,
                records, record_images, selection, written, command_revision
            };
        }

        RasterDrawInput view() const && = delete;

    private:
        friend struct runtime_detail::JobExecution;
        runtime_detail::JobExecution* job_ = nullptr;
        const CompiledRasterStream* stream_ = nullptr;
    };

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
            bool mergeable = false;
        };

        using VDraw = std::variant<nri::DrawDesc, nri::DrawIndexedDesc, Indirect>;
        nri::Pipeline* pipeline = nullptr;
        PassDescriptorBindings bindings;
        nri::DescriptorSet* descriptor_set = nullptr;
        std::uint32_t first_vertex = 0, vertex_count = 0;
        std::uint32_t parameter_offset = 0, parameter_size = 0;
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

        [[nodiscard]] std::span<const Section> recordSections() const & noexcept
        {
            if (!sections.empty()) return sections;
            return implicit_section ? std::span(&*implicit_section, 1) : std::span<const Section>{};
        }

        std::optional<Section> implicit_section;
        std::uint64_t command_revision = 0;
        bool has_explicit_sections = false;
        std::vector<Section> sections;
        std::vector<RasterDrawPacket> packets;
        std::vector<nri::AttachmentDesc> colors;
        std::vector<FrozenVertexBinding> vertices;
        std::vector<std::byte> parameters;
        // Actual external reads, including indirect table resources. Graph-owned visible,
        // indirect and count buffers retain their ordinary ResourceVersion edges instead.
        std::vector<Read> reads;
    };

    void mergeIndexedIndirectDraws(RasterDrawList& draws, std::uint32_t limit) noexcept;
} // namespace lux::render::detail
