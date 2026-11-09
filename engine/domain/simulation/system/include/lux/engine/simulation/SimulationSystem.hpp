#pragma once

#include <exec/task.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ecs/EcsCommandBuffer.hpp>

#include <lux/engine/simulation/SimulationTime.hpp>
#include <lux/engine/simulation/SystemAccessSpec.hpp>
#include <lux/engine/simulation/system/visibility.h>
#include <lux/engine/task/Task.hpp>

namespace lux::simulation
{
    // Simulation owns the object. LuxObject parentage never determines deletion.
    class LUX_ENGINE_SIMULATION_SYSTEM_PUBLIC SimulationSystem : public object::LuxObject
    {
    public:
        using Result = cxx::expected<void, error::Error>;

        // Read during graph construction. The returned storage must survive this read.
        [[nodiscard]] virtual SystemAccessSpec access() const noexcept = 0;

        // Read once at adoption. Zero capacity means this System does not record structural commands.
        [[nodiscard]] virtual ecs::EcsCommandProducerCapacity commandCapacity() const noexcept;

        // Worker execution requires declared component access and thread-safe external borrows.
        [[nodiscard]] virtual task::ETaskAffinity taskAffinity() const noexcept;

        // One finite computation. Do not wait for IO or borrow this time beyond the call.
        virtual void execute(const SimulationTime& time) noexcept = 0;

        // Controls subsequent execution, without cancelling work already accepted.
        void setEnable(bool enabled) noexcept;
        [[nodiscard]] bool isEnabled() const noexcept;

    protected:
        // One local Writer per producer; release before returning or suspending. Engine applies each batch.
        [[nodiscard]] cxx::expected<ecs::EcsCommandWriter, ecs::EcsCommandFailure>
        beginCommands(ecs::EEcsCommandPolicy policy = ecs::EEcsCommandPolicy::ABORT_BATCH) noexcept;
        [[nodiscard]] std::optional<ecs::Entity> resolveEntity(ecs::DeferredEntity entity) const noexcept;
        // Borrowed from SceneSession after Engine adoption; never stop or join it from a System.
        [[nodiscard]] execution::TaskScope& taskScope() const noexcept;
        // Only professional resource release. Engine has already settled the Scene's tasks.
        [[nodiscard]] virtual exec::task<Result> releaseResources() noexcept;

    private:
        friend class Simulation;
        execution::TaskScope* tasks_ = nullptr;
        ecs::EcsCommandBuffer* commands_ = nullptr;
        std::size_t command_producer_ = SIZE_MAX;
        bool enabled_ = true;
    };
} // namespace lux::simulation
