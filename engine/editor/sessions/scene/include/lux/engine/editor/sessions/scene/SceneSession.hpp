#pragma once
// No Frame/Pane/ImGui/GLFW/Vulkan dependency. Editing core declarations are existing, not redefined.
#include <lux/engine/editor/sessions/SessionTypes.hpp>
#include <lux/engine/editor/sessions/scene/visibility.h>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <lux/engine/editor/rendering/RendererConfig.hpp>
#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/editor/editing/EditHistoryTarget.hpp>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace lux::editor::rendering
{
    class EditorRenderer;
}
namespace lux::editor::sessions
{
    namespace detail { struct SceneTestAccess; }
    enum class ESceneAccess : std::uint8_t
    {
        INSPECT_LIVE,
        EDIT_CONTENT
    };
    enum class ESceneError : std::uint8_t
    {
        INVALID_ARGUMENT,
        WRONG_THREAD,
        BUSY,
        NOT_READY,
        CLOSED,
        READ_ONLY,
        STALE_SESSION,
        STALE_ENTITY,
        STALE_CONTENT,
        UNSUPPORTED_EDIT,
        RESOURCE_FAILURE,
        SCENE_BUILD_FAILURE,
        SCENE_EXECUTION_FAILURE,
        ALLOCATION_FAILURE,
        CONTRACT_FAILURE,
        HISTORY_FAILURE,
        EXECUTOR_FAILURE
    };
    struct SceneFailure final
    {
        ESceneError code{};
        SessionId session;
        OperationContext context;
        std::optional<lux::scene::SceneBuildFailure> build;
        std::optional<lux::scene::SceneExecutionFailure> execution;
        std::optional<lux::simulation::SimulationExecutionFailure> simulation;
        std::optional<rendering::RendererFailure> renderer;
        std::optional<lux::process::asset_loading::AssetLoadFailure> asset;
        std::optional<editing::EditFailure> history;
        std::optional<lux::task::TaskExecutorFailure> executor;
    };
    template <class T> using SceneResult = lux::cxx::expected<T, SceneFailure>;
    struct SceneEntityRef final
    {
        SessionId session;
        lux::simulation::ecs::Entity entity{lux::simulation::ecs::NullEntity};
        friend bool operator==(SceneEntityRef, SceneEntityRef) noexcept = default;
    };
    struct SceneObjectRef final
    {
        SessionId session;
        lux::world::WorldObjectId object;
    };
    struct SceneRow final
    {
        SceneEntityRef target;
        std::optional<SceneObjectRef> authored;
        std::optional<SceneEntityRef> parent;
        std::string label;
        bool has_mesh{}, has_light{}, resources_ready{};
    };
    struct SceneOutlineSnapshot final
    {
        ChangeStamp stamp;
        std::vector<SceneRow> rows;
    };
    using SceneOutlineRef = std::shared_ptr<const SceneOutlineSnapshot>;
    struct SceneSelectionNotice final
    {
        SessionId session;
        std::uint64_t selection_revision{};
        std::optional<SceneEntityRef> current;
    };
    struct SceneContentNotice final
    {
        ChangeStamp stamp;
        editing::EApplyKind origin{};
    };
    struct ComponentDisplayInfo final
    {
        std::string canonical_schema;
        std::uint32_t version{};
        std::string display_name;
        bool has_read_binding{}, editable{};
    };
    struct SceneReadData final
    {
        SceneEntityRef target;
        ChangeStamp stamp;
        std::optional<lux::simulation::ecs::Transform3D> transform;
        std::optional<lux::simulation::ecs::WorldTransform3D> world_transform;
        std::optional<lux::simulation::ecs::Mesh3D> mesh;
        std::optional<lux::simulation::ecs::Light3D> light;
        std::vector<ComponentDisplayInfo> components;
        // Other pre-existing generated fields require a typed read binding; see §12.
        // This descriptor list is not permission to drop their existing display functionality.
    };
    struct SceneResourceNotice final
    {
        SessionId session;
        std::uint64_t resource_revision{};
    };
    struct PropertyGesture final
    {
        SessionId session;
        std::uint64_t sequence{};
    };
    struct SceneOpenInfo;
    struct SceneEditInput; // ER-2 only, actual finite author data and validated recovery operations.
    struct SceneOwnerUpdate final
    {
        std::uint64_t cycle{};    // Application cycle, NOT a Simulation step or permission token.
        double elapsed_seconds{}; // Finite, nonnegative; same cycle requires same input.
    };
    struct SceneResourceSnapshot; // Define in PUBLIC SceneResourceStatus.hpp: immutable owned rows, not jobs.
    struct SceneCloseSnapshot;
    struct ResourceRequestKey;    // Define the value in PUBLIC SceneResourceStatus.hpp; request state remains private.
    class LUX_EDITOR_SCENE_SESSION_PUBLIC LUX_OBJECT() SceneSession final : public lux::object::Object<SceneSession>,
                                                                            public editing::EditHistoryTarget
    {
    public:
        static const signal_type<SceneSelectionNotice> selectionChanged;
        static const signal_type<SceneContentNotice> contentChanged;
        static const signal_type<SceneResourceNotice> resourcesChanged;
        [[nodiscard]] static SceneResult<std::unique_ptr<SceneSession>> openInspection(SceneOpenInfo &) noexcept;
        [[nodiscard]] static SceneResult<std::unique_ptr<SceneSession>> openEditing(SceneEditInput &) noexcept;
        // Failure retains supplied source ownership; success transfers at final commit, before async start.
        ~SceneSession() noexcept override;
        SceneSession(const SceneSession &) = delete;
        SceneSession &operator=(const SceneSession &) = delete;
        SceneSession(SceneSession &&) = delete;
        SceneSession &operator=(SceneSession &&) = delete;
        // Non-result queries (including selection/historyId) require the owning thread.
        // A returned owning snapshot may outlive this Session; a live Session is never a worker input.
        [[nodiscard]] SessionId id() const noexcept;
        [[nodiscard]] ESessionState state() const noexcept;
        [[nodiscard]] ESceneAccess access() const noexcept;
        [[nodiscard]] ChangeStamp stamp() const noexcept;
        [[nodiscard]] SceneResult<void> updateAtOwnerSafePoint(const SceneOwnerUpdate &) noexcept;
        [[nodiscard]] SceneResult<void> advanceScene(const SceneOwnerUpdate &) noexcept;
        [[nodiscard]] SceneResult<SceneOutlineRef> readOutline() const noexcept;
        [[nodiscard]] SceneResult<SceneReadData> readEntity(SceneEntityRef) const noexcept;
        // Bind only existing supported generated component readers. No arbitrary raw object copying.
        template <class T> [[nodiscard]] SceneResult<T> readComponent(SceneEntityRef) const noexcept;
        [[nodiscard]] SceneResult<void> select(std::optional<SceneEntityRef>) noexcept;
        [[nodiscard]] SceneSelectionNotice selection() const noexcept;
        [[nodiscard]] SceneResult<std::shared_ptr<const SceneResourceSnapshot>> readResources() const noexcept;
        // Available during CLOSING/CLOSED. Does not pump replies or publish business snapshots.
        [[nodiscard]] SceneResult<std::shared_ptr<const SceneCloseSnapshot>> closeStatus() const noexcept;
        [[nodiscard]] SceneResult<void> retryResources(const ResourceRequestKey &) noexcept;
        // Owning display value from the current mount snapshot; no VFS or provider escapes to a Pane.
        [[nodiscard]] SceneResult<std::optional<std::string>> readAssetPath(lux::asset::AssetId) const noexcept;
        // EDIT_CONTENT only. One active typed property gesture per Session in this release.
        [[nodiscard]] SceneResult<PropertyGesture> beginTransformEdit(SceneObjectRef) noexcept;
        [[nodiscard]] SceneResult<void> previewTransform(PropertyGesture,
                                                         const lux::simulation::ecs::Transform3D &) noexcept;
        [[nodiscard]] SceneResult<editing::ApplyResult> commitTransformEdit(PropertyGesture) noexcept;
        [[nodiscard]] SceneResult<void> cancelTransformEdit(PropertyGesture) noexcept;
        [[nodiscard]] SceneResult<PropertyGesture> beginLightEdit(SceneObjectRef) noexcept;
        [[nodiscard]] SceneResult<void> previewLight(PropertyGesture, const lux::simulation::ecs::Light3D &) noexcept;
        [[nodiscard]] SceneResult<editing::ApplyResult> commitLightEdit(PropertyGesture) noexcept;
        [[nodiscard]] SceneResult<void> cancelLightEdit(PropertyGesture) noexcept;
        [[nodiscard]] editing::HistoryId historyId() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetView> historyView() const noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> undo() noexcept override;
        [[nodiscard]] editing::EditResult<editing::HistoryTargetResult> redo() noexcept override;
        [[nodiscard]] bool presentationPending() const noexcept;
        [[nodiscard]] SceneResult<void> beginClose() noexcept;
        [[nodiscard]] SceneResult<ECloseProgress> advanceClose() noexcept;

    private:
        friend struct detail::SceneTestAccess;
        friend class SceneView; // Narrow lifetime/render association; never a public Registry accessor.
        [[nodiscard]] SceneResult<const void *> readComponentValue(SceneEntityRef, lux::cxx::TypeToken) const noexcept;
        struct RenderBinding final
        {
            lux::render::RenderSceneId scene;
            double coordinate_page_size{};
        };
        [[nodiscard]] SceneResult<RenderBinding> attachView(rendering::EditorRenderer &) noexcept;
        void detachView() noexcept;
        struct Impl;
        SceneSession(lux::object::ObjectDispatcherRef, std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::sessions

namespace lux::editor::sessions
{
    template <class T> SceneResult<T> SceneSession::readComponent(SceneEntityRef entity) const noexcept
    {
        auto value = readComponentValue(entity, lux::cxx::typeToken<T>());
        if (!value)
            return lux::cxx::unexpected(value.error());
        // The private type check and owner window finish before copying a supported generated value.
        try
        {
            return *static_cast<const T *>(*value);
        }
        catch (const std::bad_alloc &)
        {
            return lux::cxx::unexpected(SceneFailure{ESceneError::ALLOCATION_FAILURE, id()});
        }
    }
} // namespace lux::editor::sessions
