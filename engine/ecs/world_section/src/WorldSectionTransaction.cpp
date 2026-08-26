#include <lux/engine/ecs/WorldSectionTransaction.hpp>

#include <lux/engine/ecs/core/detail/EcsStateAccess.hpp>
#include <lux/engine/ecs/world_section/detail/ComponentLoadSerialization.hpp>
#include <lux/engine/ecs/world_section/detail/WorldSectionTransactionAccess.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace lux::ecs
{
    void ComponentLoadBinding::trackMembership(
        EcsMutation& mutation,
        std::uint64_t storage,
        std::span<const Entity> entities
    ) noexcept
    {
        detail::WorldSectionTransactionAccess::addComponentMembership(
            mutation,
            storage,
            entities
        );
    }

    WorldSectionInstance::WorldSectionInstance(
        WorldSectionInstance&& other
    ) noexcept
        : id_(std::move(other.id_)),
          entities_(std::move(other.entities_)),
          code_lifetimes_(std::move(other.code_lifetimes_)),
          world_identity_(std::exchange(other.world_identity_, 0U)),
          lease_(std::exchange(other.lease_, 0U)),
          state_(std::exchange(other.state_, EState::INACTIVE))
    {
        detail::require(state_ != EState::STAGED);
        other.id_ = {};
        other.entities_.clear();
        other.code_lifetimes_.clear();
    }

    WorldSectionInstance::~WorldSectionInstance() noexcept
    {
        detail::require(state_ == EState::INACTIVE);
    }

    namespace
    {
        [[nodiscard]] WorldSectionFailure failure(
            EWorldSectionError code,
            std::uint32_t column_index = 0U,
            ComponentSchemaId schema = {}
        )
        {
            WorldSectionFailure result;
            result.code = code;
            result.column_index = column_index;
            result.schema = std::move(schema);
            return result;
        }

        [[nodiscard]] EWorldSectionError mapSerializationError(
            lux::serialization::ESerializationError code
        ) noexcept
        {
            switch (code)
            {
            case lux::serialization::ESerializationError::LIMIT_EXCEEDED:
            case lux::serialization::ESerializationError::SIZE_OVERFLOW:
                return EWorldSectionError::LIMIT_EXCEEDED;
            case lux::serialization::ESerializationError::ALLOCATION_FAILURE:
                return EWorldSectionError::ALLOCATION_FAILURE;
            default:
                return EWorldSectionError::DECODE_FAILED;
            }
        }

        [[nodiscard]] WorldSectionFailure decodeFailure(
            const lux::serialization::SerializationFailure& source,
            std::uint32_t column_index,
            const ComponentSchemaId& schema
        )
        {
            WorldSectionFailure result = failure(
                mapSerializationError(source.code),
                column_index,
                schema
            );
            result.byte_offset = source.offset;
            return result;
        }

        [[nodiscard]] bool sameOwner(
            const std::shared_ptr<const void>& left,
            const std::shared_ptr<const void>& right
        ) noexcept
        {
            return !left.owner_before(right) && !right.owner_before(left);
        }

        void appendUniquePin(
            std::vector<std::shared_ptr<const void>>& pins,
            const std::shared_ptr<const void>& pin
        )
        {
            if (!pin)
                return;
            const auto found = std::find_if(
                pins.begin(),
                pins.end(),
                [&](const auto& existing) noexcept
                {
                    return sameOwner(existing, pin);
                }
            );
            if (found == pins.end())
                pins.push_back(pin);
        }

    } // namespace

    struct WorldSectionTransaction::Impl final
    {
        enum class EState : std::uint8_t
        {
            ACTIVE,
            FAILED,
            COMMITTED,
        };

        struct AdditionLane final
        {
            std::uint64_t storage{};
            bool dense{};
            std::vector<std::uint32_t> ordinals;
        };

        struct LoadOperation final
        {
            WorldSectionInstance* output{};
            std::vector<std::shared_ptr<const void>> pins;
            std::vector<AdditionLane> additions;
            detail::WorldSectionTransactionAccess::EntityAllocatorCheckpoint
                allocator_checkpoint{};
            bool membership_active{};
        };

        struct RemovalLane final
        {
            std::uint64_t storage{};
            std::vector<Entity> entities;
        };

        struct UnloadOperation final
        {
            WorldSectionInstance* instance{};
            std::vector<RemovalLane> removals;
        };

        EcsState* world{};
        EcsMutation mutation;
        WorldSectionLoadScratchBudget scratch{};
        lux::serialization::SerializationLimits limits{};
        std::vector<LoadOperation> loads;
        std::vector<UnloadOperation> unloads;
        std::vector<Entity> sparse_entities;
        EState state{EState::ACTIVE};

        static void resetInstance(WorldSectionInstance& instance) noexcept
        {
            instance.id_ = {};
            instance.entities_.clear();
            instance.code_lifetimes_.clear();
            instance.world_identity_ = 0U;
            instance.lease_ = 0U;
            instance.state_ = WorldSectionInstance::EState::INACTIVE;
        }

        void rollbackLoad(LoadOperation& operation) noexcept
        {
            auto& output = *operation.output;
            detail::WorldSectionTransactionAccess::rollbackEntities(
                mutation,
                output.lease_,
                output.entities_,
                operation.allocator_checkpoint,
                operation.membership_active
            );
            resetInstance(output);
        }

        void rollbackAll() noexcept
        {
            for (auto iterator = loads.rbegin(); iterator != loads.rend();
                 ++iterator)
            {
                rollbackLoad(*iterator);
            }
            for (auto& operation : unloads)
                operation.instance->state_ = WorldSectionInstance::EState::ACTIVE;
            loads.clear();
            unloads.clear();
        }

        [[nodiscard]] lux::cxx::expected<void, WorldSectionFailure> poison(
            WorldSectionFailure reason
        ) noexcept
        {
            state = EState::FAILED;
            return lux::cxx::unexpected(std::move(reason));
        }
    };

    WorldSectionTransaction::WorldSectionTransaction(
        std::unique_ptr<Impl> impl
    ) noexcept
        : impl_(std::move(impl))
    {
    }

    WorldSectionTransaction::WorldSectionTransaction(
        WorldSectionTransaction&& other
    ) noexcept = default;

    WorldSectionTransaction::~WorldSectionTransaction() noexcept
    {
        if (impl_ && impl_->state != Impl::EState::COMMITTED)
            impl_->rollbackAll();
    }

    lux::cxx::expected<void, WorldSectionFailure>
    WorldSectionTransaction::load(
        const ComponentLoadSet& load_set,
        const WorldSectionImage& image,
        WorldSectionInstance& inactive_output
    ) noexcept
    {
        if (!impl_ || impl_->state != Impl::EState::ACTIVE)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::TRANSACTION_FAILED)
            );
        }
        if (inactive_output.state_ != WorldSectionInstance::EState::INACTIVE)
        {
            return impl_->poison(
                failure(EWorldSectionError::INVALID_INSTANCE_STATE)
            );
        }

        struct ColumnPlan final
        {
            const ComponentLoadBinding* binding{};
        };

        Impl::LoadOperation operation;
        operation.output = &inactive_output;
        bool operation_stored = false;
        try
        {
            std::vector<ColumnPlan> plan;
            plan.reserve(image.columns().size());
            operation.additions.reserve(image.columns().size());
            std::size_t component_rows{};
            std::size_t max_sparse_rows{};

            for (std::size_t index{}; index < image.columns().size(); ++index)
            {
                const auto& column = image.columns()[index];
                const ComponentLoadBinding* binding = load_set.find(
                    column.schemaHash(),
                    column.schemaName()
                );
                if (binding == nullptr)
                {
                    return impl_->poison(failure(
                        EWorldSectionError::MISSING_BINDING,
                        static_cast<std::uint32_t>(index),
                        componentSchemaId(column.schemaName())
                    ));
                }
                if (binding->schema().version != column.schemaVersion() ||
                    binding->valueEncoding() != column.valueEncoding() ||
                    (column.valueEncoding() ==
                         EWorldSectionValueEncoding::FIXED &&
                     binding->fixedStride() != column.fixedStride()))
                {
                    return impl_->poison(failure(
                        EWorldSectionError::BINDING_MISMATCH,
                        static_cast<std::uint32_t>(index),
                        binding->schema().id
                    ));
                }
                if (binding->minimumDecodeScratch() >
                    impl_->scratch.decode_bytes)
                {
                    return impl_->poison(failure(
                        EWorldSectionError::LIMIT_EXCEEDED,
                        static_cast<std::uint32_t>(index),
                        binding->schema().id
                    ));
                }
                if (column.rowCount() >
                    std::numeric_limits<std::size_t>::max() - component_rows)
                {
                    return impl_->poison(
                        failure(EWorldSectionError::LIMIT_EXCEEDED)
                    );
                }
                component_rows += column.rowCount();
                appendUniquePin(
                    operation.pins,
                    load_set.codeLifetime(*binding)
                );
                appendUniquePin(
                    operation.pins,
                    binding->schema().code_lifetime
                );

                Impl::AdditionLane lane;
                lane.storage = binding->storage_;
                lane.dense = column.ordinalEncoding() ==
                    EWorldSectionOrdinalEncoding::DENSE;
                if (!lane.dense)
                {
                    lane.ordinals.reserve(column.rowCount());
                    for (std::size_t row{}; row < column.rowCount(); ++row)
                    {
                        lane.ordinals.push_back(detail::readColumnU32(
                            column.ordinalBytes(),
                            row * sizeof(std::uint32_t)
                        ));
                    }
                    max_sparse_rows = std::max(
                        max_sparse_rows,
                        lane.ordinals.size()
                    );
                }
                operation.additions.push_back(std::move(lane));
                plan.push_back(ColumnPlan{binding});
            }

            const std::size_t active_sections =
                detail::WorldColdAccess::activeSectionCount(*impl_->world);
            if (impl_->loads.size() ==
                std::numeric_limits<std::size_t>::max() ||
                active_sections >
                    std::numeric_limits<std::size_t>::max() -
                        (impl_->loads.size() + 1U))
            {
                return impl_->poison(
                    failure(EWorldSectionError::LIMIT_EXCEEDED)
                );
            }

            inactive_output.id_ = image.id();
            inactive_output.entities_.resize(image.entityCount(), NullEntity);
            inactive_output.world_identity_ =
                detail::WorldColdAccess::identity(*impl_->world);
            inactive_output.lease_ =
                detail::WorldSectionTransactionAccess::allocateLease(
                    impl_->mutation
                );

            operation.allocator_checkpoint =
                detail::WorldSectionTransactionAccess::checkpointAllocator(
                    impl_->mutation
                );
            impl_->loads.reserve(impl_->loads.size() + 1U);
            impl_->sparse_entities.reserve(max_sparse_rows);
            impl_->loads.push_back(std::move(operation));
            operation_stored = true;
            auto& stored = impl_->loads.back();

            detail::WorldSectionTransactionAccess::createEntities(
                impl_->mutation,
                inactive_output.entities_
            );
            detail::WorldSectionTransactionAccess::reserveMembership(
                impl_->mutation,
                inactive_output.entities_,
                component_rows
            );
            detail::WorldSectionTransactionAccess::activateMembership(
                impl_->mutation,
                inactive_output.lease_,
                inactive_output.entities_
            );
            stored.membership_active = true;

            for (std::size_t index{}; index < image.columns().size(); ++index)
            {
                const auto& column = image.columns()[index];
                if (column.rowCount() == 0U)
                    continue;

                std::span<const Entity> row_entities;
                const auto& lane = stored.additions[index];
                if (lane.dense)
                {
                    row_entities = inactive_output.entities_;
                }
                else
                {
                    impl_->sparse_entities.clear();
                    for (const std::uint32_t ordinal : lane.ordinals)
                    {
                        impl_->sparse_entities.push_back(
                            inactive_output.entities_[ordinal]
                        );
                    }
                    row_entities = impl_->sparse_entities;
                }

                auto loaded = plan[index].binding->load_(
                    impl_->mutation,
                    row_entities,
                    inactive_output.entities_,
                    column,
                    impl_->scratch,
                    impl_->limits
                );
                if (!loaded)
                {
                    const WorldSectionFailure reason = decodeFailure(
                        loaded.error(),
                        static_cast<std::uint32_t>(index),
                        plan[index].binding->schema().id
                    );
                    impl_->rollbackLoad(stored);
                    impl_->loads.pop_back();
                    return impl_->poison(reason);
                }
            }

            inactive_output.state_ = WorldSectionInstance::EState::STAGED;
            return {};
        }
        catch (const std::bad_alloc&)
        {
            if (operation_stored)
            {
                impl_->rollbackLoad(impl_->loads.back());
                impl_->loads.pop_back();
            }
            else
            {
                Impl::resetInstance(inactive_output);
            }
            return impl_->poison(
                failure(EWorldSectionError::ALLOCATION_FAILURE)
            );
        }
        catch (...)
        {
            if (operation_stored)
            {
                impl_->rollbackLoad(impl_->loads.back());
                impl_->loads.pop_back();
            }
            else
            {
                Impl::resetInstance(inactive_output);
            }
            return impl_->poison(failure(EWorldSectionError::DECODE_FAILED));
        }
    }

    lux::cxx::expected<void, WorldSectionFailure>
    WorldSectionTransaction::unload(WorldSectionInstance& instance) noexcept
    {
        if (!impl_ || impl_->state != Impl::EState::ACTIVE)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::TRANSACTION_FAILED)
            );
        }
        if (instance.state_ != WorldSectionInstance::EState::ACTIVE)
        {
            return impl_->poison(
                failure(EWorldSectionError::INVALID_INSTANCE_STATE)
            );
        }
        if (instance.world_identity_ !=
            detail::WorldColdAccess::identity(*impl_->world))
        {
            return impl_->poison(failure(EWorldSectionError::WRONG_WORLD));
        }

        try
        {
            struct MembershipRecord final
            {
                std::uint64_t storage{};
                Entity entity{NullEntity};
            };

            Impl::UnloadOperation operation;
            operation.instance = &instance;
            std::size_t membership_count{};
            for (const Entity entity : instance.entities_)
            {
                if (!detail::WorldSectionTransactionAccess::matches(
                        impl_->mutation,
                        entity,
                        instance.lease_
                    ))
                {
                    continue;
                }
                detail::WorldSectionTransactionAccess::forEachStorage(
                    impl_->mutation,
                    entity,
                    [&](std::uint64_t) noexcept
                    {
                        ++membership_count;
                    }
                );
            }

            std::vector<MembershipRecord> memberships;
            memberships.reserve(membership_count);
            for (const Entity entity : instance.entities_)
            {
                if (!detail::WorldSectionTransactionAccess::matches(
                        impl_->mutation,
                        entity,
                        instance.lease_
                    ))
                {
                    continue;
                }
                detail::WorldSectionTransactionAccess::forEachStorage(
                    impl_->mutation,
                    entity,
                    [&](std::uint64_t storage) noexcept
                    {
                        memberships.push_back({storage, entity});
                    }
                );
            }
            std::sort(
                memberships.begin(),
                memberships.end(),
                [](const auto& left, const auto& right) noexcept
                {
                    return left.storage < right.storage;
                }
            );
            for (std::size_t first{}; first < memberships.size();)
            {
                std::size_t last = first + 1U;
                while (last < memberships.size() &&
                       memberships[last].storage == memberships[first].storage)
                {
                    ++last;
                }
                Impl::RemovalLane lane;
                lane.storage = memberships[first].storage;
                lane.entities.reserve(last - first);
                for (; first < last; ++first)
                    lane.entities.push_back(memberships[first].entity);
                operation.removals.push_back(std::move(lane));
            }
            impl_->unloads.push_back(std::move(operation));
            instance.state_ = WorldSectionInstance::EState::STAGED;
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return impl_->poison(
                failure(EWorldSectionError::ALLOCATION_FAILURE)
            );
        }
        catch (...)
        {
            return impl_->poison(failure(EWorldSectionError::DECODE_FAILED));
        }
    }

    lux::cxx::expected<void, WorldSectionFailure>
    WorldSectionTransaction::commit() noexcept
    {
        if (!impl_ || impl_->state != Impl::EState::ACTIVE)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::TRANSACTION_FAILED)
            );
        }

        detail::WorldChangePublisher publisher(*impl_->world);

        // Canonical unload is deferred until commit. All load materialization
        // has already completed while load() held its input borrows.
        for (auto& operation : impl_->unloads)
        {
            for (auto& lane : operation.removals)
            {
                for (const Entity entity : lane.entities)
                {
                    detail::WorldSectionTransactionAccess::removeComponent(
                        impl_->mutation,
                        entity,
                        lane.storage
                    );
                }
            }
        }
        for (auto& operation : impl_->unloads)
        {
            auto& instance = *operation.instance;
            for (const Entity entity : instance.entities_)
            {
                if (detail::WorldSectionTransactionAccess::matches(
                        impl_->mutation,
                        entity,
                        instance.lease_
                    ))
                {
                    detail::WorldSectionTransactionAccess::destroyTrackedEntity(
                        impl_->mutation,
                        entity
                    );
                }
            }
        }

        // Observation order is fixed across the entire transaction.
        for (const auto& operation : impl_->unloads)
        {
            for (const auto& lane : operation.removals)
            {
                auto stream = publisher.bindComponent(lane.storage);
                if (!stream)
                    break;
                for (const Entity entity : lane.entities)
                {
                    if (!publisher.append(
                        stream,
                        entity,
                        EComponentChangeKind::REMOVED
                    ))
                        break;
                }
            }
        }
        for (const auto& operation : impl_->unloads)
        {
            for (const Entity entity : operation.instance->entities_)
            {
                if (!publisher.appendEntity(
                        entity,
                        EEntityChangeKind::DESTROYED
                    ))
                {
                    break;
                }
            }
        }
        for (const auto& operation : impl_->loads)
        {
            for (const Entity entity : operation.output->entities_)
            {
                if (!publisher.appendEntity(entity, EEntityChangeKind::ADDED))
                    break;
            }
        }
        for (const auto& operation : impl_->loads)
        {
            for (const auto& lane : operation.additions)
            {
                auto stream = publisher.bindComponent(lane.storage);
                if (!stream)
                    break;
                if (lane.dense)
                {
                    for (const Entity entity : operation.output->entities_)
                    {
                        if (!publisher.append(
                            stream,
                            entity,
                            EComponentChangeKind::ADDED
                        ))
                            break;
                    }
                }
                else
                {
                    for (const std::uint32_t ordinal : lane.ordinals)
                    {
                        if (!publisher.append(
                            stream,
                            operation.output->entities_[ordinal],
                            EComponentChangeKind::ADDED
                        ))
                            break;
                    }
                }
            }
        }

        for (auto& operation : impl_->loads)
        {
            auto& output = *operation.output;
            output.code_lifetimes_ = std::move(operation.pins);
            output.state_ = WorldSectionInstance::EState::ACTIVE;
            detail::WorldColdAccess::acquireSection(*impl_->world);
        }
        for (auto& operation : impl_->unloads)
        {
            Impl::resetInstance(*operation.instance);
            detail::WorldColdAccess::releaseSection(*impl_->world);
        }

        impl_->state = Impl::EState::COMMITTED;
        impl_->mutation = {};
        return {};
    }

    lux::cxx::expected<WorldSectionTransaction, WorldSectionFailure>
    beginWorldSectionTransaction(
        EcsState& world,
        WorldSectionLoadScratchBudget scratch,
        lux::serialization::SerializationLimits limits
    ) noexcept
    {
        if (!detail::WorldColdAccess::ownerIdle(world))
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::WORLD_BUSY)
            );
        }
        try
        {
            auto impl = std::make_unique<WorldSectionTransaction::Impl>();
            impl->world = &world;
            impl->scratch = scratch;
            impl->limits = limits;
            impl->mutation = detail::WorldColdAccess::sectionMutation(world);
            return WorldSectionTransaction(std::move(impl));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::ALLOCATION_FAILURE)
            );
        }
    }
} // namespace lux::ecs
