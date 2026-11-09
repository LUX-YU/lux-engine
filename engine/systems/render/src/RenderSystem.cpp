#include <lux/engine/render/commands/Frame.hpp>
#include <lux/engine/systems/render/RenderFrameSubmission.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/world/World.hpp>
#include <utility>

namespace lux::engine::render
{
    struct RenderSystem::Impl final
    {
        ~Impl() noexcept
        {
            while (!collectors.empty())
            {
                collectors.pop_back();
            }
            while (!reactions.empty())
            {
                reactions.pop_back();
            }
        }

        world::World& world;
        EngineContext& context;
        const RenderCapabilityMetaRegistry& metadata;
        lux::render::CapabilityComposition composition;
        lux::render::EffectOutputDesc output;
        lux::render::SceneId scene;
        lux::render::EffectId effect;
        std::vector<std::unique_ptr<RenderReaction>> reactions;
        std::vector<std::unique_ptr<RenderDataCollector>> collectors;
    };

    RenderSystem::RenderSystem(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    RenderSystem::~RenderSystem() noexcept = default;

    world::World& RenderSystem::world() noexcept
    {
        return impl_->world;
    }

    EngineContext& RenderSystem::context() noexcept
    {
        return impl_->context;
    }

    lux::render::SceneId RenderSystem::sceneId() const noexcept
    {
        return impl_->scene;
    }

    lux::render::EffectId RenderSystem::effectId() const noexcept
    {
        return impl_->effect;
    }

    RenderSystem::Result RenderSystem::create(
        lux::render::CapabilityComposition composition,
        lux::render::EffectOutputDesc output,
        world::World& world,
        EngineContext& context,
        const RenderCapabilityMetaRegistry& metadata
    ) noexcept
    {
        if (!context.execution().isMainThread())
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kRuntimeState});
        }
        auto impl = std::make_unique<Impl>(world, context, metadata, std::move(composition), output);
        return std::unique_ptr<RenderSystem>{new RenderSystem(std::move(impl))};
    }

    exec::task<scene::SceneSystem::Result> RenderSystem::initialize() noexcept
    {
        auto& owner = *impl_;
        const auto& metadata = owner.metadata;
        auto& client = context().renderClient();
        std::vector<std::pair<const RenderCapabilityMeta*, lux::render::CapabilityImplementationId>> behaviors;
        behaviors.reserve(owner.composition.contributions.size());
        for (const auto& contribution : owner.composition.contributions)
        {
            const auto* entry = metadata.find(contribution.capability);
            if (!entry)
            {
                co_return cxx::unexpected(lux::render::RenderError{
                    lux::render::kCapabilityDeclaration, {contribution.capability.hash()}
                });
            }
            behaviors.emplace_back(entry, contribution.implementation_id);
        }
        if (taskScope().stopToken().stop_requested())
        {
            co_return SceneSystem::Result{};
        }
        lux::render::CreateEffectRequest request{owner.output, std::exchange(owner.composition, {})};
        auto compiled = co_await client.request<lux::render::CreateEffect>(std::move(request));
        if (compiled.error.type)
        {
            co_return cxx::unexpected(compiled.error);
        }
        owner.effect = compiled.effect;
        if (taskScope().stopToken().stop_requested())
        {
            co_return SceneSystem::Result{};
        }
        auto created_scene = co_await client.request<lux::render::CreateScene>({});
        if (created_scene.error.type)
        {
            co_return cxx::unexpected(created_scene.error);
        }
        owner.scene = created_scene.scene;
        if (taskScope().stopToken().stop_requested())
        {
            co_return SceneSystem::Result{};
        }
        for (const auto& [type, construct] : metadata.sceneReactions())
        {
            auto created = construct(*this);
            if (!created)
            {
                co_return cxx::unexpected(created.error());
            }
            owner.reactions.push_back(std::move(*created));
        }
        lux::render::FrameDataDestination destination{owner.effect, 0, 0, owner.scene};
        for (const auto& [entry, implementation] : behaviors)
        {
            for (const auto& reaction : entry->reactions)
            {
                auto created = reaction.construct(*this, implementation);
                if (!created)
                {
                    co_return cxx::unexpected(created.error());
                }
                owner.reactions.push_back(std::move(*created));
            }
            for (const auto& collector : entry->collectors)
            {
                auto created = collector.construct(*this, destination);
                if (!created)
                {
                    co_return cxx::unexpected(created.error());
                }
                owner.collectors.push_back(std::move(*created));
            }
            ++destination.capability;
        }
        co_return SceneSystem::Result{};
    }

    cxx::expected<void, error::Error> RenderSystem::execute() noexcept
    {
        for (auto& reaction : impl_->reactions)
        {
            if (auto processed = reaction->process(); !processed)
            {
                return processed;
            }
        }
        auto frame = RenderFrameSubmission::instance().writer();
        auto completion = frame.allocate<object::ObjectId>(1);
        if (!completion)
        {
            return cxx::unexpected(completion.error());
        }
        completion->values.front() = objectId();
        frame.onCompletion(
            completion->values.data(),
            +[](void* value, const lux::render::FrameCompletion& result) noexcept
            {
                if (result.error.type == 0 && !result.requires_resynchronization)
                {
                    return;
                }
                auto target = object::ObjectRuntime::instance().resolve(*static_cast<object::ObjectId*>(value));
                if (target)
                {
                    auto& system = *static_cast<RenderSystem*>(*target);
                    for (auto& collector : system.impl_->collectors)
                    {
                        collector->invalidate();
                    }
                }
            }
        );
        for (auto& collector : impl_->collectors)
        {
            auto collected = collector->collect(frame);
            if (!collected)
            {
                return collected;
            }
        }
        return {};
    }

    exec::task<scene::SceneSystem::Result> RenderSystem::releaseResources() noexcept
    {
        auto consumed = co_await RenderFrameSubmission::instance().whenSubmittedFramesConsumed();
        if (!consumed)
        {
            co_return consumed;
        }
        while (!impl_->collectors.empty())
        {
            impl_->collectors.pop_back();
        }
        while (!impl_->reactions.empty())
        {
            impl_->reactions.back()->beginDestruction();
            auto released = co_await impl_->reactions.back()->releaseResources();
            if (!released)
            {
                co_return released;
            }
            impl_->reactions.pop_back();
        }
        auto& client = context().renderClient();
        if (!impl_->scene.isNull())
        {
            auto removed = co_await client.request<lux::render::RemoveScene>({impl_->scene});
            if (removed.error.type)
            {
                co_return cxx::unexpected(removed.error);
            }
            impl_->scene = {};
        }
        if (!impl_->effect.isNull())
        {
            auto removed = co_await client.request<lux::render::RemoveEffect>({impl_->effect});
            if (removed.error.type)
            {
                co_return cxx::unexpected(removed.error);
            }
            impl_->effect = {};
        }
        co_return SceneSystem::Result{};
    }
} // namespace lux::engine::render
