#pragma once
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/desktop/UiError.hpp>
#include <lux/engine/editor/scene/ModelPlacement.hpp>
#include <lux/engine/editor/scene/SceneInteraction.hpp>
#include <lux/engine/editor/scene/SceneProjection.hpp>
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/views/CameraNavigation.hpp>
#include <lux/engine/editor/views/ViewInfo.hpp>
#include <lux/engine/editor/workspace/WorkspaceValues.hpp>
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::desktop
{
    struct UiDescriptor;
    struct UiFailure;
    struct UiCreateInfo;
} // namespace lux::editor::desktop

namespace lux::services
{
    class ServiceResolver;
}
namespace lux::editor::scene
{
    extern const desktop::UiDescriptor kSceneView;
    struct UnboundSceneBinding final
    {
        friend bool operator==(UnboundSceneBinding, UnboundSceneBinding) = default;
    };
    struct EditedSceneBinding final
    {
        sessions::TSessionKey<SceneSession> session;
        SceneInteractionGroup* interaction{}; // Explicit external group owner; closing a view does not delete it.
        friend bool operator==(EditedSceneBinding, EditedSceneBinding) = default;
    };
    struct RunningSceneBinding final
    {
        RunId run;
        SceneInteractionGroup* interaction{};
        friend bool operator==(RunningSceneBinding, RunningSceneBinding) = default;
    };
    using VSceneViewBinding = std::variant<UnboundSceneBinding, EditedSceneBinding, RunningSceneBinding>;
    struct SceneViewState final
    {
        lux::editor::views::ViewportCameraState camera;
        render::PixelExtent extent{640, 480};
        float work_plane_height{};
    };
    struct SceneViewServices final
    {
        sessions::TSessionAccess<SceneSession> sessions;
        std::shared_ptr<ScenePresentationHub> projections;
        lux::scene::SceneRuntime& runtime;
        lux::scene::RenderResources& resources;
        render::RenderRuntime& renderer;
        const ProjectionEnvironment& environment;
        std::optional<RunInspectAccess> runs;
    };
    struct SceneViewFailure final
    {
        using VCause = std::variant<
            SceneEditError,
            ProjectionFailure,
            RunFailure,
            lux::scene::MeshQueryFailure,
            render::RendererFailure,
            desktop::EUiError,
            lux::ui::EAttachmentError,
            std::string_view>;
        VCause cause;
    };
    template <class T> using SceneViewResult = cxx::expected<T, SceneViewFailure>;
    struct SceneViewCreateInfo final
    {
        lux::ui::PaneId id;
        std::string title{"Scene"};
        VSceneViewBinding binding{UnboundSceneBinding{}};
        SceneViewState state;
        system::SystemInstanceId render_system;
    };

    class SceneView final : public lux::ui::Pane
    {
    public:
        using ModelDrop = cxx::move_only_function<void(const ModelPlacement&)>;
        [[nodiscard]] static cxx::expected<std::unique_ptr<lux::ui::Pane>, desktop::UiFailure>
        createConfigured(services::ServiceResolver&, const desktop::UiCreateInfo&);
        // UI intent only. The receiver owns admission, transport and the eventual domain result.
        object::TSignal<ModelPlacement> modelDropped{*this};
        [[nodiscard]] static SceneViewResult<std::unique_ptr<SceneView>> create(
            object::ObjectDispatcherRef,
            SceneViewServices,
            SceneViewCreateInfo
        );
        ~SceneView() noexcept override;
        SceneView(const SceneView&) = delete;
        SceneView& operator=(const SceneView&) = delete;
        SceneView(SceneView&&) = delete;
        SceneView& operator=(SceneView&&) = delete;
        [[nodiscard]] SceneViewResult<void> rebind(VSceneViewBinding);
        [[nodiscard]] SceneViewResult<void> rebindRun(
            RunId,
            std::optional<system::SystemInstanceId> render_system = {}
        );
        [[nodiscard]] SceneViewResult<void> rebindContent(const views::ViewContent&);
        [[nodiscard]] const std::shared_ptr<SceneInteractionGroup>& interactionOwner() const noexcept;
        [[nodiscard]] const VSceneViewBinding& binding() const noexcept;
        [[nodiscard]] const SceneViewState& state() const noexcept;
        [[nodiscard]] desktop::UiResult<workspace::VersionedViewState> captureState() const;
        [[nodiscard]] desktop::UiResult<cxx::move_only_function<void()>>
        prepareState(std::uint32_t, std::span<const std::byte>);
        [[nodiscard]] SceneViewResult<void> navigate(const lux::editor::views::CameraMotion&);
        [[nodiscard]] SceneViewResult<void> pick(Eigen::Vector2d position, Eigen::Vector2d extent);
        [[nodiscard]] SceneViewResult<void> dropModel(AssetReference, Eigen::Vector2d position, Eigen::Vector2d extent);
        [[nodiscard]] SceneViewResult<void> undo();
        [[nodiscard]] SceneViewResult<void> redo();
        [[nodiscard]] SceneViewResult<void> beginEdit(std::string);
        [[nodiscard]] SceneViewResult<void> previewEdit(std::vector<VSceneEdit>&);
        [[nodiscard]] SceneViewResult<void> commitEdit();
        [[nodiscard]] SceneViewResult<void> cancelEdit();
        [[nodiscard]] std::optional<sessions::ContentStamp> projectedContent() const noexcept;
        [[nodiscard]] lux::scene::SceneInstanceId presentedInstance() const noexcept;
        [[nodiscard]] lux::scene::RenderResourceId viewport() const noexcept;
        [[nodiscard]] render::RTextureHandle image() const noexcept;
        [[nodiscard]] system::SystemInstanceId renderSystem() const noexcept;
        [[nodiscard]] const SceneViewResult<void>& status() const noexcept;

    private:
        SceneView(object::ObjectDispatcherRef, SceneViewServices, SceneViewCreateInfo);
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
        object::Connection model_connection_;
    };
} // namespace lux::editor::scene
