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
    namespace
    {
        bool validCamera(const lux::simulation::ecs::Registry& registry, lux::simulation::ecs::Entity camera) noexcept
        {
            return registry.valid(camera) && registry.all_of<lux::scene::Camera>(camera);
        }
    }

    template <class Parent>
    SceneElement::SceneElement(
        Parent& parent,
        lux::ui::ElementId id,
        lux::scene::SceneRuntime& runtime,
        lux::scene::SceneInstanceId scene,
        lux::scene::RenderResources& resources,
        lux::simulation::ecs::Entity camera
    )
        : lux::ui::Element(parent, std::move(id)), runtime_(runtime), scene_(scene), resources_(resources),
          camera_(camera), image_(*this, lux::ui::ElementId{std::string(this->id().name()) + ".image"})
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
        const auto borrowed = runtime.getSceneRegistry(scene);
        if (!borrowed)
        {
            const auto* cause = std::get_if<lux::scene::ESceneRuntimeError>(&borrowed.error().cause);
            const bool is_busy = cause && *cause == lux::scene::ESceneRuntimeError::BUSY;
            return lux::cxx::unexpected(render::RendererFailure{
                is_busy ? render::ERendererError::BUSY : render::ERendererError::INVALID_ARGUMENT
            });
        }
        auto& registry = borrowed->get();
        const bool has_render_system = lux::scene::RenderAssets::find(registry, system) != nullptr;
        const bool has_camera = validCamera(registry, camera);
        const bool is_missing_target = !has_render_system || !has_camera;
        if (is_missing_target)
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        auto element =
            std::unique_ptr<SceneElement>(new SceneElement(parent, std::move(id), runtime, scene, resources, camera));
        element->requested_extent_ = config.extent;
        element->request_ = registry.create();
        registry.emplace<lux::scene::RenderViewRequest>(
            element->request_,
            lux::scene::RenderViewRequest{
                .system = system,
                .camera = camera,
                .configuration = config,
                .stop = element->stop_.get_token(),
                .destroy_entity_on_stop = true
            }
        );
        return element;
    }

    SceneElement::~SceneElement() noexcept
    {
        static_cast<void>(close());
    }

    render::RenderResult<void> SceneElement::setCamera(lux::simulation::ecs::Entity camera) noexcept
    {
        if (!scene_.valid())
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        const auto borrowed = runtime_.getSceneRegistry(scene_);
        if (!borrowed)
        {
            const auto* cause = std::get_if<lux::scene::ESceneRuntimeError>(&borrowed.error().cause);
            const bool is_busy = cause && *cause == lux::scene::ESceneRuntimeError::BUSY;
            return lux::cxx::unexpected(render::RendererFailure{
                is_busy ? render::ERendererError::BUSY : render::ERendererError::INVALID_ARGUMENT
            });
        }
        auto& registry = borrowed->get();
        const bool has_camera = validCamera(registry, camera);
        const bool has_request = registry.all_of<lux::scene::RenderViewRequest>(request_);
        if (!has_camera || !has_request)
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        if (camera == camera_)
            return {};
        registry.patch<lux::scene::RenderViewRequest>(request_, [&](auto& request) {
            request.camera = camera;
            ++request.revision;
        });
        camera_ = camera;
        return {};
    }

    void SceneElement::collectReceipt() noexcept
    {
        const bool needs_receipt = scene_.valid() && !view_.isValid();
        if (!needs_receipt)
            return;
        const auto borrowed = std::as_const(runtime_).getSceneRegistry(scene_);
        if (!borrowed)
            return;
        const auto* result = borrowed->get().try_get<lux::scene::RenderViewResult>(request_);
        const bool has_adopted_view = result && result->view.isValid();
        if (!has_adopted_view)
            return;
        auto receipt = resources_.viewReceipt(result->view);
        if (!receipt)
        {
            result_ = lux::cxx::unexpected(receipt.error());
            return;
        }
        receipt_ = std::move(*receipt);
        view_ = result->view; // The request owns the business reference.
    }

    void SceneElement::clearImage() noexcept
    {
        image_.setImage({});
        if (image_resource_.isValid())
            resources_.release(std::exchange(image_resource_, {}));
    }

    render::ERenderClose SceneElement::close() noexcept
    {
        collectReceipt();
        clearImage();
        stop_.request_stop();
        scene_ = {}; // Later receipt checks and destruction cannot touch the old scene.
        setVisible(false);
        const bool is_closed = !view_.isValid() || receipt_.status().status.state == lux::scene::EViewState::CLOSED;
        return is_closed ? render::ERenderClose::COMPLETE : render::ERenderClose::PENDING;
    }

    void SceneElement::update() noexcept
    {
        if (!scene_.valid())
            return;
        collectReceipt();
        const auto borrowed = runtime_.getSceneRegistry(scene_);
        if (!borrowed)
        {
            if (!std::as_const(runtime_).getSceneRegistry(scene_))
                static_cast<void>(close());
            return;
        }
        auto& registry = borrowed->get();
        if (!registry.all_of<lux::scene::RenderViewRequest>(request_))
        {
            static_cast<void>(close());
            return;
        }
        auto& request = registry.get<lux::scene::RenderViewRequest>(request_);
        const auto extent = displayed() ? requested_extent_ : render::PixelExtent{};
        if (request.configuration.extent != extent)
            registry.patch<lux::scene::RenderViewRequest>(request_, [&](auto& value) {
                value.configuration.extent = extent;
                ++value.revision;
            });
        const auto* adopted = registry.try_get<lux::scene::RenderViewResult>(request_);
        if (!adopted)
            return;
        if (adopted->failure)
        {
            result_ = lux::cxx::unexpected(*adopted->failure);
            return;
        }
        const bool is_current_request = adopted->adopted_revision == request.revision &&
                                        adopted->published_revision == request.revision &&
                                        adopted->published_sequence != 0;
        if (!is_current_request)
            return;
        const auto observation = receipt_.status();
        if (observation.status.failure)
        {
            result_ = lux::cxx::unexpected(*observation.status.failure);
            return;
        }
        if (observation.render_sequence != adopted->published_sequence)
            return;
        const auto output = resources_.viewOutput(view_);
        if (!output)
        {
            if (output.error().code != render::ERendererError::NOT_READY)
                result_ = lux::cxx::unexpected(output.error());
            return;
        }
        const auto info = resources_.outputInfo(*output);
        if (!info)
        {
            result_ = lux::cxx::unexpected(info.error());
            return;
        }
        const auto& stamp = info->content;
        const bool is_current_output = stamp.source.surface_generation == adopted->published_sequence &&
                                       stamp.source.view_revision == adopted->published_revision &&
                                       stamp.source.session == scene_.domain;
        const bool has_producer = stamp.frame_serial != 0 && stamp.evidence >= lux::scene::EImageEvidence::RECORDED;
        const bool is_sampleable = is_current_output && has_producer;
        if (!is_sampleable)
            return;
        if (*output == image_resource_)
        {
            result_ = {};
            return;
        }
        result_ = resources_.retain(*output);
        if (!result_)
            return;
        image_.setImage(info->texture);
        const auto old = std::exchange(image_resource_, *output);
        if (old.isValid())
            resources_.release(old);
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
        if (!scene_.valid())
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
