#pragma once

#include <filesystem>
#include <lux/engine/editor/DocumentEditor.hpp>
#include <lux/engine/editor/documents/visibility.h>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
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
namespace lux::script
{
    class ScriptArtifact;
}

namespace lux::editor::flowforge
{
    inline constexpr std::string_view kFlowForgeDocumentType = "lux.editor.flowforge.v1";

    struct FlowCompileId final
    {
        DocumentHandle document;
        std::uint64_t serial{};
        friend bool operator==(FlowCompileId, FlowCompileId) = default;
    };
    enum class EFlowCompileStage : std::uint8_t
    {
        COMPILING,
        LINKING,
        PACKAGING
    };
    struct FlowCompilePending final
    {
        EFlowCompileStage stage{};
    };
    struct FlowCompileSucceeded final
    {
        editing::StateId captured;
        editing::Revision revision;
        bool current{};
    };
    struct FlowCompileFailed final
    {
        editing::StateId captured;
        editing::Revision revision;
        EditorFailure failure;
        bool retryable{};
    };
    using FlowCompileStatus = std::variant<FlowCompilePending, FlowCompileSucceeded, FlowCompileFailed>;

    class LUX_EDITOR_DOCUMENTS_PUBLIC LUX_OBJECT() FlowForgeEditor final : public object::Object<FlowForgeEditor>,
                                                                           public DocumentEditor
    {
      public:
        static const signal_type<editing::Revision> contentChanged;
        static const signal_type<FlowCompileId> compileFinished;

        [[nodiscard]] static EditorResult<std::unique_ptr<FlowForgeEditor>> open(
            const lux::flowforge::FlowSourceDocument &, Project &, process::ExecutionRuntime &,
            lux::flowforge::FlowSourceEnvironment = {});
        ~FlowForgeEditor() override;

        [[nodiscard]] DocumentSummary summary() const override;
        [[nodiscard]] EditorResult<lux::flowforge::FlowSourceDocument> capture() const;
        [[nodiscard]] EditorResult<lux::flowforge::FlowSourceNode> captureNode(lux::flowforge::NodeId) const;
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setFunctionSignature(
            editing::StateId, lux::flowforge::NodeId, std::string_view name,
            const lux::flowforge::FlowSourceSignature &);
        [[nodiscard]] editing::EditResult<lux::flowforge::NodeId> insertFunctionUse(lux::flowforge::NodeId definition,
                                                                                    bool return_node,
                                                                                    lux::graph::GraphNodeLayout = {});
        [[nodiscard]] std::span<const lux::flowforge::ExportMethodNode> exports() const noexcept;
        [[nodiscard]] Project &project() noexcept;
        [[nodiscard]] const lux::flowforge::FlowSourceEnvironment &metadata() const noexcept;
        [[nodiscard]] std::span<const lux::graph::NodeRecord> nodes() const noexcept;
        [[nodiscard]] std::span<const lux::graph::PinRecord> pins() const noexcept;
        [[nodiscard]] std::span<const lux::graph::LinkRecord> links() const noexcept;
        [[nodiscard]] std::string_view nodeName(lux::flowforge::NodeId) const noexcept;
        [[nodiscard]] lux::flowforge::ENodeOperation nodeOperation(lux::flowforge::NodeId) const noexcept;
        [[nodiscard]] std::string_view pinName(lux::flowforge::PinId) const noexcept;
        [[nodiscard]] std::string_view pinType(lux::flowforge::PinId) const noexcept;
        [[nodiscard]] EditorResult<lux::flowforge::FlowSourceLiteral> pinLiteral(lux::flowforge::PinId) const;
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setPinLiteral(
            lux::flowforge::PinId, const lux::flowforge::FlowSourceLiteral &);
        [[nodiscard]] lux::graph::GraphNodeLayout nodeLayout(lux::flowforge::NodeId) const noexcept;
        [[nodiscard]] editing::EditResult<editing::ApplyResult> rename(std::string_view);
        [[nodiscard]] std::span<const lux::flowforge::FlowGraph::GraphVariable> variables() const noexcept;
        [[nodiscard]] editing::EditResult<std::uint64_t> addVariable(std::string_view name, std::string_view type,
                                                                     const lux::flowforge::FlowSourceLiteral &initial);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setVariable(const lux::flowforge::FlowSourceVariable &);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> removeVariable(std::uint64_t);
        [[nodiscard]] editing::EditResult<lux::flowforge::NodeId> insertNode(std::unique_ptr<lux::flowforge::Node> &,
                                                                             lux::graph::GraphNodeLayout = {});
        [[nodiscard]] editing::EditResult<editing::ApplyResult> removeNodes(
            std::span<const lux::flowforge::NodeId>, std::span<const lux::graph::LinkRecord> links = {});
        [[nodiscard]] editing::EditResult<editing::ApplyResult> connect(lux::flowforge::PinId from,
                                                                        lux::flowforge::PinId to);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> disconnect(lux::flowforge::PinId from,
                                                                           lux::flowforge::PinId to);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> moveNodes(
            std::span<const lux::graph::GraphLayoutEntry>);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setExports(
            std::vector<lux::flowforge::ExportMethodNode>);

        [[nodiscard]] EditorResult<SaveRequestId> requestSave(std::string origin) override;
        [[nodiscard]] std::span<const SaveRequestId> saveRequests() const noexcept override;
        [[nodiscard]] EditorResult<SaveRequestStatus> saveStatus(SaveRequestId) const override;
        [[nodiscard]] EditorResult<void> retrySave(SaveRequestId) override;
        [[nodiscard]] EditorResult<void> abandonSave(SaveRequestId) override;
        [[nodiscard]] EditorResult<void> acknowledgeSave(SaveRequestId) override;
        [[nodiscard]] EditorResult<FlowCompileId> requestCompile(std::filesystem::path linker = {});
        [[nodiscard]] EditorResult<SaveRequestId> requestPublish(FlowCompileId, std::string origin);
        [[nodiscard]] EditorResult<FlowCompileStatus> compileStatus(FlowCompileId) const;
        [[nodiscard]] EditorResult<std::reference_wrapper<const lux::script::ScriptArtifact>> compiled(
            FlowCompileId) const;
        [[nodiscard]] EditorResult<void> acknowledgeCompile(FlowCompileId);
        [[nodiscard]] EditorResult<void> retryLink(FlowCompileId, std::filesystem::path linker = {});

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
        friend struct FlowCodec;
        struct Data;
        static EditorResult<std::unique_ptr<FlowForgeEditor>> adopt(lux::asset::AssetId, std::string,
                                                                    lux::flowforge::FlowGraph &, Project &,
                                                                    process::ExecutionRuntime &,
                                                                    lux::flowforge::FlowSourceEnvironment);
        FlowForgeEditor(object::ObjectDispatcherRef, std::unique_ptr<Data>);
        std::unique_ptr<Data> data_;
    };

    [[nodiscard]] LUX_EDITOR_DOCUMENTS_PUBLIC EditorResult<std::unique_ptr<DocumentOpening>> openFlowForgeDocument(
        Project &, const OpenDocumentRequest &, process::ExecutionRuntime &,
        lux::flowforge::FlowSourceEnvironment = {});
} // namespace lux::editor::flowforge
