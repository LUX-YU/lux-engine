#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/systems/render/view/RenderTargetComponent.ecs_schema.hpp>
#include <lux/engine/systems/render/view/RenderViewComponent.ecs_schema.hpp>
#include <lux/engine/systems/render/view/RenderViewComponent.hpp>
#include <lux/engine/systems/render/view/ViewCollector.hpp>
#include <lux/engine/systems/render/view/ViewReaction.hpp>
#include <utility>

namespace lux::engine::render
{
    lux::render::TargetId findRenderTarget(const ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        const auto* output = registry.valid(entity) ? registry.try_get<RenderViewComponent>(entity) : nullptr;
        return output ? output->target : lux::render::TargetId{};
    }

    const lux::render::TargetDesc* renderTargetDescription(const ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        const auto* output = registry.valid(entity) ? registry.try_get<RenderViewComponent>(entity) : nullptr;
        return output ? &output->description : nullptr;
    }

    ViewReaction::CreateResult ViewReaction::create(RenderSystem& system) noexcept
    {
        return std::unique_ptr<ViewReaction>{new ViewReaction(system)};
    }

    ViewReaction::ViewReaction(RenderSystem& system) noexcept : RenderReaction(system)
    {
        watchComponents<RenderTargetComponent>();
        connectConstruct<scene::ViewComponent, &ViewReaction::inputChanged>(*this);
        connectRemoval<scene::ViewComponent, &ViewReaction::inputChanged>(*this);
        connectConstruct<ecs::Transform3D, &ViewReaction::inputChanged>(*this);
        connectRemoval<ecs::Transform3D, &ViewReaction::inputChanged>(*this);
        connectRemoval<RenderViewComponent, &ViewReaction::removeOutput>(*this);
    }

    ViewReaction::~ViewReaction() noexcept = default;

    void ViewReaction::inputChanged(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        if (registry.any_of<scene::ViewComponent, RenderViewComponent>(entity)) notify(entity);
    }

    void ViewReaction::reportFailure(ecs::Entity entity, error::Error error) noexcept
    {
        if (error.type == lux::render::kDeviceBusy)
        {
            // Retry through the existing root scheduler; the adopted output remains authoritative.
            // CPU visibility continues to follow component inputs independently of Server admission.
            notify(entity);
            return;
        }
        static_cast<void>(emit(outputFailed, Failure{entity, error}));
    }

    void ViewReaction::removeOutput(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        auto resources = std::exchange(registry.get<RenderViewComponent>(entity), {});
        const bool empty = resources.view.isNull() && resources.target.isNull() && resources.attachment.isNull();
        if (!empty)
        {
            removed_resources_.emplace(entity, std::move(resources));
            notify(entity);
        }
    }

    RenderViewComponent ViewReaction::takeOutput(ecs::Entity entity) noexcept
    {
        auto& registry = this->registry();
        auto* adopted = registry.valid(entity) ? registry.try_get<RenderViewComponent>(entity) : nullptr;
        if (!adopted)
        {
            return {};
        }
        auto resources = std::exchange(*adopted, {});
        registry.remove<RenderViewComponent>(entity);
        return resources;
    }

    exec::task<ViewReaction::Result> ViewReaction::reconcile(ecs::Entity entity) noexcept
    {
        auto drained = co_await drainRemoved(entity);
        if (!drained)
        {
            co_return drained;
        }
        auto& registry = this->registry();
        const bool present = registry.valid(entity) &&
                             registry.all_of<scene::ViewComponent, ecs::Transform3D, RenderTargetComponent>(entity);
        if (!present)
        {
            co_return co_await discard(entity, takeOutput(entity));
        }
        const auto desired = registry.get<RenderTargetComponent>(entity);
        const auto* adopted = registry.try_get<RenderViewComponent>(entity);
        const bool suspended = !desired.description.width || !desired.description.height;
        if (suspended)
        {
            if (adopted)
            {
                auto response =
                    co_await system().context().renderClient().request<lux::render::SetTargetActive>(
                        {adopted->target, false, adopted->epoch}
                    );
                if (response.error.type)
                {
                    co_return cxx::unexpected(response.error);
                }
            }
            co_return ViewReaction::Result{};
        }
        const bool same_surface =
            adopted && adopted->surface == desired.surface && adopted->description.kind == desired.description.kind;
        if (same_surface)
        {
            co_return co_await resizeOutput(entity, adopted->target, desired.description);
        }
        co_return co_await createOutput(entity, desired);
    }

