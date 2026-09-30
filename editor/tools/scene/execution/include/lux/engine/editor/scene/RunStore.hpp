#pragma once

#include <lux/engine/editor/scene/RunInspectAccess.hpp>
#include <lux/engine/editor/scene/SceneSnapshot.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>

namespace lux::editor::scene
{
    class RunController;
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
    };

    // Owns runs and their sole instance leases. Never drives SceneRuntime or borrows live author state.
    class RunStore final
    {
    public:
        explicit RunStore(lux::scene::SceneRuntime&, process::ExecutionRuntime&, std::size_t capacity = 16);
        ~RunStore();
        RunStore(const RunStore&) = delete;
        RunStore& operator=(const RunStore&) = delete;

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
        [[nodiscard]] RunInspectAccess inspect() const noexcept;
        [[nodiscard]] RunResult<std::reference_wrapper<SceneEditing>> debugEditing(RunId) noexcept;
        [[nodiscard]] RunResult<std::reference_wrapper<editing::EditHistory>> debugHistory(RunId) noexcept;
        [[nodiscard]] RunResult<void> finishEditing(RunId);

    private:
        friend class RunController;
        friend class RunInspectAccess;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
