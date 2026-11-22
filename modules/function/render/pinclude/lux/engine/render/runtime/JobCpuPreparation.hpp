#pragma once

#include <lux/engine/render/graph/execute/OwnedRasterStream.hpp>
#include <lux/engine/render/extensions/execution/GraphPreparation.hpp>
#include <lux/engine/render/resource/ConstantBufferStorage.hpp>
#include <lux/engine/render/runtime/RasterProgramInputs.hpp>
#include <lux/engine/render/graph/execute/PreparedBufferUpload.hpp>

namespace lux::render::compiled { class RenderGraph; }

namespace lux::render::detail
{
    class JobGpuResources;
    struct FrameResources;

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
        [[nodiscard]] std::span<PreparedBufferInput> bufferInputs() & noexcept { return prepared_buffer_inputs_; }

    private:
        const compiled::RenderGraph& graph_;
        JobGpuResources& gpu_;
        const RasterProgramInputs& program_inputs_;
        class ProducerFrame;
        struct PreparedBytes final
        {
            std::span<std::byte> bytes;
            bool written = false;
        };

        struct PreparedInlineBlock final
        {
            PreparedBytes payload;
            std::uint64_t source_revision = 0;
        };

        struct StorageSnapshot final
        {
            SnapshotSource source;
            StorageSnapshotOps::Selection value;
            std::uint64_t revision = 0;
            const ProgramInputRevision* program_input = nullptr;
        };

        const ResourceRegistry* snapshot_registry_ = nullptr;
        std::vector<std::span<const std::byte>> effect_values_;
        std::vector<BoundDrawResource> draw_resources_;
        std::vector<StorageSnapshot> storage_snapshots_;
        std::vector<std::uint32_t> snapshot_sources_;
        std::vector<extension_detail::AlgorithmWorkspace> workspaces_;
        std::vector<std::byte> selected_bytes_;
        std::vector<PreparedInlineBlock> constants_;
        std::vector<OwnedRasterStream> streams_;
        std::vector<PreparedBytes> cpu_data_;
        std::vector<PreparedBufferInput> prepared_buffer_inputs_;
        std::uint64_t preparation_revision_ = 0;
        std::vector<BoundConstantBuffer> constant_buffers_;
    };
} // namespace lux::render::detail
