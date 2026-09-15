#pragma once

#include <array>
#include <lux/engine/editor/DocumentEditor.hpp>
#include <lux/engine/editor/documents/visibility.h>
#include <lux/engine/material/graph/MaterialSource.hpp>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>

namespace lux::editor
{
    class Project;
}
namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::rdesc
{
    struct MaterialDescription;
}

namespace lux::editor::material
{
    inline constexpr std::string_view kMaterialDocumentType = "lux.editor.material.v1";

    struct MaterialCompileId final
    {
        DocumentHandle document;
        std::uint64_t serial{};
        friend bool operator==(MaterialCompileId, MaterialCompileId) = default;
    };
    struct MaterialCompilePending final
    {
    };
    struct MaterialCompileSucceeded final
    {
        editing::StateId captured;
        editing::Revision revision;
        bool current{};
    };
    struct MaterialCompileFailed final
    {
        editing::StateId captured;
        editing::Revision revision;
        EditorFailure failure;
    };
    using MaterialCompileStatus = std::variant<MaterialCompilePending, MaterialCompileSucceeded, MaterialCompileFailed>;

    class LUX_EDITOR_DOCUMENTS_PUBLIC LUX_OBJECT() MaterialEditor final : public object::Object<MaterialEditor>,
                                                                          public DocumentEditor
    {
      public:
        static const signal_type<editing::Revision> contentChanged;
        static const signal_type<MaterialCompileId> compileFinished;

        [[nodiscard]] static EditorResult<std::unique_ptr<MaterialEditor>> open(lux::material::MaterialSourceDocument &,
                                                                                Project &, process::ExecutionRuntime &);
        ~MaterialEditor() override;

        [[nodiscard]] DocumentSummary summary() const override;
        [[nodiscard]] const lux::material::MaterialSourceDocument &source() const noexcept;
        [[nodiscard]] Project &project() noexcept;
        [[nodiscard]] editing::EditResult<editing::ApplyResult> rename(std::string_view);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setConstant(lux::material::NodeId,
                                                                            const std::array<float, 4> &);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setShadingModel(lux::rdesc::ELightingTechnique);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setRenderState(lux::material::RenderState);
        // The replacement is a Pane-owned draft of an existing node. Failure retains it and all links.
        [[nodiscard]] editing::EditResult<editing::ApplyResult> replaceNode(editing::StateId,
                                                                            std::unique_ptr<lux::material::Node> &);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setTextureSlots(
            editing::StateId, std::span<const lux::material::TextureSlotDecl>);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setParameterSlots(
            editing::StateId, std::span<const lux::material::ParamSlotDecl>);
        [[nodiscard]] editing::EditResult<lux::material::NodeId> insertNode(std::unique_ptr<lux::material::Node> &,
                                                                            lux::graph::GraphNodeLayout = {});
        [[nodiscard]] editing::EditResult<editing::ApplyResult> removeNodes(
            std::span<const lux::material::NodeId>, std::span<const lux::graph::LinkRecord> links = {});
        [[nodiscard]] editing::EditResult<editing::ApplyResult> connect(lux::material::PinId from,
                                                                        lux::material::PinId to);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> disconnect(lux::material::PinId from,
                                                                           lux::material::PinId to);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> moveNodes(
            std::span<const lux::graph::GraphLayoutEntry>);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> moveNode(lux::material::NodeId,
                                                                         lux::graph::GraphNodeLayout);

        [[nodiscard]] EditorResult<SaveRequestId> requestSave(std::string origin) override;
        [[nodiscard]] std::span<const SaveRequestId> saveRequests() const noexcept override;
        [[nodiscard]] EditorResult<SaveRequestStatus> saveStatus(SaveRequestId) const override;
        [[nodiscard]] EditorResult<void> retrySave(SaveRequestId) override;
        [[nodiscard]] EditorResult<void> abandonSave(SaveRequestId) override;
        [[nodiscard]] EditorResult<void> acknowledgeSave(SaveRequestId) override;
        [[nodiscard]] EditorResult<MaterialCompileId> requestCompile();
        [[nodiscard]] EditorResult<SaveRequestId> requestPublish(MaterialCompileId, std::string origin);
        [[nodiscard]] EditorResult<MaterialCompileStatus> compileStatus(MaterialCompileId) const;
        [[nodiscard]] EditorResult<std::reference_wrapper<const lux::rdesc::MaterialDescription>> compiled(
            MaterialCompileId) const;
        [[nodiscard]] EditorResult<void> acknowledgeCompile(MaterialCompileId);

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
        void beginClose() noexcept;
        struct Data;
        MaterialEditor(object::ObjectDispatcherRef, std::unique_ptr<Data>);
        std::unique_ptr<Data> data_;
    };

    [[nodiscard]] LUX_EDITOR_DOCUMENTS_PUBLIC EditorResult<std::unique_ptr<DocumentOpening>> openMaterialDocument(
        Project &, const OpenDocumentRequest &, process::ExecutionRuntime &);
} // namespace lux::editor::material
