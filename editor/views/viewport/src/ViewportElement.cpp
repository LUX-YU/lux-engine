#include <lux/engine/editor/views/ViewportElement.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/RenderViewRequest.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace lux::editor::views
{
    void ViewportElement::event(object::EventView& event) noexcept
    {
        const auto* input = event.getIf<lux::ui::VInputEvent>();
        if (!input || !attachedRoot())
            return;
        auto& root = *attachedRoot();
        const auto* focus = std::get_if<lux::ui::WindowFocus>(input);
        const bool cancelled = std::holds_alternative<lux::ui::PointerCancel>(*input) || (focus && !focus->focused);
        if (cancelled)
        {
            navigation_ = pan_ = false;
            root.releasePointer(*this);
            return;
        }
        if (!navigation_enabled_)
            return;
        CameraMotion motion;
        bool moved{};
        if (const auto* button = std::get_if<lux::ui::PointerButton>(input))
        {
            const bool navigation_button =
                button->button == lux::ui::EPointerButton::RIGHT || button->button == lux::ui::EPointerButton::MIDDLE;
            if (!navigation_button)
                return;
            if (button->down && image_.interaction().hovered && root.capturePointer(*this))
            {
                navigation_ = true;
                pan_ = button->button == lux::ui::EPointerButton::MIDDLE;
                event.accept();
            }
            else if (!button->down && navigation_)
            {
                navigation_ = pan_ = false;
                root.releasePointer(*this);
                event.accept();
            }
        }
        else if (const auto* move = std::get_if<lux::ui::PointerMove>(input))
        {
            if (navigation_)
            {
                const Eigen::Vector2d delta{move->position.x - pointer_.x, move->position.y - pointer_.y};
                if (pan_)
                    motion.pan_delta = Eigen::Vector2d{-delta.x(), delta.y()} * .01;
                else
                    motion.angular_delta = Eigen::Vector2d{delta.x(), -delta.y()} * .005;
                moved = true;
                event.accept();
            }
            pointer_ = move->position;
        }
        else if (const auto* wheel = std::get_if<lux::ui::PointerWheel>(input); wheel && image_.interaction().hovered)
        {
            motion.dolly = wheel->delta.y;
            moved = true;
            event.accept();
        }
        if (moved)
            navigation_delivery_ = emit(cameraMoved, motion);
    }
    ViewportElement::ViewportElement(lux::ui::Pane& parent, lux::ui::ElementId id)
        : Element(parent, std::move(id)), image_(*this, lux::ui::ElementId{"viewport-image"})
    {}
    ViewportElement::ViewportElement(lux::ui::Element& parent, lux::ui::ElementId id)
        : Element(parent, std::move(id)), image_(*this, lux::ui::ElementId{"viewport-image"})
    {}
    void ViewportElement::setPresentation(
        std::unique_ptr<ViewportPresentation> presentation,
        render::PixelExtent extent
    ) noexcept
    {
        navigation_ = pan_ = false;
        if (attachedRoot())
            attachedRoot()->releasePointer(*this);
        image_.setImage({});
        presentation_ = std::move(presentation);
        requested_extent_ = extent;
    }
    template <class Parent>
    ViewportElement::ViewportElement(
        Parent& parent,
        lux::ui::ElementId id,
        lux::scene::SceneRuntime& runtime,
        lux::scene::SceneInstanceId scene,
        lux::scene::RenderResources& resources,
        lux::simulation::ecs::Entity camera
    )
        : lux::ui::Element(parent, std::move(id)),
          image_(*this, lux::ui::ElementId{std::string(this->id().name()) + ".image"})
    {}

    ViewportElement::CreateResult ViewportElement::create(
        lux::ui::Pane& parent,
        lux::ui::ElementId id,
        lux::scene::SceneRuntime& runtime,
        lux::scene::SceneInstanceId scene,
        lux::scene::RenderResources& resources,
        lux::system::SystemInstanceId system,
        lux::simulation::ecs::Entity camera,
        lux::scene::ViewConfig config
    ) noexcept
    {
        return createImpl(parent, std::move(id), runtime, scene, resources, system, camera, config);
    }

    ViewportElement::CreateResult ViewportElement::create(
        lux::ui::Element& parent,
        lux::ui::ElementId id,
        lux::scene::SceneRuntime& runtime,
        lux::scene::SceneInstanceId scene,
        lux::scene::RenderResources& resources,
        lux::system::SystemInstanceId system,
        lux::simulation::ecs::Entity camera,
        lux::scene::ViewConfig config
    ) noexcept
    {
        return createImpl(parent, std::move(id), runtime, scene, resources, system, camera, config);
    }

    template <class Parent>
    ViewportElement::CreateResult ViewportElement::createImpl(
        Parent& parent,
        lux::ui::ElementId id,
        lux::scene::SceneRuntime& runtime,
        lux::scene::SceneInstanceId scene,
        lux::scene::RenderResources& resources,
        lux::system::SystemInstanceId system,
        lux::simulation::ecs::Entity camera,
        lux::scene::ViewConfig config
    ) noexcept
    {
        const bool is_sampled = std::holds_alternative<lux::scene::SampledOutput>(config.output);
        const bool is_invalid_id = !id.isValid() || !system.valid();
        const bool is_wrong_thread = !parent.isOnAffinityThread();
        const bool is_invalid_extent = config.extent.width > 16384 || config.extent.height > 16384;
        const bool is_invalid_input = !is_sampled || is_invalid_id || is_wrong_thread || is_invalid_extent;
        if (is_invalid_input)
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        auto presentation = ViewportPresentation::create(runtime, scene, resources, system, camera, config);
        if (!presentation)
            return lux::cxx::unexpected(presentation.error());
        auto element = std::unique_ptr<ViewportElement>(
            new ViewportElement(parent, std::move(id), runtime, scene, resources, camera)
        );
        element->presentation_ = std::move(*presentation);
        element->requested_extent_ = config.extent;
        return element;
    }
    ViewportElement::~ViewportElement() noexcept
    {
        static_cast<void>(close());
    }
    render::RenderResult<void> ViewportElement::setCamera(simulation::ecs::Entity camera) noexcept
    {
        if (!presentation_)
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        return presentation_->setCamera(camera);
    }
    render::ERenderClose ViewportElement::close() noexcept
    {
        image_.setImage({});
        setVisible(false);
        return presentation_ ? presentation_->close() : render::ERenderClose::COMPLETE;
    }
    void ViewportElement::update() noexcept
    {
        if (!presentation_)
            return;
        presentation_->update(displayed() ? requested_extent_ : render::PixelExtent{});
        image_.setImage(presentation_->image());
    }

    lux::ui::SizeHint ViewportElement::sizeHintContent() noexcept
    {
        return image_.sizeHint();
    }
    lux::ui::SizeHint ViewportElement::measureContent(float width) noexcept
    {
        return image_.measure(width);
    }
    void ViewportElement::arrangeContent() noexcept
    {
        image_.arrange({{}, rect().size});
    }

    void ViewportElement::draw() noexcept
    {
        if (!presentation_ || !presentation_->active())
            return;
        drawChild(image_);
        const auto& interaction = image_.interaction();
        if (interaction.left_clicked)
            navigation_delivery_ = emit(clicked, ViewportPoint{interaction.local_pointer, interaction.size});
        const auto size = image_.displayedSize();
        const auto scale = ImGui::GetIO().DisplayFramebufferScale;
        const auto pixels = [](float logical, float scale) {
            return static_cast<std::uint32_t>(std::clamp(std::round(logical * scale), 0.F, 16384.F));
        };
        requested_extent_ = {pixels(size.width, scale.x), pixels(size.height, scale.y)};
    }
} // namespace lux::editor::views