    exec::task<ViewReaction::Result>
    ViewReaction::createOutput(ecs::Entity entity, RenderTargetComponent desired) noexcept
    {
        const auto current = [&]() noexcept
        {
            const bool present = canAdopt(entity) &&
                registry().all_of<scene::ViewComponent, ecs::Transform3D, RenderTargetComponent>(entity);
            if (!present) return false;
            const auto& target = registry().get<RenderTargetComponent>(entity);
            return target.description == desired.description && target.surface == desired.surface;
        };
        RenderViewComponent candidate;
        auto& client = system().context().renderClient();
        auto target = co_await client.request<lux::render::CreateTarget>({desired.description, desired.surface});
        if (target.error.type)
        {
            co_return cxx::unexpected(target.error);
        }
        candidate.target = target.target;
        candidate.description = target.description;
        candidate.epoch = target.epoch;
        candidate.surface = desired.surface;
        if (!current())
        {
            co_return co_await discard(entity, std::move(candidate));
        }
        auto view = co_await client.request<lux::render::CreateView>({system().sceneId()});
        if (view.error.type)
        {
            co_return co_await discard(entity, std::move(candidate), view.error);
        }
        candidate.view = view.view;
        if (!current())
        {
            co_return co_await discard(entity, std::move(candidate));
        }
        auto parameters = makeViewParameters(
            registry().get<scene::ViewComponent>(entity),
            registry().get<ecs::Transform3D>(entity),
            candidate.description
        );
        if (!parameters)
        {
            co_return co_await discard(entity, std::move(candidate), parameters.error());
        }
        lux::render::AttachmentRequest input{system().sceneId(), candidate.view, candidate.target, system().effectId()};
        input.parameter_type = cxx::make_basic_type_info<lux::render::CameraInput>();
        const auto bytes = std::as_bytes(std::span{&*parameters, 1});
        input.parameters.assign(bytes.begin(), bytes.end());
        auto attached = co_await client.request<lux::render::Attach>(std::move(input));
        if (attached.error.type)
        {
            co_return co_await discard(entity, std::move(candidate), attached.error);
        }
        candidate.attachment = attached.attachment;
        if (!current())
        {
            co_return co_await discard(entity, std::move(candidate));
        }
        auto previous = takeOutput(entity);
        registry().emplace<RenderViewComponent>(entity, std::exchange(candidate, {}));
        co_return co_await discard(entity, std::move(previous));
    }

    exec::task<ViewReaction::Result> ViewReaction::resizeOutput(
        ecs::Entity entity,
        lux::render::TargetId target,
        lux::render::TargetDesc description
    ) noexcept
    {
        auto response = co_await system().context().renderClient().request<lux::render::ResizeTarget>(
            {target, description}
        );
        if (response.error.type)
        {
            co_return cxx::unexpected(response.error);
        }
        auto* adopted = registry().valid(entity) ? registry().try_get<RenderViewComponent>(entity) : nullptr;
        if (!adopted || adopted->target != target)
        {
            co_return ViewReaction::Result{}; // The removal owner will release after this root ends.
        }
        // This reply describes a change already accepted by Server, even if the desired extent changed again.
        registry().patch<RenderViewComponent>(
            entity,
            [&](auto& output) noexcept
            {
                output.description = response.description;
                output.epoch = response.epoch;
            }
        );
        const bool has_inputs = canAdopt(entity) &&
            registry().all_of<scene::ViewComponent, ecs::Transform3D, RenderTargetComponent>(entity);
        const bool is_stale_target = !has_inputs ||
            registry().get<RenderTargetComponent>(entity).description != description ||
            registry().get<RenderTargetComponent>(entity).surface != adopted->surface;
        if (is_stale_target)
        {
            co_return ViewReaction::Result{};
        }
        co_return co_await resumeOutput(entity);
    }

