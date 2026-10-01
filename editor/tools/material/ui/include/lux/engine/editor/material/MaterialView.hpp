#pragma once
#include <lux/engine/editor/material/MaterialInteraction.hpp>
#include <lux/engine/editor/material/MaterialPreviewStore.hpp>
#include <lux/engine/editor/material/MaterialCompilationService.hpp>
#include <lux/engine/editor/persistence/WriteLane.hpp>
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
        MaterialPreviewStore& preview;
        MaterialCompilationService& compilation;
        persistence::WriteCoordinator& writes;
        persistence::IArtifactStore& artifacts;
        std::string publication_address;
        project::ProjectCatalogModel* assets{};
        system::SystemInstanceId render_system;
    };
    struct MaterialViewState final
    {
        lux::editor::views::CameraPose camera;
        render::PixelExtent extent{400, 400};
    };
    using VMaterialViewFailure = std::variant<
        MaterialEditError,
        VMaterialCompileFailure,
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
        [[nodiscard]] MaterialViewResult<void> beginEdit(std::string);
        [[nodiscard]] MaterialViewResult<void> previewEdit(std::vector<VMaterialEdit>&);
        [[nodiscard]] MaterialViewResult<void> commitEdit();
        [[nodiscard]] MaterialViewResult<void> cancelEdit();
        [[nodiscard]] MaterialViewResult<void> undo();
        [[nodiscard]] MaterialViewResult<void> redo();
        [[nodiscard]] MaterialViewResult<MaterialCompileId> compile();
        [[nodiscard]] MaterialViewResult<persistence::WriteTicket> publish();
        [[nodiscard]] MaterialCompileId compilation() const noexcept;
        [[nodiscard]] MaterialViewResult<void> navigate(const lux::editor::views::CameraMotion&);
        [[nodiscard]] const std::optional<MaterialViewBinding>& binding() const noexcept;
        [[nodiscard]] const MaterialViewState& state() const noexcept;
        [[nodiscard]] const MaterialViewResult<void>& status() const noexcept;
        [[nodiscard]] render::RTextureHandle image() const noexcept;

    private:
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
}
