#pragma once
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/desktop/UiError.hpp>
#include <lux/engine/editor/material/MaterialCompilationService.hpp>
#include <lux/engine/editor/material/MaterialInteraction.hpp>
#include <lux/engine/editor/material/MaterialPreview.hpp>
#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <lux/engine/editor/views/CameraNavigation.hpp>
#include <lux/engine/editor/views/ViewInfo.hpp>
#include <lux/engine/editor/workspace/WorkspaceValues.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::project
{
    class ProjectCatalogModel;
}
namespace lux::editor::persistence
{
    class WriteCoordinator;
    class IArtifactStore;
} // namespace lux::editor::persistence

namespace lux::editor::desktop
{
    struct UiDescriptor;
}

namespace lux::editor::material
{
    extern const desktop::UiDescriptor kMaterialView;
    struct MaterialViewBinding final
    {
        sessions::TSessionKey<MaterialSession> session;
        MaterialInteraction* interaction{};
        friend bool operator==(MaterialViewBinding, MaterialViewBinding) = default;
    };
    struct MaterialViewServices final
    {
        sessions::TSessionAccess<MaterialSession> sessions;
        lux::scene::SceneRuntime& runtime;
        lux::scene::RenderResources& resources;
        render::RenderRuntime& renderer;
        MaterialPreview& preview;
        std::shared_ptr<MaterialCompilationService> compilation;
        const scene::ProjectionEnvironment& environment;
        project::ProjectCatalogModel* assets{};
        system::SystemInstanceId render_system;
        std::shared_ptr<persistence::IArtifactSubmission> publication;
    };
    struct MaterialViewState final
    {
        lux::editor::views::ViewportCameraState camera;
        render::PixelExtent extent{400, 400};
    };
    using VMaterialViewFailure = std::variant<
        MaterialEditError,
        VMaterialCompileFailure,
        MaterialPreviewFailure,
        persistence::PersistenceFailure,
        scene::ProjectionFailure,
        render::RendererFailure,
        desktop::EUiError,
        lux::ui::EAttachmentError,
        std::string_view>;
    template <class T> using MaterialViewResult = cxx::expected<T, VMaterialViewFailure>;
    class MaterialView final : public lux::ui::Pane
    {
    public:
        [[nodiscard]] static MaterialViewResult<std::unique_ptr<MaterialView>> create(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            MaterialViewServices,
            std::optional<MaterialViewBinding> = {},
            MaterialViewState = {}
        );
        [[nodiscard]] static MaterialViewResult<std::unique_ptr<MaterialView>> createContent(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            sessions::TSessionAccess<MaterialSession>,
            lux::scene::SceneRuntime&,
            std::shared_ptr<MaterialCompilationService>,
            const scene::ProjectionEnvironment&,
            std::span<const render::RenderFeatureRegistration>,
            project::ProjectCatalogModel*,
            const views::ViewContent&,
            std::shared_ptr<persistence::IArtifactSubmission>,
            std::optional<MaterialViewState> = {}
        );
        ~MaterialView() noexcept override;
        MaterialView(const MaterialView&) = delete;
        MaterialView& operator=(const MaterialView&) = delete;
        MaterialView(MaterialView&&) = delete;
        MaterialView& operator=(MaterialView&&) = delete;
        [[nodiscard]] MaterialViewResult<void> rebind(std::optional<MaterialViewBinding>);
        [[nodiscard]] MaterialViewResult<void> rebindContent(const views::ViewContent&);
        [[nodiscard]] MaterialViewResult<void> beginEdit(std::string);
        [[nodiscard]] MaterialViewResult<void> previewEdit(std::vector<VMaterialEdit>&);
        [[nodiscard]] MaterialViewResult<void> commitEdit();
        [[nodiscard]] MaterialViewResult<void> cancelEdit();
        [[nodiscard]] MaterialViewResult<void> undo();
        [[nodiscard]] MaterialViewResult<void> redo();
        [[nodiscard]] MaterialViewResult<MaterialCompileId> compile();
        // Returns the publication owner's admission identity, not a file commit or signal delivery.
        [[nodiscard]] MaterialViewResult<std::uint64_t> requestPublication();
        [[nodiscard]] MaterialCompileId compilation() const noexcept;
        [[nodiscard]] MaterialViewResult<void> navigate(const lux::editor::views::CameraMotion&);
        [[nodiscard]] const std::optional<MaterialViewBinding>& binding() const noexcept;
        [[nodiscard]] const MaterialViewState& state() const noexcept;
        [[nodiscard]] desktop::UiResult<workspace::VersionedViewState> captureState() const;
        [[nodiscard]] desktop::UiResult<cxx::move_only_function<void()>>
        prepareState(std::uint32_t, std::span<const std::byte>);
        [[nodiscard]] const MaterialViewResult<void>& status() const noexcept;
        [[nodiscard]] render::RTextureHandle image() const noexcept;
        [[nodiscard]] MaterialPreviewStatus previewStatus() const;

    private:
        MaterialView(object::ObjectDispatcherRef, lux::ui::PaneId, MaterialViewServices, MaterialViewState);
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::editor::material
