#pragma once

#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/system/SystemInstanceId.hpp>
#include <lux/engine/ui/ImageElement.hpp>
#include <stop_token>

namespace lux::scene
{
    class SceneRuntime;
}

namespace lux::editor::ui
{
    // The runtime and resources outlive the element. An ID never retains the scene;
    // expired scenes end the binding without exposing an instance pointer.
    class LUX_EDITOR_UI_PUBLIC SceneElement final : public lux::ui::Element
    {
    public:
        using CreateResult = render::RenderResult<std::unique_ptr<SceneElement>>;
        [[nodiscard]] static CreateResult create(
            lux::ui::Pane& parent,
            lux::ui::ElementId id,
            lux::scene::SceneRuntime& runtime,
            lux::scene::SceneInstanceId scene,
            lux::scene::RenderResources& resources,
            lux::system::SystemInstanceId system,
            lux::simulation::ecs::Entity camera,
            lux::scene::ViewConfig config
        ) noexcept;
        [[nodiscard]] static CreateResult create(
            lux::ui::Element& parent,
            lux::ui::ElementId id,
            lux::scene::SceneRuntime& runtime,
            lux::scene::SceneInstanceId scene,
            lux::scene::RenderResources& resources,
            lux::system::SystemInstanceId system,
            lux::simulation::ecs::Entity camera,
            lux::scene::ViewConfig config
        ) noexcept;
        ~SceneElement() noexcept override;

        [[nodiscard]] lux::scene::RenderResourceId view() const noexcept
        {
            return view_;
        }
        [[nodiscard]] lux::simulation::ecs::Entity camera() const noexcept
        {
            return camera_;
        }
        [[nodiscard]] render::RenderResult<void> setCamera(lux::simulation::ecs::Entity) noexcept;
        [[nodiscard]] const lux::ui::ImageElement& image() const noexcept
        {
            return image_;
        }
        [[nodiscard]] const render::RenderResult<void>& result() const noexcept
        {
            return result_;
        }
        [[nodiscard]] lux::scene::ViewObservation observation() const noexcept
        {
            return receipt_.status();
        }
        // Call after capture of any current draw data, never from a draw callback.
        // COMPLETE denotes the actual view receipt, not merely cancellation.
        [[nodiscard]] render::ERenderClose close() noexcept;

    private:
        template <class Parent>
        static CreateResult createImpl(
            Parent&,
            lux::ui::ElementId,
            lux::scene::SceneRuntime&,
            lux::scene::SceneInstanceId,
            lux::scene::RenderResources&,
            lux::system::SystemInstanceId,
            lux::simulation::ecs::Entity,
            lux::scene::ViewConfig
        ) noexcept;
        template <class Parent>
        SceneElement(
            Parent&,
            lux::ui::ElementId,
            lux::scene::SceneRuntime&,
            lux::scene::SceneInstanceId,
            lux::scene::RenderResources&,
            lux::simulation::ecs::Entity
        );
        lux::ui::SizeHint sizeHintContent() noexcept override;
        lux::ui::SizeHint measureContent(float width) noexcept override;
        void arrangeContent() noexcept override;
        void draw() noexcept override;
        void update() noexcept override;
        void collectReceipt() noexcept;
        void clearImage() noexcept;

        lux::scene::SceneRuntime& runtime_;
        lux::scene::SceneInstanceId scene_;
        lux::scene::RenderResources& resources_;
        lux::simulation::ecs::Entity camera_, request_{lux::simulation::ecs::NullEntity};
        std::stop_source stop_;
        lux::scene::RenderResourceId view_, image_resource_;
        lux::scene::RenderViewReceipt receipt_;
        lux::ui::ImageElement image_;
        render::PixelExtent requested_extent_;
        render::RenderResult<void> result_;
    };
} // namespace lux::editor::ui
