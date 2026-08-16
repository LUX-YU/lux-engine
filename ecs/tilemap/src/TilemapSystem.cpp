#include <lux/engine/ecs/tilemap/systems/TilemapSystem.hpp>

#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapBindingComponent.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapComponent.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>

#include <cstdlib>

namespace lux::ecs
{
    struct TilemapSystem::Impl final
    {
        Impl(
            TilemapRuntime& runtime_value,
            PersistentEntityIndex& persistent_value) noexcept
            : runtime(&runtime_value), persistent(&persistent_value)
        {}

        [[nodiscard]] bool enqueue(TilemapIntentCommand command) noexcept
        {
            if (!commands.push(command))
            {
                ++metrics.command_rejections;
                return false;
            }
            ++metrics.intents_enqueued;
            return true;
        }

        void reconcile(
            lux::meta::EntityRegistryBase&,
            entt::entity entity) noexcept
        {
            static_cast<void>(enqueue(TilemapIntentCommand{
                ETilemapIntentAction::RECONCILE, entity, {}}));
        }

        void bindingDestroyed(
            lux::meta::EntityRegistryBase& registry,
            entt::entity entity) noexcept
        {
            const auto* binding =
                registry.try_get<TilemapBindingComponent>(entity);
            if (binding && binding->owned_by_system)
            {
                static_cast<void>(enqueue(TilemapIntentCommand{
                    ETilemapIntentAction::DESTROY_OWNED_HANDLE,
                    entt::null,
                    binding->runtime}));
            }
        }

        void attach(
            lux::meta::EntityRegistryBase& registry,
            EcsCommandWriter writer)
        {
            if (attached || !persistent->boundTo(registry))
                std::abort();
            attached = &registry;
            commands = writer;
            constructed = registry.on_construct<TilemapComponent>()
                .connect<&Impl::reconcile>(*this);
            updated = registry.on_update<TilemapComponent>()
                .connect<&Impl::reconcile>(*this);
            destroyed = registry.on_destroy<TilemapComponent>()
                .connect<&Impl::reconcile>(*this);
            binding_destroyed =
                registry.on_destroy<TilemapBindingComponent>()
                    .connect<&Impl::bindingDestroyed>(*this);

            // Connect first, then fold existing facts. Setup order is not an
            // implicit caller contract.
            for (const auto entity : registry.view<const TilemapComponent>())
            {
                const auto* binding =
                    registry.try_get<TilemapBindingComponent>(entity);
                if (binding && binding->owned_by_system)
                    ++metrics.live_owned_bindings;
                reconcile(registry, entity);
            }
        }

        void detach() noexcept
        {
            constructed.release();
            updated.release();
            destroyed.release();
            binding_destroyed.release();
            attached = nullptr;
            commands = {};
        }

        TilemapRuntime* runtime{nullptr};
        PersistentEntityIndex* persistent{nullptr};
        lux::meta::EntityRegistryBase* attached{nullptr};
        EcsCommandWriter commands;
        entt::scoped_connection constructed;
        entt::scoped_connection updated;
        entt::scoped_connection destroyed;
        entt::scoped_connection binding_destroyed;
        TilemapSystemSnapshot metrics;
        bool closing{false};
    };

    TilemapSystem::TilemapSystem(
        TilemapRuntime& runtime,
        PersistentEntityIndex& persistent_entities)
        : impl_(std::make_unique<Impl>(runtime, persistent_entities))
    {}

    TilemapSystem::~TilemapSystem() = default;

    void TilemapSystem::onAdded(const SystemSetupContext& setup)
    {
        impl_->attach(setup.registry(), setup.commands());
    }

    void TilemapSystem::onRemoved(const SystemRemovalContext& removal)
    {
        if (!impl_->closing || !closeComplete())
            std::abort();
        if (impl_->attached == &removal.registry())
            impl_->detach();
    }

