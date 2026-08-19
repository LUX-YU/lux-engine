#include <lux/engine/runtime/spatial3d/navigation/Spatial3DNavigationAdapterSystem.hpp>

#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>

#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lux::runtime::spatial3d
{
    namespace
    {
        enum class EOwnedRequestState : std::uint8_t
        {
            FREE,
            WAITING_ADMISSION,
            IN_FLIGHT,
            READY_SUCCESS,
            READY_FAILURE
        };

        struct OwnedRequest final
        {
            std::uint32_t generation{1u};
            EOwnedRequestState state{EOwnedRequestState::FREE};
            entt::entity entity{entt::null};
            std::uint64_t navigation_generation{0u};
            lux::ecs::scene_format::ContentBlobRef reference;
            lux::runtime::entity_scene::ContentBlobLease content;
            std::optional<lux::navigation::detour3d::PreparedNavigationRegion3D>
                prepared;
            std::optional<lux::navigation::detour3d::NavigationRegion3DFailure>
                failure;
            std::size_t owned_bytes{0u};
        };

        struct CompletionControl final
        {
            std::atomic<Spatial3DNavigationAdapterSystem*> owner{nullptr};
        };

        [[nodiscard]] lux::navigation::detour3d::NavigationRegion3DFailure
        invalidContent(std::string detail) noexcept
        {
            return {lux::navigation::detour3d::ENavigationRegion3DError::
                        INVALID_CONTENT,
                    std::move(detail)};
        }
    } // namespace

    struct Spatial3DNavigationAdapterSystem::Impl final
    {
        Impl(Spatial3DNavigationAdapterSystem& owner_value,
             lux::exec::AsyncRuntime& async_runtime_value,
             lux::exec::AsyncScope& scene_scope_value,
             Navigation3DPrepareClient preparation_value,
             lux::ecs::Navigation3DSystem& navigation_value,
             lux::runtime::entity_scene::ContentBlobClient content_value,
             Spatial3DNavigationAdapterConfig config_value)
            : async_runtime(&async_runtime_value),
              preparation(std::move(preparation_value)),
              navigation(&navigation_value), content(std::move(content_value)),
              scope(&scene_scope_value),
              completion(std::make_shared<CompletionControl>()),
              config(config_value)
        {
            if (config.maximum_owned_requests == 0u ||
                config.maximum_owned_bytes == 0u)
                std::abort();
            requests.resize(config.maximum_owned_requests);
            completion->owner.store(&owner_value, std::memory_order_release);
        }

        [[nodiscard]] OwnedRequest* find(std::uint32_t slot,
                                         std::uint32_t generation) noexcept
        {
            if (slot >= requests.size())
                return nullptr;
            auto& request = requests[slot];
            return request.state != EOwnedRequestState::FREE &&
                           request.generation == generation
                       ? &request
                       : nullptr;
        }

        [[nodiscard]] OwnedRequest* allocate() noexcept
        {
            const auto found = std::find_if(
                requests.begin(),
                requests.end(),
                [](const OwnedRequest& request) noexcept
                {
                    return request.state == EOwnedRequestState::FREE &&
                           request.generation != 0u;
                });
            return found == requests.end() ? nullptr : &*found;
        }

        [[nodiscard]] std::uint32_t
        slotOf(const OwnedRequest& request) const noexcept
        {
            return static_cast<std::uint32_t>(&request - requests.data());
        }

        void release(OwnedRequest& request) noexcept
        {
            if (request.state == EOwnedRequestState::FREE)
                return;
            // A material prepared batch must first transfer to the ECS owner,
            // which either adopts it or queues bounded stale retirement.
            // Releasing it here would synchronously destroy every layer.
            if (request.prepared && request.prepared->valid())
                std::abort();
            if (request.owned_bytes > snapshot.current_bytes)
                std::abort();
            snapshot.current_bytes -= request.owned_bytes;
            if (request.state == EOwnedRequestState::READY_SUCCESS ||
                request.state == EOwnedRequestState::READY_FAILURE)
            {
                if (snapshot.current_completions == 0u)
                    std::abort();
                --snapshot.current_completions;
            }
            if (snapshot.current_requests == 0u)
                std::abort();
            --snapshot.current_requests;
            request.content = {};
            request.prepared.reset();
            request.failure.reset();
            request.entity = entt::null;
            request.navigation_generation = 0u;
            request.reference = {};
            request.owned_bytes = 0u;
            request.state = EOwnedRequestState::FREE;
            if (request.generation != std::numeric_limits<std::uint32_t>::max())
            {
                ++request.generation;
            }
            else
            {
                request.generation = 0u;
            }
        }

        void readyFailure(OwnedRequest& request,
                          lux::navigation::detour3d::NavigationRegion3DFailure
                              failure) noexcept
        {
            if (request.prepared && request.prepared->valid())
                std::abort();
            request.prepared.reset();
            request.failure.emplace(std::move(failure));
            request.state = EOwnedRequestState::READY_FAILURE;
            ++snapshot.current_completions;
        }

        void launch(OwnedRequest& request) noexcept
        {
            if (request.state != EOwnedRequestState::WAITING_ADMISSION)
                return;
            if (closing || !preparation)
            {
                if (closing)
                {
                    ++snapshot.cancelled_requests;
                    release(request);
                    return;
                }
                readyFailure(
                    request,
                    invalidContent("navigation preparation service is closed"));
                return;
            }

            auto blob =
                lux::navigation::detour3d::navigationRegion3DBlobFromBytes(
                    request.content.bytes());
            if (!blob)
            {
                readyFailure(request, std::move(blob.error()));
                return;
            }

            const auto slot = slotOf(request);
            const auto slot_generation = request.generation;
            auto submitted = preparation.execute(BuildNavigationRegion3D{
                std::move(*blob), request.navigation_generation});
            if (!submitted)
            {
                if (submitted.error() ==
                        lux::exec::EAsyncSubmitError::QUEUE_FULL ||
                    submitted.error() == lux::exec::EAsyncSubmitError::
                                             BYTE_BUDGET_EXHAUSTED)
                {
                    ++snapshot.queue_backpressure;
                    return;
                }
                readyFailure(
                    request,
                    invalidContent("navigation preparation service is closed"));
                return;
            }
            auto pipeline =
                std::move(*submitted) |
                stdexec::continues_on(
                    lux::exec::mainThreadScheduler(*async_runtime)) |
                stdexec::then(
                    [weak = std::weak_ptr{completion}, slot, slot_generation](
                        lux::exec::AsyncOutcome<BuildNavigationRegion3D>
                            outcome) mutable noexcept
                    {
                        const auto locked = weak.lock();
                        if (!locked)
                            return;
                        auto* target =
                            locked->owner.load(std::memory_order_acquire);
                        if (target)
                        {
                            target->acceptPreparation(
                                slot, slot_generation, std::move(outcome));
                        }
                    }) |
                stdexec::upon_stopped(
                    [weak = std::weak_ptr{completion},
                     slot,
                     slot_generation]() noexcept
                    {
                        const auto locked = weak.lock();
                        if (!locked)
                            return;
                        auto* target =
                            locked->owner.load(std::memory_order_acquire);
                        if (target)
                        {
                            target->acceptPreparationStopped(slot,
                                                             slot_generation);
                        }
                    });
            request.state = EOwnedRequestState::IN_FLIGHT;
            if (!lux::exec::spawn(*scope, std::move(pipeline)))
            {
                request.state = EOwnedRequestState::WAITING_ADMISSION;
                ++snapshot.queue_backpressure;
                return;
            }
            ++snapshot.submitted_requests;
        }

        void deliverReady() noexcept
        {
            for (auto& request : requests)
            {
                if (request.state != EOwnedRequestState::READY_SUCCESS &&
                    request.state != EOwnedRequestState::READY_FAILURE)
                {
                    continue;
                }
                bool accepted = false;
                if (request.prepared)
                {
                    // The ECS owner is also the only bounded discard owner.
                    // Always offer a material batch, even when its entity or
                    // generation is already stale.
                    accepted = navigation->acceptPrepared(
                        request.entity,
                        request.navigation_generation,
                        std::move(*request.prepared));
                }
                else if (request.failure)
                {
                    const auto status = navigation->status(request.entity);
                    if (!status ||
                        status->generation !=
                            request.navigation_generation ||
                        status->state != lux::ecs::
                            ENavigationRegion3DState::WAITING_BACKGROUND)
                    {
                        ++snapshot.stale_completions;
                        release(request);
                        continue;
                    }
                    accepted =
                        navigation->acceptFailure(request.entity,
                                                  request.navigation_generation,
                                                  std::move(*request.failure));
                }
                if (!accepted)
                {
                    // A stale/closing completion is deliberately consumed by
                    // Navigation3DSystem so its prepared backend layers can
                    // retire one granule per owner tick. Queue backpressure,
                    // in contrast, leaves the value intact for retry.
                    if (request.prepared && !request.prepared->valid())
                    {
                        ++snapshot.stale_completions;
                        release(request);
                        continue;
                    }
                    ++snapshot.queue_backpressure;
                    continue;
                }
                ++snapshot.accepted_completions;
                release(request);
            }
        }

        void admitPending() noexcept
        {
            const auto pending = navigation->pendingPreparationRequests();
            std::size_t consumed = 0u;
            for (const auto& source : pending)
            {
                auto* request = allocate();
                if (!request)
                {
                    ++snapshot.queue_backpressure;
                    break;
                }
                request->state = EOwnedRequestState::WAITING_ADMISSION;
                request->entity = source.entity;
                request->navigation_generation = source.generation;
                request->reference = source.content;
                ++snapshot.current_requests;

                if (!source.content.valid() || source.generation == 0u ||
                    source.content.type.name() !=
                        lux::navigation::detour3d::
                            kNavigationRegion3DContentTypeName ||
                    source.content.schema_version !=
                        lux::navigation::detour3d::
                            kNavigationRegion3DSchemaVersion)
                {
                    readyFailure(
                        *request,
                        invalidContent(
                            "navigation content reference is invalid"));
                    ++snapshot.admitted_requests;
                    ++consumed;
                    continue;
                }

                auto lease = content.resolve(source.content);
                if (!lease)
                {
                    readyFailure(
                        *request,
                        invalidContent(
                            "navigation content bytes are unavailable"));
                    ++snapshot.admitted_requests;
                    ++consumed;
                    continue;
                }
                const auto bytes = lease->bytes().size();
                if (bytes > config.maximum_owned_bytes)
                {
                    readyFailure(
                        *request,
                        lux::navigation::detour3d::NavigationRegion3DFailure{
                            lux::navigation::detour3d::
                                ENavigationRegion3DError::CAPACITY_EXHAUSTED,
                            "navigation content exceeds the adapter byte "
                            "budget"});
                    ++snapshot.admitted_requests;
                    ++consumed;
                    continue;
                }
                if (snapshot.current_bytes >
                    config.maximum_owned_bytes - bytes)
                {
                    ++snapshot.queue_backpressure;
                    release(*request);
                    break;
                }
                request->content = std::move(*lease);
                request->owned_bytes = bytes;
                snapshot.current_bytes += request->owned_bytes;
                ++snapshot.admitted_requests;
                ++consumed;
            }
            navigation->consumePreparationRequests(consumed);
        }

        lux::exec::AsyncRuntime* async_runtime{nullptr};
        Navigation3DPrepareClient preparation;
        lux::ecs::Navigation3DSystem* navigation{nullptr};
        lux::runtime::entity_scene::ContentBlobClient content;
        lux::exec::AsyncScope* scope{nullptr};
        std::shared_ptr<CompletionControl> completion;
        Spatial3DNavigationAdapterConfig config;
        std::vector<OwnedRequest> requests;
        Spatial3DNavigationAdapterSnapshot snapshot;
        bool closing{false};
    };

    Spatial3DNavigationAdapterSystem::Spatial3DNavigationAdapterSystem(
        lux::exec::AsyncRuntime& async_runtime,
        lux::exec::AsyncScope& scene_scope,
        Navigation3DPrepareClient preparation,
        lux::ecs::Navigation3DSystem& navigation,
        lux::runtime::entity_scene::ContentBlobClient content,
        Spatial3DNavigationAdapterConfig config)
        : impl_(std::make_unique<Impl>(*this,
                                       async_runtime,
                                       scene_scope,
                                       std::move(preparation),
                                       navigation,
                                       std::move(content),
                                       config))
    {
    }

    Spatial3DNavigationAdapterSystem::~Spatial3DNavigationAdapterSystem()
    {
        impl_->completion->owner.store(nullptr, std::memory_order_release);
        if (impl_->snapshot.current_requests != 0u ||
            impl_->snapshot.current_completions != 0u ||
            impl_->snapshot.current_bytes != 0u)
        {
            // Every material completion must have transferred to the bounded
            // ECS owner before the adapter can disappear.
            std::abort();
        }
    }

    void Spatial3DNavigationAdapterSystem::update(
        const lux::ecs::SystemUpdateContext&)
    {
        impl_->deliverReady();
        if (impl_->closing)
            return;
        impl_->admitPending();
        for (auto& request : impl_->requests)
            impl_->launch(request);
    }

    std::span<const lux::ecs::ISystem::Type>
    Spatial3DNavigationAdapterSystem::prerequisites() const noexcept
    {
        static constexpr Type result[]{
            lux::ecs::systemType<lux::ecs::Navigation3DSystem>()};
        return result;
    }

    std::span<const lux::ecs::ISystem::Type>
    Spatial3DNavigationAdapterSystem::runsAfter() const noexcept
    {
        return prerequisites();
    }

    Spatial3DNavigationAdapterSnapshot
    Spatial3DNavigationAdapterSystem::snapshot() const noexcept
    {
        auto result = impl_->snapshot;
        result.waiting_admission_requests = 0u;
        result.in_flight_requests = 0u;
        for (const auto& request : impl_->requests)
        {
            if (request.state == EOwnedRequestState::WAITING_ADMISSION)
                ++result.waiting_admission_requests;
            else if (request.state == EOwnedRequestState::IN_FLIGHT)
                ++result.in_flight_requests;
        }
        return result;
    }

    void Spatial3DNavigationAdapterSystem::requestClose() noexcept
    {
        if (impl_->closing)
            return;
        impl_->closing = true;
        impl_->snapshot.closing = true;
        for (auto& request : impl_->requests)
        {
            if (request.state == EOwnedRequestState::FREE ||
                request.state == EOwnedRequestState::IN_FLIGHT ||
                request.state == EOwnedRequestState::READY_SUCCESS)
            {
                continue;
            }
            ++impl_->snapshot.cancelled_requests;
            impl_->release(request);
        }
    }

    bool Spatial3DNavigationAdapterSystem::closeComplete() const noexcept
    {
        const auto state = snapshot();
        return state.closing && state.current_requests == 0u &&
               state.current_completions == 0u &&
               state.waiting_admission_requests == 0u &&
               state.in_flight_requests == 0u && state.current_bytes == 0u;
    }

    bool Spatial3DNavigationAdapterSystem::closeNeedsOwnerTick() const noexcept
    {
        if (!impl_->closing || closeComplete())
            return false;
        return std::ranges::any_of(
            impl_->requests,
            [](const OwnedRequest& request) noexcept
            {
                return request.state == EOwnedRequestState::READY_SUCCESS ||
                       request.state == EOwnedRequestState::READY_FAILURE ||
                       request.state ==
                           EOwnedRequestState::WAITING_ADMISSION;
            });
    }

    void Spatial3DNavigationAdapterSystem::acceptPreparation(
        std::uint32_t slot,
        std::uint32_t slot_generation,
        lux::exec::AsyncOutcome<BuildNavigationRegion3D> outcome) noexcept
    {
        auto* request = impl_->find(slot, slot_generation);
        if (!request || request->state != EOwnedRequestState::IN_FLIGHT)
        {
            ++impl_->snapshot.stale_completions;
            return;
        }
        if (!outcome)
        {
            if (impl_->closing)
            {
                ++impl_->snapshot.cancelled_requests;
                impl_->release(*request);
                return;
            }
            if (outcome.error().isRuntime())
            {
                const auto error = outcome.error().runtimeError();
                if (error == lux::exec::EAsyncSubmitError::QUEUE_FULL ||
                    error ==
                        lux::exec::EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED)
                {
                    request->state = EOwnedRequestState::WAITING_ADMISSION;
                    ++impl_->snapshot.queue_backpressure;
                    return;
                }
                impl_->readyFailure(
                    *request,
                    invalidContent("navigation background operation stopped"));
                return;
            }
            impl_->readyFailure(*request,
                                std::move(outcome.error().domainError()));
            return;
        }
        if (!outcome->valid())
        {
            impl_->readyFailure(
                *request,
                lux::navigation::detour3d::NavigationRegion3DFailure{
                    lux::navigation::detour3d::ENavigationRegion3DError::
                        STALE_GENERATION,
                    "navigation preparation generation is stale"});
            return;
        }
        // A generation-stale or close-late success still owns backend
        // layers. Preserve it for deliverReady(), where Navigation3DSystem
        // consumes it into its bounded discard retirement queue.
        request->failure.reset();
        request->prepared.emplace(std::move(*outcome));
        request->state = EOwnedRequestState::READY_SUCCESS;
        ++impl_->snapshot.current_completions;
    }

    void Spatial3DNavigationAdapterSystem::acceptPreparationStopped(
        std::uint32_t slot, std::uint32_t slot_generation) noexcept
    {
        auto* request = impl_->find(slot, slot_generation);
        if (!request || request->state != EOwnedRequestState::IN_FLIGHT)
        {
            ++impl_->snapshot.stale_completions;
            return;
        }
        if (impl_->closing)
        {
            ++impl_->snapshot.cancelled_requests;
            impl_->release(*request);
            return;
        }
        impl_->readyFailure(
            *request, invalidContent("navigation preparation was stopped"));
    }
} // namespace lux::runtime::spatial3d
