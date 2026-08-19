#include <lux/engine/ecs/pixel/systems/PixelFieldSystem.hpp>

#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/pixel/components/PixelField2DComponent.hpp>
#include <lux/engine/ecs/pixel/components/PixelFieldBindingComponent.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>

#include <cmath>
#include <cstdlib>

namespace lux::ecs
{
    struct PixelFieldSystem::Impl final
    {
        Impl(
            PixelFieldRuntime& runtime_value,
            PersistentEntityIndex& persistent_value) noexcept
            : runtime(&runtime_value), persistent(&persistent_value)
        {}

        void enqueue(entt::entity entity) noexcept
        {
            if (!commands.push(PixelFieldIntentCommand{
                    EPixelFieldIntentAction::RECONCILE_ENTITY,
                    entity,
                    {}}))
            {
                ++snapshot.command_rejections;
                return;
            }
            ++snapshot.intents_enqueued;
        }

        void onFieldConstructed(
            lux::meta::EntityRegistryBase&,
            entt::entity entity) noexcept
        {
            enqueue(entity);
        }

        void onFieldUpdated(
            lux::meta::EntityRegistryBase&,
            entt::entity entity) noexcept
        {
            enqueue(entity);
        }

        void onFieldDestroyed(
            lux::meta::EntityRegistryBase&,
            entt::entity entity) noexcept
        {
            enqueue(entity);
        }

