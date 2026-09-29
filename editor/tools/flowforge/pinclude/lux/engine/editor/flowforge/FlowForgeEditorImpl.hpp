#pragma once
#include "LegacyPersistenceState.hpp"
#include <lux/engine/editor/flowforge/FlowEdit.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/editor/ui/CloseReview.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>
#include "FlowCompilationAccess.hpp"
#include <lux/engine/editor/detail/AssetSave.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/detail/AssetSource.hpp>
#include <unordered_map>

namespace lux::editor::flowforge
{
    struct FlowSourceCodec final
    {
        using Source = FlowAuthoringSource;
        static constexpr std::size_t max_bytes = 16U * 1024U * 1024U;
        lux::flowforge::FlowSourceEnvironment environment_;
        static asset::AssetId identity(const Source& value) noexcept
        {
            return value.id;
        }
        EditorResult<Source> decode(const lux::cxx::SharedBytes<>&, std::stop_token) const noexcept;
    };
    using FlowSave = detail::TAssetSave<lux::flowforge::FlowSource, FlowEncoder>;
    class FlowForgeEditor::Impl final
    {
        friend class FlowForgeEditorTestAccess;
        friend class FlowForgeEditor;

    public:
        explicit Impl(EditorContext& context);
        ~Impl();

    private:
        using NodeIndex = std::unordered_map<lux::flowforge::NodeId, const lux::flowforge::Node*>;
        using PinIndex = std::unordered_map<lux::flowforge::PinId, const lux::flowforge::Pin*>;
        class GraphElement;
        class ContentElement;

        editing::EditResult<void> canEdit() const noexcept;
        FlowEditObserver editObserver() noexcept;
        editing::EditResult<editing::ApplyResult> applyEdit(VFlowEdit edit);
        editing::EditResult<FlowEditIds> insertEdit(VFlowEdit edit);

        FlowForgeEditor* editor_{};
        // Declared before content: metadata/module leases outlive nodes, history and in-flight compilation.
        lux::flowforge::FlowSourceEnvironment environment_;
        FlowAuthoringSource source_;
        NodeIndex read_nodes_;
        PinIndex read_pins_;
        EditorContext& editor_context_;
        process::CompletionWork completion_work_;
        bool completion_pending_{};
        void adoptCompletions() noexcept;
        void applyChanges() noexcept;
        void adoptAssetResults();
        std::unique_ptr<editing::EditHistory> history_;
        transition::LegacyPersistenceState persistence_;
        AssetEditStatus asset_status_;
        std::optional<EditorResult<FlowSourceCodec::Source>> read_result_;
        process::Task reading_;
        std::optional<FlowAuthoringSource> candidate_;
        std::unique_ptr<editing::EditHistory> candidate_history_;
        transition::LegacyPersistenceState candidate_persistence_;
        asset::AssetId saved_identity_;
        std::unique_ptr<editing::EditHistory> saved_history_;
        transition::LegacyPersistenceState saved_persistence_;
        std::optional<SaveRequestId> change_save_;
        bool hide_requested_{};

        std::variant<std::monostate, FlowSave> save_;
        FlowCompilationService compilations_;
        FlowCompileId compilation_;
        std::string compile_name_;
        process::TaskId notified_compile_;
        const FlowCompileOperation* currentCompilation() const noexcept;
        std::uint64_t next_save_{1};
        bool busy_{}, finishing_interaction_{};

