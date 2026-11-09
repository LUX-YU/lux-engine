#include <lux/engine/systems/render/RenderReaction.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/world/World.hpp>

namespace lux::engine::render
{
    RenderReaction::RenderReaction(RenderSystem& system) noexcept : system_(system) {}

    ecs::Registry& RenderReaction::registry() noexcept
    {
        return system_.world().registry();
    }

    void RenderReaction::inputChanged(ecs::Registry&, ecs::Entity entity) noexcept
    {
        notify(entity);
    }

    void RenderReaction::notify(ecs::Entity entity) noexcept
    {
        if (!system().taskScope().stopToken().stop_requested())
        {
            pending_.insert(entity);
        }
    }

    bool RenderReaction::canAdopt(ecs::Entity entity) const noexcept
    {
        return !system_.taskScope().stopToken().stop_requested() && system_.world().registry().valid(entity);
    }

    void RenderReaction::reportFailure(ecs::Entity, error::Error error) noexcept
    {
        if (!failure_)
        {
            failure_ = error;
        }
    }

    lux::render::RenderResult<void> RenderReaction::process() noexcept
    {
        if (failure_)
        {
            return cxx::unexpected(*failure_);
        }
        if (system().taskScope().stopToken().stop_requested())
        {
            return {};
        }
        processing_.clear();
        processing_.swap(pending_);
        for (const auto entity : processing_)
        {
            if (!in_flight_entities_.insert(entity).second)
            {
                pending_.insert(entity); // Preserve changes while the previous root is suspended.
                continue;
            }
            auto accepted = system().taskScope().spawnOnMain(
                stdexec::write_env(
                    reconcile(entity),
                    stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                ),
                [this, entity](auto outcome) noexcept
                {
                    in_flight_entities_.erase(entity);
                    if (!outcome)
                    {
                        const auto* domain = outcome.error().domainFailure();
                        if (domain && domain->type != lux::render::kOutcomeUnknown)
                        {
                            reportFailure(entity, *domain);
                        }
                        else
                        {
                            const auto* execution_error = outcome.error().executionFailure();
                            RenderReaction::reportFailure(
                                entity,
                                domain ? *domain
                                       : execution::toError(
                                             execution_error ? *execution_error : execution::EExecutionError::STOPPING
                                         )
                            );
                        }
                    }
                }
            );
            if (!accepted)
            {
                in_flight_entities_.erase(entity);
                RenderReaction::reportFailure(entity, execution::toError(accepted.error()));
                return cxx::unexpected(*failure_);
            }
        }
        return {};
    }

    exec::task<RenderReaction::Result> RenderReaction::releaseResources() noexcept
    {
        co_return Result{};
    }
} // namespace lux::engine::render
