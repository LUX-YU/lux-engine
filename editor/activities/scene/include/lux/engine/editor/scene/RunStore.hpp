#pragma once

#include <lux/engine/editor/scene/RunInspectAccess.hpp>
#include <lux/engine/editor/scene/StartRunOperation.hpp>
#include <lux/engine/editor/scene/SceneSnapshot.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/cxx/core/function_ref.hpp>

namespace lux::services
{
    struct ServiceDescriptor;
}

namespace lux::scene
{
    struct ScriptRuntimeHost;
}

namespace lux::editor::scene
{
    extern const services::ServiceDescriptor kRunStoreService;
    class SceneSession;
    class SceneEditing;
    struct RunEnvironment final
    {
        simulation::ecs::ComponentSchemaSet components;
        std::shared_ptr<const simulation::SimulationSystemRegistry> simulation_systems;
        std::vector<lux::scene::SceneSystemRegistration> scene_systems;
        std::vector<lux::scene::RenderFeatureSceneBinding> render_bindings;
        render::RenderRuntime* renderer{};
        lux::scene::RenderResources* resources{};
        lux::scene::RenderAssetInput assets;
        // Fixed engine-only script inputs; retained through this Run's instance retirement.
        std::shared_ptr<lux::scene::ScriptRuntimeHost> scripts;
    };

    // Owns runs and their sole instance leases. Never drives SceneRuntime or borrows live author state.
    class RunStore final
    {
    public:
        explicit RunStore(lux::scene::SceneRuntime&, process::ExecutionRuntime&, std::size_t capacity = 16);
        ~RunStore();
        RunStore(const RunStore&) = delete;
        RunStore& operator=(const RunStore&) = delete;
        RunStore(RunStore&&) = delete;
        RunStore& operator=(RunStore&&) = delete;

        [[nodiscard]] RunResult<std::unique_ptr<StartRunOperation>> prepare(
            SceneSession&,
            RunEnvironment,
            RunConfiguration = {}
        );
        [[nodiscard]] RunResult<std::unique_ptr<StartRunOperation>> prepare(
            SceneSnapshot,
            RunEnvironment,
            RunConfiguration = {}
        );
        // Owner safe point only. A RunId is published only after successful instance construction.
        [[nodiscard]] RunResult<RunId> adopt(StartRunOperation&);

        [[nodiscard]] RunResult<RunInfo> info(RunId) const;
        [[nodiscard]] RunResult<void> pause(RunId) noexcept;
        [[nodiscard]] RunResult<void> resume(RunId);
        [[nodiscard]] RunResult<StepTicket> step(RunId);
        // Results survive instance reclamation until individual acknowledgement or acknowledgeStop.
        [[nodiscard]] RunResult<lux::scene::SceneStepStatus> stepStatus(StepTicket) const noexcept;
        [[nodiscard]] RunResult<void> acknowledgeStep(StepTicket) noexcept;
        [[nodiscard]] RunResult<StopTicket> stop(RunId);
        // Final acknowledgement clears every remaining terminal step, then releases the Run slot.
        [[nodiscard]] RunResult<void> acknowledgeStop(RunId);
        // Receives actual drive/retirement facts, then maintains pause debug state. No tick here.
        [[nodiscard]] RunResult<void> update();
        // No live or unacknowledged Run remains. Preparation results have their own operation owner.
        [[nodiscard]] RunResult<bool> settled() const noexcept;
        [[nodiscard]] RunInspectAccess inspect() const noexcept;
        [[nodiscard]] RunResult<std::reference_wrapper<SceneEditing>> debugEditing(RunId) noexcept;
        [[nodiscard]] RunResult<std::reference_wrapper<editing::EditHistory>> debugHistory(RunId) noexcept;
        [[nodiscard]] RunResult<void> finishEditing(RunId);
        // Synchronous borrows protected against callback-driven resume, stop and slot reclamation.
        using Inspect = cxx::function_ref<
            RunResult<void>(const simulation::ecs::Registry&, const std::optional<editing::HistorySnapshot>&)>;
        using Edit = cxx::function_ref<RunResult<void>(SceneEditing&)>;
        [[nodiscard]] RunResult<void> withInspection(RunningObjectRef, Inspect);
        [[nodiscard]] RunResult<void> withEditing(RunningObjectRef, editing::StateId, editing::Revision, Edit);

    private:
        friend class RunInspectAccess;
        [[nodiscard]] RunResult<std::unique_ptr<StartRunOperation>> launch(std::shared_ptr<StartRunOperation::Impl>);
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
