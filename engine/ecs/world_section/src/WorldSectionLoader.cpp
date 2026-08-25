#include <lux/engine/ecs/WorldSectionLoader.hpp>

#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
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
        WorldMutation& edit,
        std::uint64_t storage,
        std::span<const Entity> entities
    ) noexcept
    {
        detail::WorldSectionTransactionAccess::addComponentMembership(
            edit,
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

    struct WorldSectionLoadBatch::Impl final
    {
        struct LoadOperation final
        {
            ComponentLoadSet loads;
            const WorldSectionImage* image{};
            WorldSectionInstance* output{};
            std::vector<const ComponentLoadBinding*> plan;
            std::vector<std::shared_ptr<const void>> pins;
            std::size_t component_rows{};
            std::size_t max_sparse_rows{};
        };

        World* world{};
        WorldMutation edit;
        WorldSectionLoadScratchBudget scratch{};
        lux::serialization::SerializationLimits limits{};
        std::vector<LoadOperation> loads;
        std::vector<WorldSectionInstance*> unloads;
        std::vector<Entity> sparse_entities;

        void resetStaged() noexcept
        {
            for (auto& operation : loads)
            {
                auto& output = *operation.output;
                output.id_ = {};
                output.entities_.clear();
                output.code_lifetimes_.clear();
                output.world_identity_ = 0U;
                output.lease_ = 0U;
                output.state_ = WorldSectionInstance::EState::INACTIVE;
            }
            for (WorldSectionInstance* instance : unloads)
                instance->state_ = WorldSectionInstance::EState::ACTIVE;
        }

        void rollbackLoads() noexcept
        {
            for (auto& operation : loads)
            {
                auto& output = *operation.output;
                detail::WorldSectionTransactionAccess::rollbackEntities(
                    edit,
                    output.lease_,
                    output.entities_
                );
                detail::WorldSectionTransactionAccess::destroyBareEntities(
                    edit,
                    output.entities_
                );
            }
        }
    };

    WorldSectionLoadBatch::WorldSectionLoadBatch(
        std::unique_ptr<Impl> impl
    ) noexcept
        : impl_(std::move(impl))
    {
    }

    WorldSectionLoadBatch::WorldSectionLoadBatch(
        WorldSectionLoadBatch&& other
    ) noexcept = default;

    WorldSectionLoadBatch& WorldSectionLoadBatch::operator=(
        WorldSectionLoadBatch&& other
    ) noexcept
    {
        if (this != &other)
        {
            if (impl_)
            {
                impl_->rollbackLoads();
                impl_->resetStaged();
            }
            impl_ = std::move(other.impl_);
        }
        return *this;
    }

    WorldSectionLoadBatch::~WorldSectionLoadBatch() noexcept
    {
        if (impl_)
        {
            impl_->rollbackLoads();
            impl_->resetStaged();
        }
    }

    lux::cxx::expected<void, WorldSectionFailure>
    WorldSectionLoadBatch::load(
        const ComponentLoadSet& loads,
        const WorldSectionImage& image,
        WorldSectionInstance& inactive_output
    ) noexcept
    {
        if (!impl_ ||
            inactive_output.state_ != WorldSectionInstance::EState::INACTIVE)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::INVALID_INSTANCE_STATE)
            );
        }

        try
        {
            Impl::LoadOperation operation;
            operation.loads = loads;
            operation.image = &image;
            operation.output = &inactive_output;
            operation.plan.reserve(image.columns().size());

            for (std::size_t index{}; index < image.columns().size(); ++index)
            {
                const auto& column = image.columns()[index];
                const ComponentLoadBinding* binding = operation.loads.find(
                    column.schemaHash(),
                    column.schemaName()
                );
                if (binding == nullptr)
                {
                    return lux::cxx::unexpected(failure(
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
                    return lux::cxx::unexpected(failure(
                        EWorldSectionError::BINDING_MISMATCH,
                        static_cast<std::uint32_t>(index),
                        binding->schema().id
                    ));
                }
                if (column.rowCount() >
                    std::numeric_limits<std::size_t>::max() -
                        operation.component_rows)
                {
                    return lux::cxx::unexpected(
                        failure(EWorldSectionError::LIMIT_EXCEEDED)
                    );
                }
                operation.component_rows += column.rowCount();
                operation.plan.push_back(binding);
                appendUniquePin(
                    operation.pins,
                    operation.loads.codeLifetime(*binding)
                );
                appendUniquePin(
                    operation.pins,
                    binding->schema().code_lifetime
                );
                if (column.ordinalEncoding() ==
                    EWorldSectionOrdinalEncoding::U32_LIST)
                {
                    operation.max_sparse_rows = std::max(
                        operation.max_sparse_rows,
                        static_cast<std::size_t>(column.rowCount())
                    );
                }
            }

            inactive_output.id_ = image.id();
            inactive_output.entities_.resize(image.entityCount(), NullEntity);
            inactive_output.world_identity_ =
                detail::WorldColdAccess::identity(*impl_->world);
            inactive_output.lease_ =
                detail::WorldSectionTransactionAccess::allocateLease(
                    impl_->edit
                );
            impl_->loads.push_back(std::move(operation));
            inactive_output.state_ = WorldSectionInstance::EState::STAGED;
            return {};
        }
        catch (const std::bad_alloc&)
        {
            inactive_output.id_ = {};
            inactive_output.entities_.clear();
            inactive_output.world_identity_ = 0U;
            inactive_output.lease_ = 0U;
            return lux::cxx::unexpected(
                failure(EWorldSectionError::ALLOCATION_FAILURE)
            );
        }
        catch (...)
        {
            inactive_output.id_ = {};
            inactive_output.entities_.clear();
            inactive_output.world_identity_ = 0U;
            inactive_output.lease_ = 0U;
            return lux::cxx::unexpected(
                failure(EWorldSectionError::DECODE_FAILED)
            );
        }
    }

    lux::cxx::expected<void, WorldSectionFailure>
    WorldSectionLoadBatch::unload(
        WorldSectionInstance& instance
    ) noexcept
    {
        if (!impl_ || instance.state_ != WorldSectionInstance::EState::ACTIVE)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::INVALID_INSTANCE_STATE)
            );
        }
        if (instance.world_identity_ !=
            detail::WorldColdAccess::identity(*impl_->world))
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::WRONG_WORLD)
            );
        }
        if (std::find(impl_->unloads.begin(), impl_->unloads.end(), &instance) !=
            impl_->unloads.end())
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::INVALID_INSTANCE_STATE)
            );
        }
        try
        {
            impl_->unloads.push_back(&instance);
            instance.state_ = WorldSectionInstance::EState::STAGED;
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::ALLOCATION_FAILURE)
            );
        }
    }

    lux::cxx::expected<void, WorldSectionFailure>
    WorldSectionLoadBatch::commit() noexcept
    {
        if (!impl_)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::INVALID_INSTANCE_STATE)
            );
        }

        const auto fail = [&](WorldSectionFailure reason)
            -> lux::cxx::expected<void, WorldSectionFailure>
        {
            impl_->rollbackLoads();
            impl_->resetStaged();
            impl_.reset();
            return lux::cxx::unexpected(std::move(reason));
        };

        const std::size_t active_sections =
            detail::WorldColdAccess::activeSectionCount(*impl_->world);
        detail::require(active_sections >= impl_->unloads.size());
        if (impl_->loads.size() >
            std::numeric_limits<std::size_t>::max() -
                (active_sections - impl_->unloads.size()))
        {
            return fail(failure(EWorldSectionError::LIMIT_EXCEEDED));
        }

        try
        {
            std::size_t max_sparse_rows{};
            for (const auto& operation : impl_->loads)
            {
                max_sparse_rows = std::max(
                    max_sparse_rows,
                    operation.max_sparse_rows
                );
                detail::WorldSectionTransactionAccess::createEntities(
                    impl_->edit,
                    operation.output->entities_
                );
                detail::WorldSectionTransactionAccess::reserveMembership(
                    impl_->edit,
                    operation.output->entities_,
                    operation.component_rows
                );
                detail::WorldSectionTransactionAccess::activateMembership(
                    impl_->edit,
                    operation.output->lease_,
                    operation.output->entities_
                );
            }
            impl_->sparse_entities.reserve(max_sparse_rows);

            for (auto& operation : impl_->loads)
            {
                const auto columns = operation.image->columns();
                for (std::size_t index{}; index < columns.size(); ++index)
                {
                    const auto& column = columns[index];
                    if (column.rowCount() == 0U)
                        continue;

                    std::span<const Entity> row_entities;
                    if (column.ordinalEncoding() ==
                        EWorldSectionOrdinalEncoding::DENSE)
                    {
                        row_entities = operation.output->entities_;
                    }
                    else
                    {
                        impl_->sparse_entities.clear();
                        for (std::size_t row{}; row < column.rowCount(); ++row)
                        {
                            const std::uint32_t ordinal =
                                detail::readColumnU32(
                                    column.ordinalBytes(),
                                    row * sizeof(std::uint32_t)
                                );
                            impl_->sparse_entities.push_back(
                                operation.output->entities_[ordinal]
                            );
                        }
                        row_entities = impl_->sparse_entities;
                    }

                    auto loaded = operation.plan[index]->load_(
                        impl_->edit,
                        row_entities,
                        operation.output->entities_,
                        column,
                        impl_->scratch,
                        impl_->limits
                    );
                    if (!loaded)
                    {
                        return fail(decodeFailure(
                            loaded.error(),
                            static_cast<std::uint32_t>(index),
                            operation.plan[index]->schema().id
                        ));
                    }
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            return fail(failure(EWorldSectionError::ALLOCATION_FAILURE));
        }
        catch (...)
        {
            return fail(failure(EWorldSectionError::DECODE_FAILED));
        }

        const std::uint64_t publication_epoch =
            detail::worldChangeEpoch(*impl_->world);
        bool publish_exact = true;
        const auto component_change = [&](std::uint64_t storage,
                                          Entity entity,
                                          EComponentChangeKind kind) noexcept
        {
            if (!publish_exact)
                return;
            detail::recordWorldComponentChange(
                *impl_->world,
                storage,
                entity,
                kind
            );
            publish_exact = detail::worldChangeEpoch(*impl_->world) ==
                publication_epoch;
        };
        const auto entity_change = [&](Entity entity,
                                       EEntityChangeKind kind) noexcept
        {
            if (!publish_exact)
                return;
            detail::recordWorldEntityChange(*impl_->world, entity, kind);
            publish_exact = detail::worldChangeEpoch(*impl_->world) ==
                publication_epoch;
        };

        // Publication order is fixed for the entire lexical batch:
        // removals, destructions, creations, additions.
        for (WorldSectionInstance* instance : impl_->unloads)
        {
            for (const Entity entity : instance->entities_)
            {
                if (!detail::WorldSectionTransactionAccess::matches(
                        impl_->edit,
                        entity,
                        instance->lease_
                    ))
                    continue;
                detail::WorldSectionTransactionAccess::forEachStorage(
                    impl_->edit,
                    entity,
                    [&](std::uint64_t storage) noexcept
                    {
                        component_change(
                            storage,
                            entity,
                            EComponentChangeKind::REMOVED
                        );
                        detail::WorldSectionTransactionAccess::removeComponent(
                            impl_->edit,
                            entity,
                            storage
                        );
                    }
                );
            }
        }
        for (WorldSectionInstance* instance : impl_->unloads)
        {
            for (const Entity entity : instance->entities_)
            {
                if (!detail::WorldSectionTransactionAccess::matches(
                        impl_->edit,
                        entity,
                        instance->lease_
                    ))
                    continue;
                detail::WorldSectionTransactionAccess::destroyTrackedEntity(
                    impl_->edit,
                    entity
                );
                entity_change(entity, EEntityChangeKind::DESTROYED);
            }
        }
        for (const auto& operation : impl_->loads)
        {
            for (const Entity entity : operation.output->entities_)
                entity_change(entity, EEntityChangeKind::ADDED);
        }
        for (const auto& operation : impl_->loads)
        {
            const auto columns = operation.image->columns();
            for (std::size_t index{}; index < columns.size(); ++index)
            {
                const auto& column = columns[index];
                const auto storage = operation.plan[index]->storage_;
                if (column.ordinalEncoding() ==
                    EWorldSectionOrdinalEncoding::DENSE)
                {
                    for (const Entity entity : operation.output->entities_)
                    {
                        component_change(
                            storage,
                            entity,
                            EComponentChangeKind::ADDED
                        );
                    }
                }
                else
                {
                    for (std::size_t row{}; row < column.rowCount(); ++row)
                    {
                        const std::uint32_t ordinal = detail::readColumnU32(
                            column.ordinalBytes(),
                            row * sizeof(std::uint32_t)
                        );
                        component_change(
                            storage,
                            operation.output->entities_[ordinal],
                            EComponentChangeKind::ADDED
                        );
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
        for (WorldSectionInstance* instance : impl_->unloads)
        {
            instance->id_ = {};
            instance->entities_.clear();
            instance->code_lifetimes_.clear();
            instance->world_identity_ = 0U;
            instance->lease_ = 0U;
            instance->state_ = WorldSectionInstance::EState::INACTIVE;
            detail::WorldColdAccess::releaseSection(*impl_->world);
        }

        impl_.reset();
        return {};
    }

    lux::cxx::expected<WorldSectionLoadBatch, WorldSectionFailure>
    WorldSectionLoader::begin(
        World& world,
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
            auto impl = std::make_unique<WorldSectionLoadBatch::Impl>();
            impl->world = &world;
            impl->scratch = scratch;
            impl->limits = limits;
            impl->edit = detail::WorldColdAccess::sectionMutation(world);
            return WorldSectionLoadBatch(std::move(impl));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::ALLOCATION_FAILURE)
            );
        }
    }
} // namespace lux::ecs
