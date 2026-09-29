#include <lux/engine/editor/ui/SceneElement.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/RenderViewRequest.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace lux::editor::ui
{
    template <class Parent>
    SceneElement::SceneElement(
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

    SceneElement::CreateResult SceneElement::create(
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

    SceneElement::CreateResult SceneElement::create(
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
    SceneElement::CreateResult SceneElement::createImpl(
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
        auto presentation = scene::ViewportPresentation::create(runtime, scene, resources, system, camera, config);
        if (!presentation)
            return lux::cxx::unexpected(presentation.error());
        auto element =
            std::unique_ptr<SceneElement>(new SceneElement(parent, std::move(id), runtime, scene, resources, camera));
        element->presentation_ = std::move(*presentation);
        element->requested_extent_ = config.extent;
        return element;
    }
    SceneElement::~SceneElement() noexcept
    {
        static_cast<void>(close());
    }
    render::RenderResult<void> SceneElement::setCamera(simulation::ecs::Entity camera) noexcept
    {
        return presentation_->setCamera(camera);
    }
    render::ERenderClose SceneElement::close() noexcept
    {
        image_.setImage({});
        setVisible(false);
        return presentation_ ? presentation_->close() : render::ERenderClose::COMPLETE;
    }
    void SceneElement::update() noexcept
    {
        presentation_->update(displayed() ? requested_extent_ : render::PixelExtent{});
        image_.setImage(presentation_->image());
    }

    lux::ui::SizeHint SceneElement::sizeHintContent() noexcept
    {
        return image_.sizeHint();
    }
    lux::ui::SizeHint SceneElement::measureContent(float width) noexcept
    {
        return image_.measure(width);
    }
    void SceneElement::arrangeContent() noexcept
    {
        image_.arrange({{}, rect().size});
    }

    void SceneElement::draw() noexcept
    {
        if (!presentation_->active())
            return;
        drawChild(image_);
        const auto size = image_.displayedSize();
        const auto scale = ImGui::GetIO().DisplayFramebufferScale;
        const auto pixels = [](float logical, float scale) {
            return static_cast<std::uint32_t>(std::clamp(std::round(logical * scale), 0.F, 16384.F));
        };
        requested_extent_ = {pixels(size.width, scale.x), pixels(size.height, scale.y)};
    }
} // namespace lux::editor::ui