        EditorResult<void> changeAsset(EAssetChange, asset::AssetId = {}, bool reload = false);
        EditorResult<void> reviewAsset(EAssetChangeDecision, std::string_view);
        void startAssetChange();
        void applyAssetChange();
        EditorResult<SaveRequestId> requestSaveAs(std::string_view);
        void assetFailure(EditorFailure failure);
        void indexContent();
        EditorResult<lux::flowforge::FlowSource> capture() const;
        ProjectStorage& project() noexcept;
        const lux::flowforge::FlowSourceEnvironment& metadata() const noexcept;
        std::span<const lux::graph::NodeRecord> nodes() const noexcept;
        std::span<const lux::graph::PinRecord> pins() const noexcept;
        std::span<const lux::graph::LinkRecord> links() const noexcept;
        std::string_view nodeName(lux::flowforge::NodeId id) const noexcept;
        lux::flowforge::ENodeOperation nodeOperation(lux::flowforge::NodeId id) const noexcept;
        std::string_view pinName(lux::flowforge::PinId id) const noexcept;
        std::string_view pinType(lux::flowforge::PinId id) const noexcept;
        lux::graph::GraphNodeLayout nodeLayout(lux::flowforge::NodeId id) const noexcept;
        EditorResult<lux::flowforge::FlowSourceLiteral> pinLiteral(lux::flowforge::PinId id) const;
        editing::EditResult<editing::ApplyResult> setPinLiteral(
            lux::flowforge::PinId id,
            const lux::flowforge::FlowSourceLiteral& literal
        );
        editing::EditResult<editing::ApplyResult> rename(std::string_view name);
        editing::EditResult<lux::flowforge::NodeId> insertNode(
            std::unique_ptr<lux::flowforge::Node>& input,
            lux::graph::GraphNodeLayout placement = {}
        );
        EditorResult<lux::flowforge::FlowSourceNode> captureNode(lux::flowforge::NodeId id) const;
        editing::EditResult<lux::flowforge::NodeId> insertFunctionUse(
            lux::flowforge::NodeId id,
            bool return_node,
            lux::graph::GraphNodeLayout layout = {}
        );
        editing::EditResult<editing::ApplyResult> setFunctionSignature(
            editing::StateId base,
            lux::flowforge::NodeId id,
            std::string_view name,
            const lux::flowforge::FlowSourceSignature& signature
        );
        std::span<const lux::flowforge::ExportMethodNode> exports() const noexcept;
        editing::EditResult<editing::ApplyResult> setExports(std::vector<lux::flowforge::ExportMethodNode> exports);
        editing::EditResult<editing::ApplyResult> removeNodes(
            std::span<const lux::flowforge::NodeId> nodes,
            std::span<const lux::graph::LinkRecord> links
        );
        editing::EditResult<editing::ApplyResult> connect(lux::flowforge::PinId from, lux::flowforge::PinId to);
        editing::EditResult<editing::ApplyResult> disconnect(lux::flowforge::PinId from, lux::flowforge::PinId to);
        editing::EditResult<editing::ApplyResult> moveNodes(std::span<const lux::graph::GraphLayoutEntry> entries);
        std::span<const lux::flowforge::FlowGraph::GraphVariable> variables() const noexcept;
        editing::EditResult<std::uint64_t> addVariable(
            std::string_view name,
            std::string_view type,
            const lux::flowforge::FlowSourceLiteral& initial
        );
        editing::EditResult<editing::ApplyResult> setVariable(const lux::flowforge::FlowSourceVariable& value);
        editing::EditResult<editing::ApplyResult> removeVariable(std::uint64_t id);
        EditorResult<SaveRequestId> requestSave(std::string origin);
        std::span<const SaveRequestId> saveRequests() const noexcept;
        EditorResult<VSaveRequestStatus> saveStatus(SaveRequestId id) const;
        EditorResult<void> retrySave(SaveRequestId id);
        EditorResult<void> abandonSave(SaveRequestId id);
        EditorResult<void> acknowledgeSave(SaveRequestId id);
        EditorResult<lux::process::TaskId> requestCompile(std::filesystem::path linker = {});
        EditorResult<SaveRequestId> requestPublish(lux::process::TaskId compile, std::string origin);
        EditorResult<VFlowCompileStatus> compileStatus(lux::process::TaskId id) const;
        EditorResult<std::reference_wrapper<const lux::script::ScriptArtifact>> compiled(lux::process::TaskId id) const;
        EditorResult<void> retryLink(lux::process::TaskId id, std::filesystem::path linker = {});
        editing::HistoryId historyId() const noexcept;
        editing::EditResult<editing::HistoryTargetView> historyView() const noexcept;
        editing::EditResult<editing::HistoryTargetResult> undo() noexcept;
        editing::EditResult<editing::HistoryTargetResult> redo() noexcept;
        CloseRequest close_request_;
        bool close_prepared_{};
        std::optional<ECloseDecision> close_decision_;
        void event(object::EventView& event) noexcept;
        EditorResult<void> finishEditing();
        void update() noexcept;
        void createContent(EditorResult<void>& status);
        EditorResult<void> finishContentEditing();
        void applyContentIntents() noexcept;
        void contentCommand(object::EventView&) noexcept;
        std::unique_ptr<ContentElement> content_;
        object::Connection close_connection_;
    };
}
