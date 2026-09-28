#pragma once
#include "LegacyPersistenceState.hpp"
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/editor/metadata/SceneRegistrations.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>
#include <lux/engine/editor/ui/CloseReview.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/detail/AssetSave.hpp>
#include <lux/engine/editor/detail/AssetSource.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <unordered_map>

namespace lux::editor::material
{
    using MaterialSave = detail::TAssetSave<lux::material::MaterialSource, MaterialEncoder>;
    struct MaterialSourceCodec final
    {
        using Source = lux::material::MaterialSource;
        static constexpr std::size_t max_bytes = 16U * 1024U * 1024U;
        static asset::AssetId identity(const Source& source) noexcept
        {
            return source.id;
        }
        static EditorResult<Source> decode(const lux::cxx::SharedBytes<>&, std::stop_token) noexcept;
    };
    class MaterialEditor::Impl final
    {
        friend class MaterialEditorTestAccess;
        friend class MaterialEditor;
        friend class ui::MaterialPreviewElement;

    public:
        explicit Impl(EditorContext& context);
        ~Impl();

    private:
        template <class Access> class TValueEdit;

        struct GraphDelta;
        class GraphEditOperation;
        class GraphElement;
        class Content;

        editing::EditResult<void> canEdit() const noexcept;
        editing::EditResult<editing::ApplyResult> editGraph(GraphDelta before, GraphDelta after, std::string label);

        template <class Access>
        editing::EditResult<editing::ApplyResult> change(
            Access access,
            typename Access::Value value,
            std::string label
        );

        // The preview resources outlive content Elements that borrow their scene.
        inline static constexpr lux::system::SystemInstanceId preview_render_system_{2};
        struct Preview;
        std::unique_ptr<Preview> preview_;
        std::string preview_failure_;
        MaterialEditor* editor_{};
        lux::material::MaterialSource source_;
        EditorContext& editor_context_;
        process::CompletionWork completion_work_;
        bool completion_deferred_{};
        void adoptCompletions() noexcept;
        void applyChanges() noexcept;
        void adoptAssetResults();
        std::unique_ptr<editing::EditHistory> history_;
        transition::LegacyPersistenceState persistence_;
        AssetEditStatus asset_status_;
        std::optional<EditorResult<MaterialSourceCodec::Source>> read_result_;
        process::Task reading_;
        std::optional<lux::material::MaterialSource> candidate_;
        std::unique_ptr<editing::EditHistory> candidate_history_;
        transition::LegacyPersistenceState candidate_persistence_;
        asset::AssetId saved_identity_;
        std::unique_ptr<editing::EditHistory> saved_history_;
        transition::LegacyPersistenceState saved_persistence_;
        std::optional<SaveRequestId> change_save_;
        bool hide_requested_{};
        std::variant<std::monostate, MaterialSave> save_;
        struct Compilation final
        {
            process::TaskId id;
            editing::HistoryId history;
            editing::StateId state;
            editing::Revision revision;
            lux::scene::RenderAssetInput preview_assets;
            VMaterialCompileStatus status{MaterialCompilePending{}};
            std::optional<MaterialCompiled> output;
        };
        std::variant<std::monostate, Compilation> compilation_;
        std::optional<EditorResult<MaterialCompiled>> compile_result_;
        process::Task compile_task_;
        std::uint64_t next_save_{1};
        bool busy_{}, finishing_interaction_{};

        EditorResult<void> changeAsset(EAssetChange, asset::AssetId = {}, bool reload = false);
        EditorResult<void> reviewAsset(EAssetChangeDecision, std::string_view);
        void startAssetChange();
        void applyAssetChange();
        EditorResult<SaveRequestId> requestSaveAs(std::string_view);
        EditorResult<void> resetPreview();
        void assetFailure(EditorFailure failure);
        const lux::material::MaterialSource& source() const noexcept;
        ProjectStorage& project() noexcept;
        editing::EditResult<editing::ApplyResult> rename(std::string_view name);
        editing::EditResult<editing::ApplyResult> setConstant(
            lux::material::NodeId node,
            const std::array<float, 4>& value
        );
        editing::EditResult<editing::ApplyResult> setShadingModel(lux::rdesc::ELightingTechnique value);
        editing::EditResult<editing::ApplyResult> setRenderState(lux::material::RenderState state);
        editing::EditResult<editing::ApplyResult> setTextureSlots(
            editing::StateId base,
            std::span<const lux::material::TextureSlotDecl> slots
        );
        editing::EditResult<editing::ApplyResult> setParameterSlots(
            editing::StateId base,
            std::span<const lux::material::ParamSlotDecl> slots
        );
        editing::EditResult<editing::ApplyResult> replaceNode(
            editing::StateId base,
            std::unique_ptr<lux::material::Node>& replacement
        );
        editing::EditResult<lux::material::NodeId> insertNode(
            std::unique_ptr<lux::material::Node>& node,
            lux::graph::GraphNodeLayout placement = {}
        );
        editing::EditResult<editing::ApplyResult> removeNodes(
            std::span<const lux::material::NodeId> nodes,
            std::span<const lux::graph::LinkRecord> links
        );
        editing::EditResult<editing::ApplyResult> connect(lux::material::PinId from, lux::material::PinId to);
        editing::EditResult<editing::ApplyResult> disconnect(lux::material::PinId from, lux::material::PinId to);
        editing::EditResult<editing::ApplyResult> moveNode(
            lux::material::NodeId node,
            lux::graph::GraphNodeLayout value
        );
        editing::EditResult<editing::ApplyResult> moveNodes(std::span<const lux::graph::GraphLayoutEntry> entries);
        EditorResult<SaveRequestId> requestSave(std::string origin);
        std::span<const SaveRequestId> saveRequests() const noexcept;
        EditorResult<VSaveRequestStatus> saveStatus(SaveRequestId id) const;
        EditorResult<void> retrySave(SaveRequestId id);
        EditorResult<void> abandonSave(SaveRequestId id);
        EditorResult<void> acknowledgeSave(SaveRequestId id);
        EditorResult<lux::process::TaskId> requestCompile();
        EditorResult<SaveRequestId> requestPublish(lux::process::TaskId compile, std::string origin);
        EditorResult<VMaterialCompileStatus> compileStatus(lux::process::TaskId id) const;
        EditorResult<std::reference_wrapper<const lux::rdesc::MaterialDescription>> compiled(lux::process::TaskId id
        ) const;
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
        EditorResult<void> createPreview();
        EditorResult<lux::scene::RenderAssetInput> capturePreviewAssets();
        void updatePreview(lux::cxx::SharedBytes<> bytes, editing::StateId state, lux::scene::RenderAssetInput base);
        void maintainPreview() noexcept;
        lux::simulation::ecs::Entity previewCamera() const noexcept;
        lux::scene::SceneInstanceId previewInstance() const noexcept;
        std::string previewStatus() const;
        EditorResult<void> navigatePreview(
            const lux::simulation::ecs::Transform3D& pose,
            const lux::scene::Camera& camera
        );
        void createContent(EditorResult<void>& status);
        EditorResult<void> finishContentEditing();
        void applyContentIntents() noexcept;
        void contentCommand(object::EventView&) noexcept;
        std::unique_ptr<Content> content_;
        object::Connection close_connection_;
    };
}
