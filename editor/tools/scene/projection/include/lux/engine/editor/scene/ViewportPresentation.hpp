#pragma once
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/system/SystemInstanceId.hpp>
#include <stop_token>

namespace lux::scene
{
    class SceneRuntime;
}
namespace lux::editor::scene
{
    enum class EOverlaySubmit : std::uint8_t
    {
        UNCHANGED,
        NOT_READY,
        BACKPRESSURED,
        ACCEPTED,
        REJECTED
    };
    struct OverlayConfiguration final
    {
        simulation::ecs::Entity selection{simulation::ecs::NullEntity};
        std::uint64_t selection_version{}, structure_version{};
        float plane_height{};
    };
    struct HighlightRenderer;
    // Derived view resources only. Never owns or advances the bound scene.
    class ViewportPresentation final
    {
    public:
        using CreateResult = render::RenderResult<std::unique_ptr<ViewportPresentation>>;
        [[nodiscard]] static CreateResult create(
            lux::scene::SceneRuntime&,
            lux::scene::SceneInstanceId,
            lux::scene::RenderResources&,
            lux::system::SystemInstanceId,
            simulation::ecs::Entity,
            lux::scene::ViewConfig
        ) noexcept;
        ~ViewportPresentation() noexcept;
        [[nodiscard]] lux::scene::RenderResourceId view() const noexcept
        {
            return view_;
        }
        [[nodiscard]] simulation::ecs::Entity camera() const noexcept
        {
            return camera_;
        }
        [[nodiscard]] render::RTextureHandle image() const noexcept
        {
            return image_;
        }
        [[nodiscard]] bool active() const noexcept
        {
            return scene_.valid();
        }
        [[nodiscard]] const render::RenderResult<void>& result() const noexcept
        {
            return result_;
        }
        [[nodiscard]] lux::scene::ViewObservation observation() const noexcept
        {
            return receipt_.status();
        }
        [[nodiscard]] render::RenderResult<void> setCamera(simulation::ecs::Entity) noexcept;
        [[nodiscard]] render::ERenderClose close() noexcept;
        void update(render::PixelExtent) noexcept;
        [[nodiscard]] render::RenderResult<EOverlaySubmit> updateOverlay(render::RenderRuntime&, OverlayConfiguration);
        void retryOverlay() noexcept;

    private:
        ViewportPresentation(
            lux::scene::SceneRuntime&,
            lux::scene::SceneInstanceId,
            lux::scene::RenderResources&,
            simulation::ecs::Entity
        );
        void collectReceipt() noexcept;
        void clearImage() noexcept;
        std::unique_ptr<HighlightRenderer> highlight_;
        lux::system::SystemInstanceId system_;
        lux::scene::SceneRuntime& runtime_;
        lux::scene::SceneInstanceId scene_;
        lux::scene::RenderResources& resources_;
        simulation::ecs::Entity camera_, request_{simulation::ecs::NullEntity};
        std::stop_source stop_;
        lux::scene::RenderResourceId view_, image_resource_;
        lux::scene::RenderViewReceipt receipt_;
        render::RTextureHandle image_;
        render::RenderResult<void> result_;
    };
}
