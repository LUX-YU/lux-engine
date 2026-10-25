#include <lux/engine/context/render/RenderDiagnostics.hpp>
#include <lux/engine/render/capabilities/RasterAssets.hpp>
#include <lux/engine/scene/render/DirectLightBinding.hpp>
#include <lux/engine/scene/render/DirectLightValues.hpp>

namespace lux::scene
{
    using namespace lux::render;
    using namespace engine::render;
    using namespace simulation::ecs;

    std::span<const RenderPayloadContract> DirectLightBinding::inputs() noexcept
    {
        static const auto contracts = []() noexcept
        {
            RenderPayloadContract input;
            input.schema = kDirectLightsInput;
            input.use = ERenderPayloadUse::RESOURCE_INPUT;
            input.lifetime = ERenderPayloadLifetime::UNTIL_RESPONSE;
            input.setCppType(cxx::typeToken<CreateDirectLights::Request>());
            return std::array{input};
        }();
        return contracts;
    }

    RenderResult<std::unique_ptr<DirectLightBinding>> DirectLightBinding::create(RenderBindingContext& context) noexcept
    {
        const auto input = context.input(kDirectLightsInput);
        auto create = context.client.bind<CreateDirectLights>();
        auto remove = context.client.bind<RemoveDirectLight>();
        if (!input || !create || !remove)
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        return std::unique_ptr<DirectLightBinding>{new DirectLightBinding(context, *input, *create, *remove)};
    }

    DirectLightBinding::DirectLightBinding(
        RenderBindingContext& context,
        CapabilityInput input,
        BoundCommand<CreateDirectLights> create,
        BoundCommand<RemoveDirectLight> remove
    ) noexcept
        : registry_(context.registry), client_(context.client), execution_(context.execution), input_(input),
          create_(create), remove_(remove), revision_(context.input_revision),
          adopted_revision_(context.adopted_input_revision)
    {
    }

    DirectLightBinding::~DirectLightBinding() noexcept
    {
        if (pending_)
        {
            std::terminate(); // Scene stop settles producers before destroying behavior objects.
        }
    }

    exec::task<RenderResult<void>> DirectLightBinding::sendWork() noexcept
    {
        auto& work = *work_;
        for (; work.released < work.retired.size(); ++work.released)
        {
            RemoveDirectLight::Request request{work.retired[work.released]};
            const auto response = co_await client_.request(remove_, request);
            if (response.error.type != 0)
            {
                work.error = response.error;
                co_return RenderResult<void>{};
            }
        }
        if (!work.creations.empty())
        {
            CreateDirectLights::Request request{input_, 0, work.values};
            auto response = co_await client_.request(create_, request);
            work.error = response.error;
            work.created = std::move(response.lights);
            if (work.error.type == 0 && work.created.size() != work.creations.size())
            {
                work.error = RenderError{kCommandContract};
            }
        }
        co_return RenderResult<void>{};
    }

    void DirectLightBinding::startWork() noexcept
    {
        pending_ = true;
        auto work = execution::taskResult(stdexec::starts_on(execution_.mainScheduler(), sendWork()));
        const auto accepted = execution_.spawn(stdexec::then(
            std::move(work),
            [this](auto result) noexcept
            {
                if (!result)
                {
                    const auto* domain = result.error().domainFailure();
                    work_->error = domain ? *domain : RenderError{kRuntimeState};
                }
                pending_ = false;
                // Work keeps the result until maintain reaches its structural safe point.
            }
        ));
        if (!accepted)
        {
            std::terminate(); // The application keeps shared Execution open through Scene cleanup.
        }
    }

