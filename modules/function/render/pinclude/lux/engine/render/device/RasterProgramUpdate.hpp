#pragma once

#include <lux/engine/render/resource/RasterProgramBuild.hpp>
#include <lux/engine/render/device/ProgramCompileInput.hpp>
#include <lux/engine/render/graph/Ids.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>
#include <lux/engine/render/runtime/JobId.hpp>
#include <chrono>

namespace lux::render::detail
{
    // The build call borrows only its granted inputs. The returned native recipe owns its bytes.
    struct RasterProgramBuildData final
    {
        struct Stream final
        {
            std::uint32_t pass;
            std::span<const ResourceInstanceId> sources{};
            std::optional<std::vector<RuntimeRasterProgram>> programs{};
            std::vector<rdesc::ShaderProgramArtifact> shaders;
            bool ready = false;
            bool reuse = false;
        };

        struct Snapshot final
        {
            ResourceInstanceId owner;
            RevisionedValue<const void*> selection;
        };

        const ResourceRegistry& registry;
        const DeviceSession& device;
        std::vector<Stream> streams;
        std::vector<Snapshot> snapshots;
        std::span<const std::uint32_t> snapshot_reads;

        [[nodiscard]] Stream* find(DrawStreamSlot) noexcept;
        [[nodiscard]] bool canRead(const Stream&, ResourceInstanceId) const noexcept;
    };

    // The device owns the selected inputs and every unpublished native use through driver return.
    struct RasterProgramUpdate final
    {
        RasterProgramUpdate(DeviceSession& device, ExecutionBindingId binding) noexcept
            : device(device), binding(binding) {}
        ~RasterProgramUpdate() noexcept;
        RasterProgramUpdate(const RasterProgramUpdate&) = delete;
        RasterProgramUpdate& operator=(const RasterProgramUpdate&) = delete;

        struct PendingProgram final
        {
            RasterProgramLink link;
            ResourceProgramSource source;
        };

        struct Stream final
        {
            std::uint32_t pass;
            std::vector<PendingProgram> programs;
            std::unique_ptr<DynamicRasterPrograms> candidate;
        };

        [[nodiscard]] exec::task<RenderResult<void>> compile(execution::CpuScheduler) noexcept;

        DeviceSession& device;
        const ExecutionBindingId binding;
        std::uint64_t input_revision = 0;
        std::uint32_t build = kGraphInvalidIndex;
        RasterProgramBuilder::Mapping mapping{nullptr, nullptr};
        std::vector<Stream> streams;
        stdexec::inplace_stop_source stop;
        std::optional<RenderError> returned;
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
#endif
    };
} // namespace lux::render::detail
