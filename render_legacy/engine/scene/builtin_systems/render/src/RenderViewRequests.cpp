#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/Camera.hpp>

#include <algorithm>

namespace lux::scene
{
    namespace
    {
        using simulation::ecs::Entity;
        using simulation::ecs::NullEntity;

        bool sameOutput(const ViewConfig& left, const ViewConfig& right) noexcept
        {
            if (left.output.index() != right.output.index())
                return false;
            const auto* native = std::get_if<NativeSurfaceOutput>(&left.output);
            return !native || native->native_window == std::get<NativeSurfaceOutput>(right.output).native_window;
        }
    }

    void RenderSystem::CancelViewRequest::operator()() const noexcept
    {
        const auto found = system->view_request_indices_.find(entity);
        if (found == system->view_request_indices_.end())
            return;
        auto& record = system->view_requests_[found->second];
        record.removed = true;
        record.dirty = true;
        // Prepared updates have already captured their own passive uses. Revocation
        // never waits for a UI frame or mutates Registry from a cancellation callback.
        if (record.view.isValid())
            system->resources_.release(std::exchange(record.view, {}));
    }

    void RenderSystem::requestChanged(simulation::ecs::Registry&, Entity entity) noexcept
    {
        const auto [entry, inserted] = view_request_indices_.try_emplace(entity, view_requests_.size());
        if (inserted)
            view_requests_.push_back({.entity = entity});
        else
            view_requests_[entry->second].dirty = true;
    }

    void RenderSystem::requestDestroyed(simulation::ecs::Registry&, Entity entity) noexcept
    {
        const auto found = view_request_indices_.find(entity);
        if (found != view_request_indices_.end())
        {
            auto& record = view_requests_[found->second];
            record.removed = true;
            record.dirty = true;
        }
    }

