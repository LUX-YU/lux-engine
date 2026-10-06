#pragma once

#include <lux/engine/editor/views/ViewportPresentation.hpp>
#include <lux/engine/editor/views/CameraNavigation.hpp>
#include <lux/engine/system/SystemInstanceId.hpp>
#include <lux/engine/ui/ImageElement.hpp>
#include <stop_token>

namespace lux::scene
{
    class SceneRuntime;
}

namespace lux::editor::views
{
    struct ViewportPoint final
    {
        lux::ui::Point position;
        lux::ui::Size extent;
    };
    // The runtime and resources outlive the element. An ID never retains the scene;
    // expired scenes end the binding without exposing an instance pointer.
    class ViewportElement final : public lux::ui::Element
    {
    public:
        object::TSignal<CameraMotion> cameraMoved{*this};
        object::TSignal<ViewportPoint> clicked{*this};
        void enableNavigation(bool enabled) noexcept
        {
            navigation_enabled_ = enabled;
        }
        [[nodiscard]] object::SignalDelivery navigationDelivery() const noexcept
        {
            return navigation_delivery_;
        }
        ViewportElement(lux::ui::Pane&, lux::ui::ElementId);
        ViewportElement(lux::ui::Element&, lux::ui::ElementId);
        ViewportElement(object::ObjectDispatcherRef, lux::ui::ElementId);
        ViewportElement(const ViewportElement&) = delete;
        ViewportElement& operator=(const ViewportElement&) = delete;
        ViewportElement(ViewportElement&&) = delete;
        ViewportElement& operator=(ViewportElement&&) = delete;
        // Cold/safe-point swap of a fully prepared presentation; old retirement remains with RenderResources.
        void setPresentation(std::unique_ptr<ViewportPresentation>, render::PixelExtent) noexcept;
        [[nodiscard]] bool bound() const noexcept
        {
            return bool(presentation_);
        }
        using CreateResult = render::RenderResult<std::unique_ptr<ViewportElement>>;
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
        ~ViewportElement() noexcept override;
        [[nodiscard]] ViewportPresentation& presentation() noexcept
        {
            return *presentation_;
        }

        [[nodiscard]] lux::scene::RenderResourceId view() const noexcept
        {
            return presentation_ ? presentation_->view() : lux::scene::RenderResourceId{};
        }
        [[nodiscard]] lux::simulation::ecs::Entity camera() const noexcept
        {
            return presentation_ ? presentation_->camera() : simulation::ecs::NullEntity;
        }
        [[nodiscard]] render::RenderResult<void> setCamera(lux::simulation::ecs::Entity) noexcept;
        [[nodiscard]] const lux::ui::ImageElement& image() const noexcept
        {
            return image_;
        }
        [[nodiscard]] const render::RenderResult<void>& result() const noexcept
        {
            return presentation_->result();
        }
        [[nodiscard]] lux::scene::ViewObservation observation() const noexcept
        {
            return presentation_ ? presentation_->observation() : lux::scene::ViewObservation{};
        }
        // Call after capture of any current draw data, never from a draw callback.
        // COMPLETE denotes the actual view receipt, not merely cancellation.
        [[nodiscard]] render::ERenderClose close() noexcept;

    private:
        void event(object::EventView&) noexcept override;
        lux::ui::Point pointer_;
        bool navigation_enabled_{}, navigation_{}, pan_{};
        object::SignalDelivery navigation_delivery_;
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
        lux::ui::SizeHint sizeHintContent() noexcept override;
        lux::ui::SizeHint measureContent(float width) noexcept override;
        void arrangeContent() noexcept override;
        void draw() noexcept override;
        void update() noexcept override;

        std::unique_ptr<ViewportPresentation> presentation_;
        lux::ui::ImageElement image_;
        render::PixelExtent requested_extent_;
    };
} // namespace lux::editor::views
