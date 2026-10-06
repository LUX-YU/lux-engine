#include "HighlightRenderer.hpp"
#include <limits>
#include <lux/engine/editor/views/ViewportPresentation.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/RenderViewRequest.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <utility>
namespace lux::editor::views
{
    namespace
    {
        bool validCamera(const simulation::ecs::Registry& registry, simulation::ecs::Entity camera) noexcept
        {
            return registry.valid(camera) && registry.all_of<lux::scene::Camera>(camera);
        }
        bool sameProjection(const lux::scene::Camera& first, const lux::scene::Camera& second) noexcept
        {
            if (first.primary != second.primary)
            {
                return false;
            }
            return std::visit(
                [&](const auto& left)
                {
                    const auto* right = std::get_if<std::decay_t<decltype(left)>>(&second.projection);
                    if (!right)
                    {
                        return false;
                    }
                    const bool same_planes = left.near_plane == right->near_plane && left.far_plane == right->far_plane;
                    if constexpr (requires { left.vertical_fov; })
                    {
                        return same_planes && left.vertical_fov == right->vertical_fov;
                    }
                    else
                    {
                        return same_planes && left.vertical_extent == right->vertical_extent;
                    }
                },
                first.projection
            );
        }
    } // namespace
    ViewportPresentation::ViewportPresentation(
        lux::scene::SceneRuntime& runtime,
        lux::scene::SceneInstanceId scene,
        lux::scene::RenderResources& resources,
        simulation::ecs::Entity camera
    )
        : runtime_(runtime), scene_(scene), resources_(resources), camera_(camera)
    {
    }
    ViewportPresentation::CreateResult ViewportPresentation::create(
        lux::scene::SceneRuntime& runtime,
        lux::scene::SceneInstanceId scene,
        lux::scene::RenderResources& resources,
        lux::system::SystemInstanceId system,
        simulation::ecs::Entity camera,
        lux::scene::ViewConfig config
    ) noexcept
    {
        const bool is_invalid_extent = config.extent.width > 16384 || config.extent.height > 16384;
        if (!system.valid() || is_invalid_extent)
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        const auto borrowed = runtime.borrowInstance(scene);
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
            std::unique_ptr<ViewportPresentation>(new ViewportPresentation(runtime, scene, resources, camera));
        element->system_ = system;
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
    ViewportPresentation::~ViewportPresentation() noexcept
    {
        static_cast<void>(close());
    }

    ViewportPresentation::CreateResult ViewportPresentation::create(
        lux::scene::SceneRuntime& runtime,
        lux::scene::SceneInstanceId scene,
        lux::scene::RenderResources& resources,
        lux::system::SystemInstanceId system,
        const simulation::ecs::Transform3D& transform,
        const lux::scene::Camera& camera,
        lux::scene::ViewConfig config
    ) noexcept
    {
        const bool is_invalid_extent = config.extent.width > 16384 || config.extent.height > 16384;
        const bool is_invalid_transform = !transform.translation.allFinite() ||
                                          !transform.rotation.coeffs().allFinite() || !transform.scale.allFinite();
        if (!system.valid() || is_invalid_extent || is_invalid_transform || !lux::scene::cameraProjection(camera, 1.0))
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        const auto borrowed = runtime.borrowInstance(scene);
        if (!borrowed)
        {
            const auto* cause = std::get_if<lux::scene::ESceneRuntimeError>(&borrowed.error().cause);
            const bool is_busy = cause && *cause == lux::scene::ESceneRuntimeError::BUSY;
            return lux::cxx::unexpected(render::RendererFailure{
                is_busy ? render::ERendererError::BUSY : render::ERendererError::INVALID_ARGUMENT
            });
        }
        auto& registry = borrowed->get();
        if (!lux::scene::RenderAssets::find(registry, system))
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        auto result = std::unique_ptr<ViewportPresentation>(
            new ViewportPresentation(runtime, scene, resources, simulation::ecs::NullEntity)
        );
        result->system_ = system;
        result->request_ = registry.create();
        result->camera_ = result->request_;
        registry.emplace<simulation::ecs::Transform3D>(result->request_, transform);
        registry.emplace<lux::scene::Camera>(result->request_, camera);
        registry.emplace<lux::scene::RenderViewRequest>(
            result->request_,
            lux::scene::RenderViewRequest{
                .system = system,
                .camera = result->camera_,
                .configuration = config,
                .stop = result->stop_.get_token(),
                .destroy_entity_on_stop = true
            }
        );
        return result;
    }

    render::RenderResult<void> ViewportPresentation::setCameraPose(
        const simulation::ecs::Transform3D& transform,
        const lux::scene::Camera& camera
    ) noexcept
    {
        const bool is_invalid_pose = !transform.translation.allFinite() || !transform.rotation.coeffs().allFinite() ||
                                     !transform.scale.allFinite() || !lux::scene::cameraProjection(camera, 1.0);
        if (camera_ != request_ || is_invalid_pose)
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        const auto borrowed = runtime_.borrowInstance(scene_);
        if (!borrowed)
        {
            const auto* cause = std::get_if<lux::scene::ESceneRuntimeError>(&borrowed.error().cause);
            const bool is_busy = cause && *cause == lux::scene::ESceneRuntimeError::BUSY;
            return lux::cxx::unexpected(render::RendererFailure{
                is_busy ? render::ERendererError::BUSY : render::ERendererError::INVALID_ARGUMENT
            });
        }
        auto& registry = borrowed->get();
        const bool has_target =
            registry.valid(request_) &&
            registry.all_of<simulation::ecs::Transform3D, lux::scene::Camera, lux::scene::RenderViewRequest>(request_);
        if (!has_target)
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        const auto& previous = registry.get<simulation::ecs::Transform3D>(request_);
        const bool same_transform = previous.translation == transform.translation &&
                                    previous.rotation.coeffs() == transform.rotation.coeffs() &&
                                    previous.scale == transform.scale;
        const bool same_camera = sameProjection(registry.get<lux::scene::Camera>(request_), camera);
        const bool is_unchanged = same_transform && same_camera;
        if (is_unchanged)
        {
            return {};
        }
        if (registry.get<lux::scene::RenderViewRequest>(request_).revision == std::numeric_limits<std::uint64_t>::max())
        {
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::CAPACITY});
        }
        if (!same_transform)
        {
            registry.patch<simulation::ecs::Transform3D>(request_, [&](auto& value) { value = transform; });
        }
        if (!same_camera)
        {
            registry.patch<lux::scene::Camera>(request_, [&](auto& value) { value = camera; });
        }
        registry.patch<lux::scene::RenderViewRequest>(request_, [](auto& value) { ++value.revision; });
        return {};
    }

    render::RenderResult<render::PixelExtent> ViewportPresentation::currentImageExtent() const noexcept
    {
        const bool is_local_camera = scene_.valid() && camera_ == request_;
        if (!is_local_camera)
        {
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        }
        const auto borrowed = std::as_const(runtime_).borrowInstance(scene_);
        if (!borrowed)
        {
            const auto* cause = std::get_if<lux::scene::ESceneRuntimeError>(&borrowed.error().cause);
            const bool is_busy = cause && *cause == lux::scene::ESceneRuntimeError::BUSY;
            return lux::cxx::unexpected(render::RendererFailure{
                is_busy ? render::ERendererError::BUSY : render::ERendererError::INVALID_ARGUMENT
            });
        }
        const auto& registry = borrowed->get();
        const bool has_camera =
            registry.valid(camera_) && registry.all_of<lux::scene::Camera, simulation::ecs::WorldTransform3D>(camera_);
        const auto* request = registry.try_get<lux::scene::RenderViewRequest>(request_);
        const auto* adopted = registry.try_get<lux::scene::RenderViewResult>(request_);
        const bool has_request = has_camera && request && adopted;
        const bool is_current = has_request && !adopted->failure && adopted->view == view_ &&
                                adopted->adopted_revision == request->revision &&
                                adopted->published_revision == request->revision && adopted->published_sequence != 0 &&
                                receipt_.status().render_sequence == adopted->published_sequence;
        if (!is_current || !image_resource_.isValid())
        {
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::NOT_READY});
        }
        const auto info = resources_.outputInfo(image_resource_);
        if (!info)
        {
            return lux::cxx::unexpected(info.error());
        }
        const auto& stamp = info->content;
        const bool is_sampleable = info->texture == image_ && info->extent == request->configuration.extent &&
                                   stamp.source.session == scene_.domain &&
                                   stamp.source.view_revision == request->revision &&
                                   stamp.source.surface_generation == adopted->published_sequence &&
                                   stamp.frame_serial != 0 && stamp.evidence >= lux::scene::EImageEvidence::RECORDED;
        if (!is_sampleable)
        {
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::NOT_READY});
        }
        return info->extent;
    }

    render::RenderResult<void> ViewportPresentation::setCamera(lux::simulation::ecs::Entity camera) noexcept
    {
        if (!scene_.valid())
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        const auto borrowed = runtime_.borrowInstance(scene_);
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
        registry.patch<lux::scene::RenderViewRequest>(
            request_,
            [&](auto& request)
            {
                request.camera = camera;
                ++request.revision;
            }
        );
        camera_ = camera;
        return {};
    }

    void ViewportPresentation::collectReceipt() noexcept
    {
        const bool needs_receipt = scene_.valid() && !view_.isValid();
        if (!needs_receipt)
            return;
        const auto borrowed = std::as_const(runtime_).borrowInstance(scene_);
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

    void ViewportPresentation::clearImage() noexcept
    {
        image_ = {};
        if (image_resource_.isValid())
            resources_.release(std::exchange(image_resource_, {}));
    }

    render::ERenderClose ViewportPresentation::close() noexcept
    {
        collectReceipt();
        clearImage();
        highlight_.reset(); // Unsubmitted candidates release here; accepted pins belong to the renderer.
        stop_.request_stop();
        scene_ = {}; // Later receipt checks and destruction cannot touch the old scene.
        const bool is_closed = !view_.isValid() || receipt_.status().status.state == lux::scene::EViewState::CLOSED;
        return is_closed ? render::ERenderClose::COMPLETE : render::ERenderClose::PENDING;
    }

    void ViewportPresentation::update(render::PixelExtent extent) noexcept
    {
        if (!scene_.valid())
            return;
        collectReceipt();
        const auto borrowed = runtime_.borrowInstance(scene_);
        if (!borrowed)
        {
            const auto* error = std::get_if<lux::scene::ESceneRuntimeError>(&borrowed.error().cause);
            const bool is_retired = error && (*error == lux::scene::ESceneRuntimeError::INVALID_ID ||
                                              *error == lux::scene::ESceneRuntimeError::STOPPED);
            if (is_retired)
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
        if (request.configuration.extent != extent)
            registry.patch<lux::scene::RenderViewRequest>(
                request_,
                [&](auto& value)
                {
                    value.configuration.extent = extent;
                    ++value.revision;
                }
            );
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
        image_ = info->texture;
        const auto old = std::exchange(image_resource_, *output);
        if (old.isValid())
            resources_.release(old);
    }

} // namespace lux::editor::views

namespace lux::editor::views
{
    render::RenderResult<EOverlaySubmit> ViewportPresentation::updateOverlay(
        render::RenderRuntime& renderer,
        OverlayConfiguration configuration
    )
    {
        if (!scene_.valid() || !view_.isValid())
            return EOverlaySubmit::NOT_READY;
        if (!highlight_)
            highlight_ = std::make_unique<HighlightRenderer>();
        return highlight_->update(runtime_, scene_, system_, resources_, renderer, view_, configuration);
    }
    void ViewportPresentation::retryOverlay() noexcept
    {
        if (highlight_)
            highlight_->rejected.reset();
    }
} // namespace lux::editor::views