    void RenderSystem::maintainViewRequests() noexcept
    {
        view_request_batch_.clear();
        for (auto& record : view_requests_)
        {
            const bool has_camera = record.adopted.camera != NullEntity;
            const bool is_camera_missing = has_camera && (!registry_.valid(record.adopted.camera) ||
                                                          !registry_.all_of<Camera>(record.adopted.camera));
            const auto* result = registry_.try_get<RenderViewResult>(record.entity);
            if (record.dirty || (is_camera_missing && result && !result->failure))
            {
                view_request_batch_.push_back(record.entity);
                record.dirty = false;
            }
        }
        for (const auto entity : view_request_batch_)
        {
            const auto index = view_request_indices_.at(entity);
            auto& record = view_requests_[index];
            const auto* request = registry_.try_get<RenderViewRequest>(entity);
            bool destroy_entity{};
            if (request && request->stop.stop_requested())
            {
                destroy_entity = request->destroy_entity_on_stop;
                record.cancellation.reset();
                if (record.view.isValid())
                    resources_.release(std::exchange(record.view, {}));
                registry_.remove<RenderViewRequest>(entity);
                request = nullptr;
            }
            if (record.removed || !request)
            {
                if (record.view.isValid())
                {
                    const auto observed = resources_.observeView(record.view);
                    if (observed && observed->handle.isValid())
                        retiring_views_.push_back(std::exchange(record.view, {}));
                    else
                        resources_.release(std::exchange(record.view, {}));
                }
                record.cancellation.reset();
                record.adopted = {};
                record.attempted_revision = 0;
                record.removed = false;
                if (registry_.valid(entity))
                    registry_.remove<RenderViewResult>(entity);
                if (!request)
                {
                    view_request_indices_.erase(entity);
                    if (index + 1 != view_requests_.size())
                    {
                        record = std::move(view_requests_.back());
                        view_request_indices_.at(record.entity) = index;
                    }
                    view_requests_.pop_back();
                    if (destroy_entity)
                        registry_.destroy(entity);
                    continue;
                }
            }
            const auto input = *request;
            // Result construction happens only at the structure-safe maintenance boundary.
            auto& result = registry_.get_or_emplace<RenderViewResult>(entity);
            const auto fail = [&](render::RendererFailure failure) {
                registry_.patch<RenderViewResult>(entity, [&](auto& value) {
                    value.failure = failure;
                    value.published_revision = 0;
                    value.published_sequence = 0;
                });
            };
            const bool has_camera = input.camera != NullEntity;
            const bool is_invalid_camera =
                has_camera && (!registry_.valid(input.camera) || !registry_.all_of<Camera>(input.camera));
            const bool is_invalid_target = input.system != instance_ || input.revision == 0;
            const bool is_invalid_extent =
                input.configuration.extent.width > 16384 || input.configuration.extent.height > 16384;
            const bool is_output_changed =
                record.view.isValid() && !sameOutput(input.configuration, record.adopted.configuration);
            if (is_invalid_camera || is_invalid_target || is_invalid_extent || is_output_changed)
            {
                record.attempted_revision = std::max(record.attempted_revision, input.revision);
                fail({render::ERendererError::INVALID_ARGUMENT});
                continue;
            }
            if (input.revision <= record.attempted_revision)
            {
                const bool is_same_adopted = input.revision == record.adopted.revision && record.view.isValid() &&
                                             input.camera == record.adopted.camera &&
                                             input.configuration.extent == record.adopted.configuration.extent;
                if (!is_same_adopted && !result.failure)
                    fail({render::ERendererError::INVALID_ARGUMENT});
                continue;
            }
            record.attempted_revision = input.revision;
            if (!record.view.isValid())
            {
                auto created = resources_.requestView(scene_state_.resource, input.configuration);
                if (!created)
                {
                    fail(created.error());
                    continue;
                }
                record.view = *created;
            }
            else
            {
                auto resized = resources_.requestViewExtent(record.view, input.configuration.extent);
                if (!resized)
                {
                    fail(resized.error());
                    continue;
                }
            }
            if (record.adopted.stop != input.stop)
            {
                record.cancellation.reset();
                if (input.stop.stop_possible())
                    record.cancellation = std::make_unique<std::stop_callback<CancelViewRequest>>(
                        input.stop,
                        CancelViewRequest{this, entity}
                    );
            }
            record.adopted = input;
            registry_.patch<RenderViewResult>(entity, [&](auto& value) {
                value.view = record.view;
                value.adopted_revision = input.revision;
                value.published_revision = 0;
                value.published_sequence = 0;
                value.failure.reset();
            });
        }
        // Resource replies can become ready without a Registry notification.
        for (const auto& record : view_requests_)
        {
            if (!record.view.isValid())
                continue;
            auto* result = registry_.try_get<RenderViewResult>(record.entity);
            const auto observed = resources_.observeView(record.view);
            if (result && !result->failure && observed && observed->status.failure)
                registry_.patch<RenderViewResult>(record.entity, [&](auto& value) {
                    value.failure = observed->status.failure;
                    value.published_revision = 0;
                    value.published_sequence = 0;
                });
        }
    }

    void RenderSystem::captureViewPublications()
    {
        view_publications_.clear();
        for (const auto& association : view_associations_.values)
        {
            if (association.request == NullEntity)
                continue;
            view_publications_.push_back(
                {association.request,
                 association.resource,
                 association.view,
                 association.extent,
                 association.request_revision,
                 association.output_sequence}
            );
        }
    }

    void RenderSystem::commitViewPublications() noexcept
    {
        for (const auto& publication : view_publications_)
        {
            auto* result = registry_.try_get<RenderViewResult>(publication.entity);
            const auto* request = registry_.try_get<RenderViewRequest>(publication.entity);
            const bool is_current_request = result && request && !result->failure && result->view == publication.view &&
                                            result->adopted_revision == publication.revision &&
                                            request->revision == publication.revision;
            if (!is_current_request)
                continue;
            const auto observed = resources_.observeView(publication.view);
            const bool is_current_view = observed && observed->handle == publication.handle &&
                                         observed->render_extent == publication.extent &&
                                         observed->render_sequence == publication.sequence;
            if (!is_current_view)
                continue;
            // Publication identity is independent of eventual GPU completion evidence.
            const auto stamped = resources_.setViewOutput(
                publication.view,
                {view_associations_.instance.domain, publication.revision, publication.revision, publication.sequence}
            );
            registry_.patch<RenderViewResult>(publication.entity, [&](auto& value) {
                if (!stamped)
                {
                    value.failure = stamped.error();
                    value.published_revision = 0;
                    value.published_sequence = 0;
                }
                else
                {
                    value.published_revision = publication.revision;
                    value.published_sequence = publication.sequence;
                }
            });
        }
        view_publications_.clear();
    }
}
