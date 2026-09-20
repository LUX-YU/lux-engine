#pragma once

namespace lux::scene { class Scene; }

#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneRun.hpp>
#include <lux/engine/editor/scene/detail/SceneResources.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>

namespace lux::editor::scene
{
    struct SceneEditorMetadata;
}

namespace lux::editor::scene::detail
{
    struct SceneObjects;
    EditorResult<std::shared_ptr<const lux::scene::SceneDescription>> editorSceneDescription(const NativeScene &);

    // Shared author/Run assembly decision. Absence means the description has no
    // RenderSystem, not an incomplete Scene or a fallback renderer.
    EditorResult<std::unique_ptr<lux::scene::SceneRenderBinding>> beginSceneRendering(rendering::EditorRenderer &,
                                                                                      const NativeScene &,
                                                                                      const SceneEditorMetadata &,
                                                                                      bool author_view = true);

    class SceneRun final
    {
      public:
        SceneRun(process::ExecutionRuntime &, rendering::EditorRenderer &, SceneEditorMetadata,
                 std::shared_ptr<SceneRunSlot>);
        ~SceneRun();
        SceneRun(const SceneRun &) = delete;
        SceneRun &operator=(const SceneRun &) = delete;
        [[nodiscard]] EditorResult<void> validateStart(std::chrono::nanoseconds) const;
        EditorResult<RunId> start(DocumentHandle, editing::HistoryId, editing::StateId, SceneCapture, SceneResourcePins,
                                  std::chrono::nanoseconds);
        EditorResult<void> pause(RunId);
        EditorResult<void> resume(RunId);
        EditorResult<void> step(RunId);
        EditorResult<void> stop(RunId);
        void poll(PollBudget &budget, bool may_release_world);
        [[nodiscard]] SceneObjects *objects() noexcept;
        [[nodiscard]] lux::scene::Scene *scene() noexcept;
        [[nodiscard]] lux::scene::SceneRenderBinding *renderBinding() noexcept;
        [[nodiscard]] bool takeCatalogChange() noexcept;
        [[nodiscard]] editing::EditHistory *history() noexcept;
        void invalidateDerived() noexcept;
        [[nodiscard]] const RunStatus &status() const noexcept;
        [[nodiscard]] EditorResult<RunViewLease> openView(RunId, rendering::ViewConfig);
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] double coordinatePageSize() const noexcept;

      private:
        struct Data;
        std::unique_ptr<Data> data_;
    };
} // namespace lux::editor::scene::detail
