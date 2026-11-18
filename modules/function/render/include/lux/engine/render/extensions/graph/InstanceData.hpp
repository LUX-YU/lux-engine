#pragma once

#include <exec/task.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <lux/engine/render/extensions/graph/IndexedDrawData.hpp>
#include <lux/engine/render/extensions/graph/ParameterSource.hpp>
#include <memory>

namespace lux::render
{
    class ResourceRegistry;
}

namespace lux::render::detail
{
    class DeviceSession;
    class ResourceUploader;
    class Preparation;

    struct InstanceInputs final
    {
        std::uint32_t slot;
        std::span<const std::byte> parameters;
        std::span<const ResourceInput> resources;
    };

    struct ResourceUpdate final
    {
        GraphResourceId resource;
        ResourceInput input;
    };

    // Only real derived data implements this interface. Instances own it; Features have no runtime object.
    class InstanceData
    {
    public:
        virtual ~InstanceData() noexcept = default;
        [[nodiscard]] virtual RenderResult<bool>
        prepare(const InstanceInputs&, ResourceUploader&, Preparation&) noexcept = 0;

        [[nodiscard]] virtual std::span<const TargetId> sampledTargets() const noexcept
        {
            return {};
        }

        [[nodiscard]] virtual bool requiresPrograms() const noexcept
        {
            return false;
        }

        // Dynamic data advances this revision whenever its required programs change.
        [[nodiscard]] virtual std::uint64_t programRevision() const noexcept
        {
            return 0;
        }

        // False means the captured revision expired or preparation was cancelled.
        [[nodiscard]] virtual exec::task<RenderResult<bool>>
        preparePrograms(execution::CpuScheduler, std::uint64_t, stdexec::inplace_stop_token) noexcept
        {
            co_return true;
        }
    };
} // namespace lux::render::detail

namespace lux::render
{
    // Immutable graph-owned construction and dependency data. Never stores a native handle or mutable state.
    struct InstanceDataRecipe final
    {
        using CreateResult = RenderResult<std::unique_ptr<detail::InstanceData>>;
        using Create = CreateResult (*)(
            detail::DeviceSession&, ResourceRegistry&, InstanceAccess&,
            std::span<const ResourceInstanceId>, const void*
        ) noexcept;
        std::vector<GraphResourceId> outputs;
        ConfigValue configuration;
        Create create = nullptr;
        std::vector<GraphResourceId> inputs;
        std::uint32_t parameter_bytes = 0;
        std::uint32_t parameter_group = 0;
        std::vector<GraphPassId> passes;
        EParameterSource parameter_source = EParameterSource::INSTANCE;
    };
} // namespace lux::render
