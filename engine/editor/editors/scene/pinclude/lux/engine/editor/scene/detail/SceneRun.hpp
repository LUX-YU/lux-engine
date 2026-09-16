#pragma once

#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneRun.hpp>
#include <lux/engine/editor/scene/detail/SceneResources.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>

namespace lux::editor::scene::detail
{
    class SceneRun final
    {
      public:
        SceneRun(process::ExecutionRuntime &, rendering::EditorRenderer &,
                 std::shared_ptr<const lux::scene::SceneMetaManager>, std::shared_ptr<SceneRunSlot>);
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
        void poll(std::size_t budget);
        [[nodiscard]] const RunStatus &status() const noexcept;
        [[nodiscard]] EditorResult<RunViewLease> openView(RunId, rendering::ViewConfig);
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] double coordinatePageSize() const noexcept;

      private:
        struct Data;
        std::unique_ptr<Data> data_;
    };
} // namespace lux::editor::scene::detail