    void TilemapSystem::applyReconcile(
        lux::meta::EntityRegistry& registry,
        entt::entity entity) noexcept
    {
        if (impl_->attached != &registry)
            std::abort();
        ++impl_->metrics.commands_applied;

        const auto* component = registry.try_get<TilemapComponent>(entity);
        auto* binding = registry.try_get<TilemapBindingComponent>(entity);
        if (impl_->closing)
        {
            // Keep a live provider binding until Section deactivation removes
            // its authored fact. Dependent chunks retire first.
            if (!component && binding && binding->owned_by_system)
                registry.remove<TilemapBindingComponent>(entity);
            return;
        }
        if (!component)
        {
            if (binding && binding->owned_by_system)
                registry.remove<TilemapBindingComponent>(entity);
            return;
        }
        if (!binding)
        {
            const auto handle = impl_->runtime->create({component->id});
            if (!handle.isValid())
                return;
            registry.emplace<TilemapBindingComponent>(
                entity,
                TilemapBindingComponent{handle, true});
            ++impl_->metrics.created_bindings;
            ++impl_->metrics.live_owned_bindings;
            return;
        }
        if (!binding->owned_by_system)
            return;

        const auto description = impl_->runtime->desc(binding->runtime);
        if (!impl_->runtime->isAlive(binding->runtime) ||
            description.id != component->id)
        {
            const auto previous = binding->runtime;
            const auto replacement = impl_->runtime->create({component->id});
            if (!replacement.isValid())
                return;
            registry.patch<TilemapBindingComponent>(
                entity,
                [replacement](TilemapBindingComponent& value) noexcept
                {
                    value.runtime = replacement;
                });
            if (impl_->runtime->isAlive(previous))
                impl_->runtime->destroy(previous);
            ++impl_->metrics.destroyed_bindings;
        }
        ++impl_->metrics.updated_bindings;
    }

    void TilemapSystem::releaseOwned(TilemapHandle handle) noexcept
    {
        if (impl_->runtime->isAlive(handle))
            impl_->runtime->destroy(handle);
        ++impl_->metrics.commands_applied;
        ++impl_->metrics.destroyed_bindings;
        if (impl_->metrics.live_owned_bindings == 0u)
            std::abort();
        --impl_->metrics.live_owned_bindings;
    }

    void TilemapIntentCommand::prepareRegistryPublication(
        lux::meta::EntityRegistry& registry) const noexcept
    {
        if (action == ETilemapIntentAction::RECONCILE)
        {
            reserveEcsCommandStorage(
                registry.storage<TilemapBindingComponent>(), 1u);
        }
    }

    void TilemapSystem::requestClose() noexcept
    {
        if (impl_->closing)
            return;
        impl_->closing = true;
        impl_->metrics.closing = true;
    }

    bool TilemapSystem::closeComplete() const noexcept
    {
        return impl_->closing &&
            impl_->metrics.intents_enqueued ==
                impl_->metrics.commands_applied &&
            impl_->metrics.live_owned_bindings == 0u;
    }

    bool TilemapSystem::closeNeedsOwnerTick() const noexcept
    {
        return impl_->closing && !closeComplete();
    }

    TilemapSystemSnapshot TilemapSystem::snapshot() const noexcept
    {
        auto result = impl_->metrics;
        result.closed = closeComplete();
        return result;
    }

    TilemapHandle TilemapSystem::resolveTilemap(
        const lux::entity_scene::PersistentEntityRef& reference)
        const noexcept
    {
        if (impl_->closing || !impl_->attached || !reference.valid())
            return {};
        const auto entity = impl_->persistent->find(reference.id);
        if (entity == entt::null)
            return {};
        const auto* binding =
            impl_->attached->try_get<TilemapBindingComponent>(entity);
        return binding && impl_->runtime->isAlive(binding->runtime)
            ? binding->runtime
            : TilemapHandle{};
    }
} // namespace lux::ecs
