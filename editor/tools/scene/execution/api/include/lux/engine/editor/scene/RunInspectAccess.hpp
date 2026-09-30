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
        [[nodiscard]] bool contains(RunningObjectRef ref) const noexcept
        {
            return contains_(store_, ref);
        }
        [[nodiscard]] RunResult<RunningObjectRef> reference(RunId, simulation::ecs::Entity) const noexcept;

    private:
        friend class RunStore;
        using Contains = bool (*)(const RunStore&, RunningObjectRef) noexcept;
        explicit RunInspectAccess(const RunStore& store, Contains contains) noexcept
            : store_(store), contains_(contains)
        {}
        const RunStore& store_;
        Contains contains_;
    };
}
