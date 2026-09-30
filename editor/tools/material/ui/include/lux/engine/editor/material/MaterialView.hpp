#pragma once
#include <lux/engine/editor/material/MaterialInteraction.hpp>
#include <lux/engine/editor/material/MaterialPreviewStore.hpp>
#include <lux/engine/editor/scene/SceneElement.hpp>
#include <lux/engine/editor/scene/CameraNavigation.hpp>
#include <lux/engine/editor/project/ProjectCatalogAccess.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>

namespace lux::editor::material
{
    struct MaterialViewBinding final
    {
        sessions::TSessionKey<MaterialSession> session;
        MaterialInteraction* interaction{};
        friend bool operator==(MaterialViewBinding, MaterialViewBinding) = default;
    };
    enum class EMaterialViewAction : std::uint8_t
    {
        COMPILE,
        PUBLISH
    };
    struct MaterialViewRequests final
    {
        void* owner{};
        MaterialCompileResult<void> (*request)(void*, sessions::TSessionKey<MaterialSession>, EMaterialViewAction){};
    };
    struct MaterialViewServices final
    {
        sessions::TSessionAccess<MaterialSession> sessions;
        lux::scene::SceneRuntime& runtime;
        lux::scene::RenderResources& resources;
        render::RenderRuntime& renderer;
        MaterialPreviewStore& preview;
        MaterialViewRequests requests;
        project::ProjectCatalogAccess assets;
        system::SystemInstanceId render_system;
    };
    struct MaterialViewState final
    {
        scene::CameraPose camera;
        render::PixelExtent extent{400, 400};
    };
    using VMaterialViewFailure = std::variant<
        MaterialEditError,
        VMaterialCompileFailure,
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
        [[nodiscard]] MaterialViewResult<void> request(EMaterialViewAction);
        [[nodiscard]] MaterialViewResult<void> navigate(const scene::CameraMotion&);
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
