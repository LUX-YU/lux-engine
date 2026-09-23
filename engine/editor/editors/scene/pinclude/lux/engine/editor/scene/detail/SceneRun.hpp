#pragma once

namespace lux::scene
{
class SceneInstance;
}

#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneRun.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/simulation/Simulation.hpp>

namespace lux::editor::scene
{
struct SceneEditorMetadata;
}

namespace lux::editor::scene::detail
{
struct SceneObjects;
class SceneRun final
{
  public:
    SceneRun(process::ExecutionRuntime &, process::TaskScope &, lux::render::RenderRuntime &, SceneEditorMetadata,
             std::shared_ptr<SceneRunSlot>);
    ~SceneRun();
    SceneRun(const SceneRun &) = delete;
    SceneRun &operator=(const SceneRun &) = delete;
    [[nodiscard]] EditorResult<void> validateStart(std::chrono::nanoseconds) const;
    EditorResult<RunId> start(DocumentHandle, editing::HistoryId, editing::StateId, SceneCapture,
                              std::shared_ptr<lux::scene::RenderAssetSource>, std::chrono::nanoseconds);
    EditorResult<void> pause(RunId);
    EditorResult<void> resume(RunId);
    EditorResult<void> step(RunId);
    EditorResult<void> stop(RunId);
    void poll(PollBudget &budget, bool may_release_world);
    [[nodiscard]] SceneObjects *objects() noexcept;
    [[nodiscard]] lux::scene::SceneInstance *scene() noexcept;
    [[nodiscard]] bool takeCatalogChange() noexcept;
    [[nodiscard]] editing::EditHistory *history() noexcept;
    void invalidateDerived() noexcept;
    [[nodiscard]] const RunStatus &status() const noexcept;
    [[nodiscard]] EditorResult<std::unique_ptr<lux::render::RenderView>> openView(RunId, lux::render::ViewConfig);
    [[nodiscard]] bool settled() const noexcept;
    [[nodiscard]] double coordinatePageSize() const noexcept;

  private:
    struct Data;
    std::unique_ptr<Data> data_;
};
} // namespace lux::editor::scene::detail
