#pragma once

#include <lux/engine/render/graph/DrawList.hpp>
#include <lux/engine/render/graph/IndexedGeometry.hpp>
#include <lux/engine/render/effect/CpuCall.hpp>
#include <lux/engine/render/resource/ConstantBufferStorage.hpp>
#include <lux/engine/render/runtime/RasterProgramInputs.hpp>
#include <lux/engine/render/graph/execute/PreparedBufferUpload.hpp>

namespace lux::render::compiled { class RenderGraph; }

namespace lux::render::detail
{
    class JobGpuResources;
    struct FrameResources;
    struct InputSlot;

    // CPU selection storage reused across slots. Freeze copies the accepted draw packets into the selected FIF.
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

        std::vector<RasterStreamDraw> draws;
        std::vector<RasterStreamBinding> bindings;
        std::vector<RasterStreamVertex> vertices;
        std::vector<std::byte> constants;
        std::vector<RasterStreamSection> sections;
        std::vector<RasterSampledImageRead> textures;
        std::vector<Geometry> geometry;
        std::vector<GeometryAttribute> geometry_attributes;
        std::vector<RecordRead> records;
        std::vector<RasterSampledImageRead> record_images;
        std::uint64_t selection = 0;
        bool written = false;
    };
    // CPU inputs and reusable workspaces for the linked call sequence of one installed Job.
    class JobCpuPreparation final
    {
    public:
        JobCpuPreparation(
            const compiled::RenderGraph& graph, JobGpuResources& gpu, const RasterProgramInputs& programs
        ) noexcept
            : graph_(graph), gpu_(gpu), program_inputs_(programs) {}

        struct PreparationSources final
        {
            std::vector<BoundConstantBuffer> constant_buffers;
            std::vector<SnapshotSource> snapshots;
            std::vector<BoundDrawResource> draw_resources;
            std::vector<std::span<const std::byte>> effect_values;
        };

        [[nodiscard]] RenderResult<void> bind(const ResourceRegistry&, PreparationSources, const TargetDesc&) noexcept;
        [[nodiscard]] RenderResult<void> select(FrameResources&, std::uint32_t slot, const TargetDesc&) noexcept;
        [[nodiscard]] std::span<const BoundConstantBuffer> constantBuffers() const & noexcept { return constant_buffers_; }
        std::span<const BoundConstantBuffer> constantBuffers() const && = delete;
        [[nodiscard]] std::span<const RasterDrawInput> drawInputs() const & noexcept { return streams_; }
        std::span<const RasterDrawInput> drawInputs() const && = delete;
        [[nodiscard]] std::span<PreparedBufferInput> bufferInputs() & noexcept { return prepared_buffer_inputs_; }

    private:
        const compiled::RenderGraph& graph_;
        JobGpuResources& gpu_;
        const RasterProgramInputs& program_inputs_;
        friend class lux::render::CpuCallFrame;

        [[nodiscard]] std::span<std::byte> valueBytes(InputSlot) noexcept;

        struct StorageSnapshot final
        {
            SnapshotSource source;
            const ProgramInputRevision* program_input = nullptr;
        };

        const ResourceRegistry* snapshot_registry_ = nullptr;
        std::vector<std::span<const std::byte>> effect_values_;
        std::vector<BoundDrawResource> draw_resources_;
        std::vector<StorageSnapshot> storage_snapshots_;
        std::vector<RevisionedValue<const void*>> selected_snapshots_;
        std::vector<std::uint32_t> snapshot_sources_;
        std::vector<extension_detail::AlgorithmWorkspace> workspaces_;
        std::vector<std::byte> selected_bytes_;
        std::vector<RasterDrawInput> streams_;
        std::vector<PreparedBufferInput> prepared_buffer_inputs_;
        std::uint64_t preparation_revision_ = 0;
        std::vector<BoundConstantBuffer> constant_buffers_;
    };
} // namespace lux::render::detail