    exec::task<ViewReaction::Result> ViewReaction::resumeOutput(ecs::Entity entity) noexcept
    {
        const auto& adopted = registry().get<RenderViewComponent>(entity);
        const auto desired = registry().get<RenderTargetComponent>(entity);
        const auto target = adopted.target;
        const auto epoch = adopted.epoch;
        auto parameters = makeViewParameters(
            registry().get<scene::ViewComponent>(entity),
            registry().get<ecs::Transform3D>(entity),
            adopted.description
        );
        if (!parameters)
        {
            co_return cxx::unexpected(parameters.error());
        }
        lux::render::AttachmentInputs input;
        input.attachment = adopted.attachment;
        input.parameter_type = cxx::make_basic_type_info<lux::render::CameraInput>();
        const auto bytes = std::as_bytes(std::span{&*parameters, 1});
        input.parameters.assign(bytes.begin(), bytes.end());
        auto& client = system().context().renderClient();
        auto response = co_await client.request<lux::render::UpdateAttachmentInputs>(std::move(input));
        if (response.error.type)
        {
            co_return cxx::unexpected(response.error);
        }
        const bool has_inputs = canAdopt(entity) && registry().all_of<
            scene::ViewComponent, ecs::Transform3D, RenderTargetComponent, RenderViewComponent
        >(entity);
        const bool is_stale_target = !has_inputs ||
            registry().get<RenderTargetComponent>(entity).description != desired.description ||
            registry().get<RenderTargetComponent>(entity).surface != desired.surface ||
            registry().get<RenderViewComponent>(entity).target != target;
        if (is_stale_target)
        {
            co_return ViewReaction::Result{};
        }
        auto resumed = co_await client.request<lux::render::SetTargetActive>({target, true, epoch});
        co_return resumed.error.type ? ViewReaction::Result{cxx::unexpected(resumed.error)} : ViewReaction::Result{};
    }

    exec::task<ViewReaction::Result> ViewReaction::releaseOutput(RenderViewComponent& resources) noexcept
    {
        auto& client = system().context().renderClient();
        if (!resources.attachment.isNull())
        {
            auto removed = co_await client.request<lux::render::Detach>(resources.attachment);
            if (removed.error.type)
            {
                co_return cxx::unexpected(removed.error);
            }
            resources.attachment = {};
        }
        if (!resources.view.isNull())
        {
            auto removed = co_await client.request<lux::render::RemoveView>({system().sceneId(), resources.view});
            if (removed.error.type)
            {
                co_return cxx::unexpected(removed.error);
            }
            resources.view = {};
        }
        if (!resources.target.isNull())
        {
            auto removed = co_await client.request<lux::render::RemoveTarget>(resources.target);
            if (removed.error.type)
            {
                co_return cxx::unexpected(removed.error);
            }
            resources.target = {};
        }
        co_return ViewReaction::Result{};
    }

    exec::task<ViewReaction::Result>
    ViewReaction::discard(ecs::Entity entity, RenderViewComponent resources, error::Error cause) noexcept
    {
        auto released = co_await releaseOutput(resources);
        if (!released)
        {
            removed_resources_.emplace(entity, std::move(resources));
            if (released.error().type != lux::render::kDeviceBusy)
            {
                RenderReaction::reportFailure(entity, released.error());
            }
            co_return released;
        }
        co_return cause.type ? ViewReaction::Result{cxx::unexpected(cause)} : ViewReaction::Result{};
    }

    exec::task<ViewReaction::Result> ViewReaction::drainRemoved(ecs::Entity entity) noexcept
    {
        for (;;)
        {
            const auto found = removed_resources_.find(entity);
            if (found == removed_resources_.end())
            {
                co_return ViewReaction::Result{};
            }
            auto node = removed_resources_.extract(found);
            auto released = co_await releaseOutput(node.mapped());
            if (!released)
            {
                removed_resources_.insert(std::move(node));
                if (released.error().type != lux::render::kDeviceBusy)
                {
                    RenderReaction::reportFailure(entity, released.error());
                }
                co_return released;
            }
        }
    }

    exec::task<ViewReaction::Result> ViewReaction::releaseResources() noexcept
    {
        while (!removed_resources_.empty())
        {
            auto released = co_await drainRemoved(removed_resources_.begin()->first);
            if (!released)
            {
                co_return released;
            }
        }
        for (;;)
        {
            auto adopted = registry().view<RenderViewComponent>();
            if (adopted.begin() == adopted.end())
            {
                co_return ViewReaction::Result{};
            }
            const auto entity = *adopted.begin();
            auto released = co_await discard(entity, takeOutput(entity));
            if (!released)
            {
                co_return released;
            }
        }
    }
} // namespace lux::engine::render
