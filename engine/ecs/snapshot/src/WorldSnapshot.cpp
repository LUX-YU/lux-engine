#include <lux/engine/ecs/WorldSnapshot.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/ecs/schema/ComponentOperationsAccess.hpp>

#include <entt/core/type_info.hpp>
#include <entt/entity/entity.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace lux::ecs
{
    struct detail::WorldSnapshotAccess final
    {
        [[nodiscard]] static auto& registry(World& world) noexcept
        {
            return world.registry_;
        }

        [[nodiscard]] static const auto& registry(const World& world) noexcept
        {
            return world.registry_;
        }
    };

    struct WorldSnapshot::Impl final
    {
        ComponentSchemaSet schemas;
        std::unique_ptr<World> shadow;
    };

    namespace
    {
        [[nodiscard]] lux::cxx::expected<void, SnapshotError>
        validateStorages(
            const World& source,
            const ComponentSchemaSet& schemas
        ) noexcept
        {
            const std::uint64_t entity_storage = entt::type_hash<Entity>::value();
            const auto& registry = detail::WorldSnapshotAccess::registry(source);
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
            }
            return {};
        }

        void cloneEntities(const World& source, World& target)
        {
            const auto* source_entities =
                detail::WorldSnapshotAccess::registry(source).storage<Entity>();
            detail::require(source_entities != nullptr);
            auto& target_entities =
                detail::WorldSnapshotAccess::registry(target).storage<Entity>();

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
            const World& source,
            World& target,
            const ComponentSchemaSet& schemas
        ) noexcept
        {
            try
            {
                auto edit = detail::WorldColdAccess::suppressingEdit(target);
                cloneEntities(source, target);
                const auto* entities =
                    detail::WorldSnapshotAccess::registry(source).storage<Entity>();

                for (const ComponentSchema& schema : schemas.all())
                {
                    if (schema.snapshot == EComponentSnapshotPolicy::REBUILD)
                        continue;
                    if (!schema.operations.cloneable())
                    {
                        return lux::cxx::unexpected(
                            SnapshotError{
                                ESnapshotError::INVALID_COPY_SCHEMA,
                                detail::ComponentOperationsAccess::storageKey(
                                    schema.operations
                                ),
                                schema.id}
                        );
                    }

                    schema.operations.reserve(
                        edit,
                        schema.operations.size(source)
                    );
                    for (auto [entity] : entities->each())
                    {
                        if (schema.operations.has(source, entity))
                        {
                            schema.operations.clone(
                                source,
                                entity,
                                edit,
                                entity
                            );
                        }
                    }
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

    WorldSnapshot::WorldSnapshot(std::unique_ptr<Impl> impl) noexcept
        : impl_(std::move(impl))
    {
    }

    WorldSnapshot::WorldSnapshot(WorldSnapshot&&) noexcept = default;
    WorldSnapshot& WorldSnapshot::operator=(WorldSnapshot&&) noexcept = default;
    WorldSnapshot::~WorldSnapshot() noexcept = default;

    lux::cxx::expected<WorldSnapshot, SnapshotError> WorldSnapshot::capture(
        const World& world,
        const ComponentSchemaSet& schemas
    ) noexcept
    {
        if (!detail::WorldColdAccess::ownerIdle(world))
        {
            return lux::cxx::unexpected(
                SnapshotError{ESnapshotError::WORLD_BUSY}
            );
        }
        if (auto validation = validateStorages(world, schemas); !validation)
            return lux::cxx::unexpected(validation.error());

        try
        {
            auto impl = std::make_unique<Impl>();
            impl->schemas = schemas;
            impl->shadow = std::make_unique<World>();
            if (auto cloned = cloneWorld(world, *impl->shadow, schemas); !cloned)
                return lux::cxx::unexpected(cloned.error());
            detail::establishWorldChangeBaseline(*impl->shadow);
            return WorldSnapshot(std::move(impl));
        }
        catch (...)
        {
            return lux::cxx::unexpected(
                SnapshotError{ESnapshotError::ALLOCATION_FAILURE}
            );
        }
    }

    lux::cxx::expected<std::unique_ptr<World>, SnapshotError>
    WorldSnapshot::instantiate(WorldConfig config) const noexcept
    {
        if (!impl_ || !impl_->shadow)
            return lux::cxx::unexpected(SnapshotError{ESnapshotError::INVALID_COPY_SCHEMA});

        try
        {
            auto result = std::make_unique<World>(config);
            if (auto cloned = cloneWorld(*impl_->shadow, *result, impl_->schemas); !cloned)
                return lux::cxx::unexpected(cloned.error());
            detail::establishWorldChangeBaseline(*result);
            return result;
        }
        catch (...)
        {
            return lux::cxx::unexpected(SnapshotError{ESnapshotError::ALLOCATION_FAILURE});
        }
    }

    lux::cxx::expected<void, SnapshotError> WorldSnapshot::restore(
        World& world
    ) const noexcept
    {
        if (!detail::WorldColdAccess::ownerIdle(world) ||
            world.schedule_ != nullptr)
            return lux::cxx::unexpected(SnapshotError{ESnapshotError::WORLD_BUSY});

        auto replacement = instantiate(world.config_);
        if (!replacement)
            return lux::cxx::unexpected(replacement.error());

        world.registry_.swap((*replacement)->registry_);
        detail::establishWorldChangeBaseline(world);
        return {};
    }

    void WorldSnapshot::clear() noexcept
    {
        impl_.reset();
    }

    bool WorldSnapshot::empty() const noexcept
    {
        return !impl_ || !impl_->shadow;
    }
} // namespace lux::ecs
