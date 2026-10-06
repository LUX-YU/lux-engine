#pragma once

#include <lux/engine/editor/scene/RunTypes.hpp>
#include <memory>

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
        // Retains the actual allocation, not a Session/Registry alias. inspect() remains an explicit
        // non-owning borrow for stack-owned stores; registered UI uses this owning access instead.
        [[nodiscard]] static RunResult<RunInspectAccess> create(std::shared_ptr<const RunStore>) noexcept;
        RunInspectAccess(const RunInspectAccess&) = default;
        RunInspectAccess(RunInspectAccess&&) noexcept = default;
        RunInspectAccess& operator=(const RunInspectAccess&) = delete;
        RunInspectAccess& operator=(RunInspectAccess&&) = delete;
        using BorrowResult = RunResult<std::reference_wrapper<const simulation::ecs::Registry>>;
        [[nodiscard]] BorrowResult borrow(RunId id) const noexcept
        {
            return borrow_(store_, id);
        }
        [[nodiscard]] RunResult<RunInfo> describe(RunId id) const
        {
            return describe_(store_, id);
        }
        [[nodiscard]] bool contains(RunningObjectRef ref) const noexcept
        {
            return contains_(store_, ref);
        }
        [[nodiscard]] RunResult<RunningObjectRef> reference(RunId id, simulation::ecs::Entity entity) const noexcept
        {
            return reference_(store_, id, entity);
        }

    private:
        friend class RunStore;
        using Contains = bool (*)(const RunStore&, RunningObjectRef) noexcept;
        using Borrow = BorrowResult (*)(const RunStore&, RunId) noexcept;
        using Describe = RunResult<RunInfo> (*)(const RunStore&, RunId);
        using Reference = RunResult<RunningObjectRef> (*)(const RunStore&, RunId, simulation::ecs::Entity) noexcept;
        explicit RunInspectAccess(
            const RunStore& store,
            Contains contains,
            Borrow borrow,
            Describe describe,
            Reference reference
        ) noexcept
            : store_(store), contains_(contains), borrow_(borrow), describe_(describe), reference_(reference)
        {}
        std::shared_ptr<const RunStore> owner_;
        const RunStore& store_;
        Contains contains_;
        Borrow borrow_;
        Describe describe_;
        Reference reference_;
    };
}
