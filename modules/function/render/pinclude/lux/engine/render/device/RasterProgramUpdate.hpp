#pragma once

#include <lux/engine/render/resource/RasterProgramBuild.hpp>
#include <lux/engine/render/device/ProgramCompileInput.hpp>
#include <lux/engine/render/runtime/ExecutionBindingId.hpp>
#include <chrono>

namespace lux::render::detail
{
    struct BoundDrawResource;

    // The build call borrows only its granted inputs. The returned native recipe owns its bytes.
    struct RasterProgramBuildData final
    {
        struct Stream final
        {
            std::uint32_t pass;
            std::span<const BoundDrawResource> sources{};
            std::optional<std::vector<RuntimeRasterProgram>> programs{};
            std::vector<rdesc::ShaderProgramArtifact> shaders;
            bool ready = false;
            bool reuse = false;
        };

        struct Snapshot final
        {
            std::uint32_t slot;
            ResourceInstanceId owner;
            const cxx::basic_type_info* type;
            extension_detail::SelectedStorageSnapshot selection;
        };

        const ResourceRegistry& registry;
        const DeviceSession& device;
        std::vector<Stream> streams;
        std::vector<Snapshot> snapshots;

        [[nodiscard]] Stream* find(DrawStreamSlot) noexcept;
        [[nodiscard]] bool canRead(const Stream&, ResourceInstanceId) const noexcept;
    };

    // The device owns the selected inputs and every unpublished native use through driver return.
    struct RasterProgramUpdate final
    {
        RasterProgramUpdate(
            DeviceSession& device, const ResourceRegistry& registry, ExecutionBindingId binding
        ) noexcept
            : device(device), registry(registry), binding(binding) {}
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

        struct Selection final
        {
            std::uint32_t producer;
            RasterProgramBuilder::Mapping draw_mapping;
        };

        [[nodiscard]] exec::task<RenderResult<void>> compile(execution::CpuScheduler) noexcept;
        [[nodiscard]] bool publishReturnedUses() noexcept;

        DeviceSession& device;
        const ResourceRegistry& registry;
        const ExecutionBindingId binding;
        std::uint64_t input_revision = 0;
        struct Publication final
        {
            ResourceProgramSource source;
            NativeProgramId program;
        };

        std::vector<Publication> pending_publications;
        std::vector<Selection> selections;
        std::vector<Stream> streams;
        stdexec::inplace_stop_source stop;
        std::optional<RenderError> returned;
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
#endif
    };
} // namespace lux::render::detail
