#pragma once

#include <lux/engine/editor/DocumentEditor.hpp>
#include <lux/engine/editor/documents/visibility.h>
#include <lux/engine/editor/scene/SceneResources.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>
#include <lux/engine/world/WorldObjectId.hpp>

namespace lux::editor::rendering
{
    class EditorRenderer;
}

namespace lux::editor::scene
{
    inline constexpr std::string_view kSceneDocumentType = "lux.editor.scene.v1";

    struct SceneObjectRow final
    {
        lux::world::WorldObjectId object;
        lux::world::WorldObjectId parent;
        std::string label;
    };

    struct SelectionNotice final
    {
        lux::world::WorldObjectId object;
        std::uint64_t revision{};
    };

    struct SceneComponentInfo final
    {
        lux::cxx::TypeToken type;
        std::string name;
    };

    struct NativeScene;

    class LUX_EDITOR_DOCUMENTS_PUBLIC LUX_OBJECT() SceneEditor final : public lux::object::Object<SceneEditor>,
                                                                       public DocumentEditor
    {
      public:
        static const signal_type<SelectionNotice> selectionChanged;
        static const signal_type<std::uint64_t> resourcesChanged;

        [[nodiscard]] static EditorResult<std::unique_ptr<SceneEditor>> open(
            NativeScene &, Project &, rendering::EditorRenderer &, std::shared_ptr<const lux::scene::SceneMetaManager>);
        ~SceneEditor() override;

        [[nodiscard]] DocumentSummary summary() const override;
        [[nodiscard]] std::span<const SceneObjectRow> objects() const noexcept;
        [[nodiscard]] SelectionNotice selection() const noexcept;
        [[nodiscard]] EditorResult<void> select(lux::world::WorldObjectId);
        [[nodiscard]] std::vector<SceneComponentInfo> components(lux::world::WorldObjectId) const;
        [[nodiscard]] const void *component(lux::world::WorldObjectId, lux::cxx::TypeToken) const noexcept;
        [[nodiscard]] std::shared_ptr<const SceneResourceSnapshot> resources() const noexcept;
        [[nodiscard]] SceneResult<void> retryResource(const ResourceRequestKey &);
        [[nodiscard]] const Project &project() const noexcept;
        [[nodiscard]] std::string diagnostic() const;
        [[nodiscard]] EditorResult<lux::render::RenderSceneId> renderScene() const;
        [[nodiscard]] double coordinatePageSize() const noexcept;
        [[nodiscard]] EditorResult<void> addViews(std::vector<std::unique_ptr<DocumentView>> &);
        [[nodiscard]] std::span<const std::unique_ptr<DocumentView>> views() const noexcept override;

        [[nodiscard]] editing::HistoryId historyId() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetView> historyView() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> undo() noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> redo() noexcept override;
        void requestClose() noexcept override;
        [[nodiscard]] CloseStatus closeStatus() const override;
        void poll(PollBudget &) override;

      private:
        struct Data;
        SceneEditor(object::ObjectDispatcherRef, std::unique_ptr<Data>);
        std::unique_ptr<Data> data_;
    };

    [[nodiscard]] LUX_EDITOR_DOCUMENTS_PUBLIC EditorResult<std::unique_ptr<DocumentOpening>> openSceneDocument(
        Project &, const OpenDocumentRequest &, process::ExecutionRuntime &, rendering::EditorRenderer &,
        std::shared_ptr<const lux::scene::SceneMetaManager>);
} // namespace lux::editor::scene
