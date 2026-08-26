#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>
#include <lux/engine/simulation/ecs/core/detail/EcsStateAccess.hpp>
#include <lux/engine/simulation/ecs/schema/ComponentOperationsAccess.hpp>
#include <lux/engine/simulation/ecs/snapshot/detail/ComponentSnapshotSetAccess.hpp>

#include <entt/core/type_info.hpp>
#include <entt/entity/entity.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace lux::simulation::ecs
{
    struct detail::EcsSnapshotAccess final
    {
        [[nodiscard]] static auto& registry(EcsState& world) noexcept
        {
            return world.registry_;
        }

        [[nodiscard]] static const auto& registry(const EcsState& world) noexcept
        {
            return world.registry_;
        }
    };

    struct EcsSnapshot::Impl final
    {
        ComponentSnapshotSet components;
        std::unique_ptr<EcsState> shadow;
    };

    EcsSnapshot::EcsSnapshot() noexcept = default;

    namespace
    {
        [[nodiscard]] lux::cxx::expected<void, SnapshotError>
        validateStorages(
            const EcsState& source,
            const ComponentSnapshotSet& components
        ) noexcept
        {
            const std::uint64_t entity_storage = entt::type_hash<Entity>::value();
            const auto& registry = detail::EcsSnapshotAccess::registry(source);
            const auto& schemas =
                detail::ComponentSnapshotSetAccess::schemas(components);
            for (auto&& [storage_id, storage] : registry.storage())
            {
                if (storage_id == entity_storage || storage.empty())
                    continue;

                const auto iterator = std::find_if(
                    schemas.all().begin(), schemas.all().end(),
                    [storage_id](const ComponentSchema& schema)
                    {
                        return detail::ComponentOperationsAccess::storageKey(
                            schema.operations
                        ) == storage_id;
                    }
                );
                if (iterator == schemas.all().end())
                {
                    return lux::cxx::unexpected(
                        SnapshotError{
                            ESnapshotError::UNKNOWN_COMPONENT_STORAGE,
                            storage_id}
                    );
                }
                if (iterator->snapshot == EComponentSnapshotPolicy::COPY &&
                    detail::ComponentSnapshotSetAccess::findStorage(
                        components,
                        storage_id
                    ) == nullptr)
                {
                    return lux::cxx::unexpected(
                        SnapshotError{
                            ESnapshotError::INVALID_COPY_SCHEMA,
                            storage_id,
                            iterator->id}
                    );
                }
            }
            return {};
        }

        void cloneEntities(const EcsState& source, EcsState& target)
        {
            const auto* source_entities =
                detail::EcsSnapshotAccess::registry(source).storage<Entity>();
            detail::require(source_entities != nullptr);
            auto& target_entities =
                detail::EcsSnapshotAccess::registry(target).storage<Entity>();

            target_entities.reserve(source_entities->size());
            Entity placeholder = NullEntity;
            for (auto iterator = source_entities->rbegin();
                 iterator != source_entities->rend(); ++iterator)
            {
                target_entities.generate(*iterator);
                if (*iterator > placeholder)
                    placeholder = *iterator;
            }
            target_entities.start_from(entt::entt_traits<Entity>::next(placeholder));
            target_entities.free_list(source_entities->free_list());
        }

        [[nodiscard]] lux::cxx::expected<void, SnapshotError>
        cloneWorld(
            const EcsState& source,
            EcsState& target,
            const ComponentSnapshotSet& components
        ) noexcept
        {
            try
            {
                auto edit = detail::EcsColdAccess::mutation(target);
                cloneEntities(source, target);
                const auto& registry =
                    detail::EcsSnapshotAccess::registry(source);
                const std::uint64_t entity_storage =
                    entt::type_hash<Entity>::value();
                for (auto&& [storage_id, storage] : registry.storage())
                {
                    if (storage_id == entity_storage || storage.empty())
                        continue;
                    const ComponentSnapshotBinding* binding =
                        detail::ComponentSnapshotSetAccess::findStorage(
                            components,
                            storage_id
                        );
                    if (binding == nullptr)
                        continue;
                    detail::ComponentSnapshotSetAccess::clone(
                        *binding,
                        source,
                        edit
                    );
                }
                return {};
            }
            catch (...)
            {
                return lux::cxx::unexpected(
                    SnapshotError{ESnapshotError::ALLOCATION_FAILURE}
                );
            }
        }
    } // namespace

    EcsSnapshot::EcsSnapshot(std::unique_ptr<Impl> impl) noexcept
        : impl_(std::move(impl))
    {
    }

    EcsSnapshot::EcsSnapshot(EcsSnapshot&&) noexcept = default;
    EcsSnapshot& EcsSnapshot::operator=(EcsSnapshot&&) noexcept = default;
    EcsSnapshot::~EcsSnapshot() noexcept = default;

    lux::cxx::expected<EcsSnapshot, SnapshotError> EcsSnapshot::capture(
        const EcsState& world,
        const ComponentSnapshotSet& components
    ) noexcept
    {
        if (!detail::EcsColdAccess::ownerIdle(world))
        {
            return lux::cxx::unexpected(
                SnapshotError{ESnapshotError::WORLD_BUSY}
            );
        }
        if (auto validation = validateStorages(world, components); !validation)
            return lux::cxx::unexpected(validation.error());

        try
        {
            auto impl = std::make_unique<Impl>();
            impl->components = components;
            impl->shadow = std::make_unique<EcsState>();
            if (auto cloned = cloneWorld(
                    world,
                    *impl->shadow,
                    components
                ); !cloned)
                return lux::cxx::unexpected(cloned.error());
            return EcsSnapshot(std::move(impl));
        }
        catch (...)
        {
            return lux::cxx::unexpected(
                SnapshotError{ESnapshotError::ALLOCATION_FAILURE}
            );
        }
    }

    lux::cxx::expected<std::unique_ptr<EcsState>, SnapshotError>
    EcsSnapshot::instantiate() const noexcept
    {
        if (!impl_ || !impl_->shadow)
            return lux::cxx::unexpected(SnapshotError{ESnapshotError::INVALID_COPY_SCHEMA});

        try
        {
            auto result = std::make_unique<EcsState>();
            if (auto cloned = cloneWorld(
                    *impl_->shadow,
                    *result,
                    impl_->components
                ); !cloned)
                return lux::cxx::unexpected(cloned.error());
            return result;
        }
        catch (...)
        {
            return lux::cxx::unexpected(SnapshotError{ESnapshotError::ALLOCATION_FAILURE});
        }
    }

    lux::cxx::expected<void, SnapshotError> EcsSnapshot::restore(
        EcsState& world
    ) const noexcept
    {
        if (!detail::EcsColdAccess::ownerIdle(world))
            return lux::cxx::unexpected(SnapshotError{ESnapshotError::WORLD_BUSY});

        auto replacement = instantiate();
        if (!replacement)
            return lux::cxx::unexpected(replacement.error());

        world.registry_.swap((*replacement)->registry_);
        return {};
    }

    void EcsSnapshot::clear() noexcept
    {
        impl_.reset();
    }

    bool EcsSnapshot::empty() const noexcept
    {
        return !impl_ || !impl_->shadow;
    }
} // namespace lux::simulation::ecs
