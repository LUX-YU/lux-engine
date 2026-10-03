#pragma once
#include <lux/engine/editor/material/MaterialInteraction.hpp>
#include <lux/engine/editor/material/MaterialPreview.hpp>
#include <lux/engine/editor/material/MaterialCompilationService.hpp>
#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <lux/engine/editor/views/CameraNavigation.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>

namespace lux::editor::project
{
    class ProjectCatalogModel;
}
namespace lux::editor::persistence
{
    class WriteCoordinator;
    class IArtifactStore;
}

namespace lux::editor::views { class ViewFactoryEntry; }

namespace lux::editor::material
{
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
        MaterialCompilationService& compilation;
        const scene::ProjectionEnvironment& environment;
        project::ProjectCatalogModel* assets{};
        system::SystemInstanceId render_system;
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
        views::EViewError,
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
        // This is a user intention. Admission and publication results belong to its explicit receiver.
        object::TSignal<persistence::DerivedArtifact> publishRequested{*this};
        [[nodiscard]] MaterialViewResult<void> requestPublication();
        [[nodiscard]] MaterialCompileId compilation() const noexcept;
        [[nodiscard]] MaterialViewResult<void> navigate(const lux::editor::views::CameraMotion&);
        [[nodiscard]] const std::optional<MaterialViewBinding>& binding() const noexcept;
        [[nodiscard]] const MaterialViewState& state() const noexcept;
        [[nodiscard]] views::ViewCaptureResult captureState() const;
        [[nodiscard]] views::ViewStateResult prepareState(std::uint32_t, std::span<const std::byte>);
        [[nodiscard]] const MaterialViewResult<void>& status() const noexcept;
        [[nodiscard]] render::RTextureHandle image() const noexcept;
        [[nodiscard]] MaterialPreviewStatus previewStatus() const;

    private:
        friend MaterialViewResult<views::DetachedView> makeMaterialContentView(
        object::ObjectDispatcherRef,
        lux::ui::PaneId,
        sessions::TSessionAccess<MaterialSession>,
        lux::scene::SceneRuntime&,
        MaterialCompilationService&,
        const scene::ProjectionEnvironment&,
        std::span<const render::RenderFeatureRegistration>,
        project::ProjectCatalogModel*,
        const views::ViewContent&
        );
        MaterialView(object::ObjectDispatcherRef, lux::ui::PaneId, MaterialViewServices, MaterialViewState);
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] MaterialViewResult<views::DetachedView> makeMaterialView(
        object::ObjectDispatcherRef,
        lux::ui::PaneId,
        MaterialViewServices,
        std::optional<MaterialViewBinding> = {},
        MaterialViewState = {}
    );
    // A complete content view owns its interaction and preview target. Low-level makeMaterialView
    // remains available for explicitly borrowed tools using an independently owned preview target.
    [[nodiscard]] MaterialViewResult<views::DetachedView> makeMaterialContentView(
        object::ObjectDispatcherRef,
        lux::ui::PaneId,
        sessions::TSessionAccess<MaterialSession>,
        lux::scene::SceneRuntime&,
        MaterialCompilationService&,
        const scene::ProjectionEnvironment&,
        std::span<const render::RenderFeatureRegistration>,
        project::ProjectCatalogModel*,
        const views::ViewContent&
    );

    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> makeMaterialViewFactory(
        sessions::TSessionAccess<material::MaterialSession> sessions,
        lux::scene::SceneRuntime& runtime,
        material::MaterialCompilationService& compilation,
        const scene::ProjectionEnvironment& environment,
        std::span<const render::RenderFeatureRegistration> features,
        project::ProjectCatalogModel* assets,
        cxx::move_only_function<void(const persistence::DerivedArtifact&)> = {}
    );
}