    void DirectLightBinding::adoptWork(bool closing) noexcept
    {
        auto& work = *work_;
        for (std::size_t i = work.released; i < work.retired.size(); ++i)
        {
            retired_.push_back({work.retired[i], 0});
        }
        const bool can_adopt = !closing && work.error.type == 0;
        for (std::size_t i = 0; i < work.created.size(); ++i)
        {
            if (!can_adopt)
            {
                retired_.push_back({work.created[i], 0});
                continue;
            }
            const auto creation = work.creations[i];
            const auto demand = demands_.find(creation.entity);
            const bool current = demand != demands_.end() && demand->second.serial == creation.demand;
            const bool available = current && registry_.valid(creation.entity) &&
                                   registry_.all_of<Light3D, WorldTransform3D>(creation.entity) &&
                                   registry_.get<Light3D>(creation.entity).value.type == creation.type;
            if (!available)
            {
                retired_.push_back({work.created[i], 0});
                continue;
            }
            registry_.remove<detail::LightRenderResourceComponent>(creation.entity);
            registry_.emplace<detail::LightRenderResourceComponent>(
                creation.entity,
                work.created[i],
                creation.type,
                detail::lightShadowInputs(registry_.get<Light3D>(creation.entity).value)
            );
            demands_.erase(demand);
            ++revision_;
        }
        const bool retryable = work.error.type == kDeviceBusy || work.error.type == kRuntimeCapacity ||
                               work.error.type == kCommandCancelled || work.error.type == kFrameResynchronization;
        if (work.error.type != 0 && !retryable)
        {
            reportRenderFailure(work.error, "Prepare direct light resources");
            if (work.released != work.retired.size())
            {
                failure_ = work.error; // An unsuccessful release is not proof of resource discharge.
            }
            for (const auto& creation : work.creations)
            {
                const auto found = demands_.find(creation.entity);
                if (found != demands_.end() && found->second.serial == creation.demand)
                {
                    demands_.erase(found); // A new author event retries; the old adopted relation remains valid.
                }
            }
        }
        work_.reset();
    }

    SceneStageResult DirectLightBinding::maintain(SceneStageContext& context) noexcept
    {
        const bool closing = context.stop.stop_requested();
        context.publication_needed |= work_.has_value() || !demands_.empty() || !retired_.empty();
        if (!context.allow_structure && !closing)
        {
            return ESceneProgress::COMPLETE;
        }
        if (work_ && !pending_)
        {
            adoptWork(closing);
        }
        if (closing)
        {
            if (pending_)
            {
                return ESceneProgress::PENDING;
            }
            registry_.clear<detail::LightRenderResourceComponent>();
            demands_.clear();
            // RenderSystem removes the owning Render Scene after every binding is quiescent.
            // That operation clears its resource scope; no post-RemoveScene release is manufactured.
            retired_.clear();
            return ESceneProgress::COMPLETE;
        }
        if (failure_.type != 0)
        {
            return cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, failure_});
        }
        if (pending_)
        {
            return ESceneProgress::COMPLETE;
        }
        if (demands_.empty() && retired_.empty())
        {
            return ESceneProgress::COMPLETE;
        }
        work_.emplace();
        for (auto it = demands_.begin(); it != demands_.end();)
        {
            const auto entity = it->first;
            const bool available = registry_.valid(entity) && registry_.all_of<Light3D, WorldTransform3D>(entity);
            if (!available)
            {
                if (registry_.valid(entity))
                {
                    registry_.remove<detail::LightRenderResourceComponent>(entity);
                }
                it = demands_.erase(it);
                continue;
            }
            const auto& light = registry_.get<Light3D>(entity).value;
            const auto* resource = registry_.try_get<detail::LightRenderResourceComponent>(entity);
            if (resource && resource->type == light.type && !it->second.revoked)
            {
                const auto shadows = detail::lightShadowInputs(light);
                if (resource->shadows != shadows)
                {
                    registry_.patch<detail::LightRenderResourceComponent>(
                        entity,
                        [&](auto& value) noexcept { value.shadows = shadows; }
                    );
                    ++revision_;
                }
                it = demands_.erase(it);
                continue;
            }
            auto value = detail::lightValue(light, registry_.get<WorldTransform3D>(entity).value);
            if (!value)
            {
                reportRenderFailure(value.error(), "Prepare direct light candidate");
                it = demands_.erase(it);
                continue;
            }
            work_->creations.push_back({entity, it->second.serial, light.type});
            work_->values.push_back(std::move(*value));
            ++it;
        }
        std::erase_if(
            retired_,
            [this](const Retirement& retired) noexcept
            {
                if (retired.after_revision > adopted_revision_)
                {
                    return false;
                }
                work_->retired.push_back(retired.light);
                return true;
            }
        );
        if (work_->creations.empty() && work_->retired.empty())
        {
            work_.reset();
        }
        else
        {
            startWork();
        }
        return ESceneProgress::COMPLETE;
    }
} // namespace lux::scene
