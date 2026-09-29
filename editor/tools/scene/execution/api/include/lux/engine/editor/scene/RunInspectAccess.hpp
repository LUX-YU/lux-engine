#pragma once

#include <lux/engine/editor/scene/RunTypes.hpp>

namespace lux::editor::scene
{
    class RunStore;
    struct RunningObjectRef final
    {
        RunId run;
        lux::scene::SceneInstanceId instance;
        simulation::ecs::Entity entity{simulation::ecs::NullEntity};
        friend bool operator==(RunningObjectRef, RunningObjectRef) = default;
    };

    // Read-only, owner-thread borrows end at the next drive/structural change/owner wait.
    class RunInspectAccess final
    {
    public:
        [[nodiscard]] RunResult<std::reference_wrapper<const simulation::ecs::Registry>> borrow(RunId) const noexcept;
        [[nodiscard]] bool contains(RunningObjectRef) const noexcept;
        [[nodiscard]] RunResult<RunningObjectRef> reference(RunId, simulation::ecs::Entity) const noexcept;

    private:
        friend class RunStore;
        explicit RunInspectAccess(const RunStore& store) noexcept : store_(store) {}
        const RunStore& store_;
    };
}
