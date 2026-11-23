#pragma once

#include <lux/engine/render/resource/RasterProgramBuild.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/ProgramInputRevision.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>
#include <lux/engine/render/runtime/JobId.hpp>

namespace lux::render
{
    struct ProgramInputRevision::Use final
    {
        detail::RasterProgramInputs* owner;
        ProgramInputRevision* source;
        std::vector<std::uint32_t> builds;
        Use* previous = nullptr;
        Use* next = nullptr;
    };
}

namespace lux::render::compiled { class RenderGraph; }

namespace lux::render::detail
{
    struct SnapshotSource final
    {
        ResourceInstanceId id;
        void* storage;
        const StorageSnapshotOps* ops;
    };

    struct BoundDrawResource final
    {
        ResourceInstanceId id;
        const void* storage = nullptr;
        cxx::basic_type_info implementation;
        std::uint32_t capabilities = 0;
        bool indexed_geometry = false;
    };

    struct FrameResources;
    class JobGpuResources;
    class DeviceEntry;
    struct RasterProgramUpdate;

    // The installed binding owns its mappings and subscriptions. Only selected recipes cross native compilation.
    class RasterProgramInputs final
    {
    public:
        RasterProgramInputs(const compiled::RenderGraph& graph, JobGpuResources& gpu) noexcept
            : graph_(graph), gpu_(gpu) {}
        ~RasterProgramInputs() noexcept;
        RasterProgramInputs(const RasterProgramInputs&) = delete;
        RasterProgramInputs& operator=(const RasterProgramInputs&) = delete;

        [[nodiscard]] RenderResult<void> bind(
            const ResourceRegistry&,
            std::span<const SnapshotSource>, std::span<const BoundDrawResource>
        ) noexcept;
        void install(DeviceEntry&, ExecutionBindingId) noexcept;

        [[nodiscard]] bool pending() const noexcept { return unready_ != 0; }
        [[nodiscard]] std::uint32_t nextBuild() const noexcept;
        [[nodiscard]] bool inputsBusy(std::uint32_t slot) const noexcept;
        [[nodiscard]] RenderError reportableError() const noexcept;
        [[nodiscard]] const ProgramInputRevision* revisionSource(ResourceInstanceId) const noexcept;
        [[nodiscard]] const void* mapping(std::uint32_t slot) const noexcept;
        [[nodiscard]] bool current(const RasterProgramUpdate&) const noexcept;
        [[nodiscard]] RenderResult<void> select(RasterProgramUpdate&) const noexcept;
        void settle(RasterProgramUpdate&, std::span<FrameResources>) noexcept;

    private:
        friend class lux::render::ProgramInputRevision;
        void invalidate(std::span<const std::uint32_t> builds) noexcept;

        struct Source final
        {
            SnapshotSource binding;
            ProgramInputRevision* revision;
        };

        struct Build final
        {
            enum class EState { DIRTY, READY, FAILED };
            struct Stream final
            {
                std::uint32_t pass, list;
                std::vector<ResourceInstanceId> grants;
            };
            RasterProgramBuilder::Build compile;
            const void* inputs;
            std::vector<Stream> streams;
            std::vector<std::uint32_t> sources;
            std::vector<std::uint32_t> reads;
            RasterProgramBuilder::Mapping mapping{nullptr, nullptr};
            std::uint64_t revision = 1;
            EState state = EState::DIRTY;
            RenderError error{};
        };

        const compiled::RenderGraph& graph_;
        JobGpuResources& gpu_;
        const ResourceRegistry* registry_ = nullptr;
        std::vector<Build> builds_;
        std::vector<Source> sources_;
        std::vector<ProgramInputRevision::Use> subscriptions_;
        std::uint32_t unready_ = 0, failed_ = 0;
        DeviceEntry* device_ = nullptr;
        ExecutionBindingId binding_{};
    };
} // namespace lux::render::detail