        void onBindingDestroyed(
            lux::meta::EntityRegistryBase& registry,
            entt::entity entity) noexcept
        {
            // EnTT on_destroy fires while the component is still readable.
            // Destroying the whole entity removes the binding before the
            // deferred field-intent command can inspect it, so ownership must
            // be captured here. Explicit leaf bindings are never destroyed.
            if (const auto* binding =
                    registry.try_get<PixelFieldBindingComponent>(entity);
                binding && binding->owned_by_system)
            {
                if (!commands.push(PixelFieldIntentCommand{
                        EPixelFieldIntentAction::DESTROY_OWNED_HANDLE,
                        entt::null,
                        binding->field}))
                    ++snapshot.command_rejections;
                else
                    ++snapshot.intents_enqueued;
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
            constructed = registry.on_construct<PixelField2DComponent>()
                .connect<&Impl::onFieldConstructed>(*this);
            updated = registry.on_update<PixelField2DComponent>()
                .connect<&Impl::onFieldUpdated>(*this);
            destroyed = registry.on_destroy<PixelField2DComponent>()
                .connect<&Impl::onFieldDestroyed>(*this);
            binding_destroyed =
                registry.on_destroy<PixelFieldBindingComponent>()
                    .connect<&Impl::onBindingDestroyed>(*this);

            // Fold existing facts after connecting so setup order is not an
            // implicit caller contract. Existing explicit leaf bindings are
            // adopted rather than recreated by applyIntent().
            for (const auto entity :
                 registry.view<const PixelField2DComponent>())
            {
                if (const auto* binding =
                        registry.try_get<PixelFieldBindingComponent>(entity);
                    binding && binding->owned_by_system)
                {
                    ++snapshot.live_owned_bindings;
                }
                enqueue(entity);
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

        PixelFieldRuntime* runtime{nullptr};
        PersistentEntityIndex* persistent{nullptr};
        lux::meta::EntityRegistryBase* attached{nullptr};
        EcsCommandWriter commands;
        entt::scoped_connection constructed;
        entt::scoped_connection updated;
        entt::scoped_connection destroyed;
        entt::scoped_connection binding_destroyed;
        bool closing{false};
        PixelFieldSystemSnapshot snapshot;
    };

    PixelFieldSystem::PixelFieldSystem(
        PixelFieldRuntime& runtime,
        PersistentEntityIndex& persistent_entities)
        : impl_(std::make_unique<Impl>(runtime, persistent_entities))
    {}

    PixelFieldSystem::~PixelFieldSystem() = default;

    void PixelFieldSystem::onAdded(const SystemSetupContext& setup)
    {
        impl_->attach(setup.registry(), setup.commands());
    }

    void PixelFieldSystem::applyIntent(
        lux::meta::EntityRegistry& registry,
        entt::entity entity) noexcept
    {
        if (impl_->attached != &registry)
            std::abort();
        ++impl_->snapshot.commands_applied;

        const auto* component =
            registry.try_get<PixelField2DComponent>(entity);
        auto* binding =
            registry.try_get<PixelFieldBindingComponent>(entity);
        if (impl_->closing)
        {
            // Closing stops creation, but it does not erase a live field's
            // binding ahead of the Section deactivation that owns the field
            // fact.  Consumers can therefore retire while their provider is
            // still valid.  Once the authored component is gone, the normal
            // observer/deferred-command chain removes the binding here and
            // its on_destroy queues handle retirement for the next barrier.
            if (!component && binding && binding->owned_by_system)
                registry.remove<PixelFieldBindingComponent>(entity);
            return;
        }
        if (!component)
        {
            if (binding)
                registry.remove<PixelFieldBindingComponent>(entity);
            return;
        }

        if (!binding)
        {
            PixelFieldDesc description;
            description.extent = EPixelFieldExtent::INFINITE_FIELD;
            const auto handle = impl_->runtime->create(description);
            if (!handle.isValid())
                return;
            registry.emplace<PixelFieldBindingComponent>(
                entity,
                PixelFieldBindingComponent{handle, true});
            ++impl_->snapshot.create_commands;
            ++impl_->snapshot.live_owned_bindings;
            return;
        }

        // Invalid leaf handles fail closed. System-owned handles can be
        // reconstructed; explicit content-owned handles remain the leaf's
        // responsibility and are never silently replaced.
        if (!impl_->runtime->isAlive(binding->field) &&
            binding->owned_by_system)
        {
            PixelFieldDesc description;
            description.extent = EPixelFieldExtent::INFINITE_FIELD;
            const auto handle = impl_->runtime->create(description);
            if (!handle.isValid())
                return;
            registry.patch<PixelFieldBindingComponent>(
                entity,
                [handle](PixelFieldBindingComponent& value) noexcept
                {
                    value.field = handle;
                });
        }
        ++impl_->snapshot.update_commands;
    }

    void PixelFieldSystem::update(const SystemUpdateContext& context)
    {
        if (impl_->attached != &context.registry())
            std::abort();
        auto& registry = context.registry();
        impl_->snapshot.frame_fields_visited = 0u;
        impl_->snapshot.frame_updates = 0u;
        if (impl_->closing)
            return;
        registry.view<
            const PixelField2DComponent,
            const PixelFieldBindingComponent,
            const ResolvedTransform2DComponent>().each(
            [this](
                const PixelField2DComponent& component,
                const PixelFieldBindingComponent& binding,
                const ResolvedTransform2DComponent& transform)
            {
                ++impl_->snapshot.frame_fields_visited;
                if (!(component.cell_size > 0.0) ||
                    !std::isfinite(component.cell_size))
                {
                    return;
                }
                const auto cell_size = static_cast<float>(
                    component.cell_size);
                if (!std::isfinite(cell_size) || !(cell_size > 0.0f))
                    return;
                if (impl_->runtime->updateFrame(
                        binding.field,
                        PixelFieldFrame{transform.position, cell_size},
                        static_cast<float>(component.draw_priority),
                        component.visible,
                        component.simulation_enabled))
                {
                    ++impl_->snapshot.frame_updates;
                }
            });
    }

    void PixelFieldSystem::requestClose() noexcept
    {
        if (impl_->closing)
            return;
        impl_->closing = true;
        impl_->snapshot.closing = true;
    }

    bool PixelFieldSystem::closeComplete() const noexcept
    {
        return impl_->closing &&
            impl_->snapshot.intents_enqueued ==
                impl_->snapshot.commands_applied &&
            impl_->snapshot.live_owned_bindings == 0u;
    }

    bool PixelFieldSystem::closeNeedsOwnerTick() const noexcept
    {
        return impl_->closing && !closeComplete();
    }

    PixelFieldSystemSnapshot PixelFieldSystem::snapshot() const noexcept
    {
        auto result = impl_->snapshot;
        result.closed = closeComplete();
        return result;
    }

    PixelFieldHandle PixelFieldSystem::resolveField(
        const PersistentEntityRef& reference)
        const noexcept
    {
        if (impl_->closing || !impl_->attached || !reference.valid())
            return {};
        const auto entity = impl_->persistent->find(reference.id);
        if (entity == entt::null)
            return {};
        const auto* binding =
            impl_->attached->try_get<PixelFieldBindingComponent>(entity);
        return binding && impl_->runtime->isAlive(binding->field)
            ? binding->field
            : PixelFieldHandle{};
    }

    void PixelFieldSystem::releaseOwned(PixelFieldHandle handle) noexcept
    {
        impl_->runtime->destroy(handle);
        ++impl_->snapshot.commands_applied;
        ++impl_->snapshot.destroy_commands;
        if (impl_->snapshot.live_owned_bindings == 0u)
            std::abort();
        --impl_->snapshot.live_owned_bindings;
    }

    void PixelFieldIntentCommand::prepareRegistryPublication(
        lux::meta::EntityRegistry& registry) const noexcept
    {
        if (action == EPixelFieldIntentAction::RECONCILE_ENTITY)
        {
            reserveEcsCommandStorage(
                registry.storage<PixelFieldBindingComponent>(), 1u);
        }
    }

}
