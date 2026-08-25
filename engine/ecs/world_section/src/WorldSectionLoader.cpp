#include <lux/engine/ecs/WorldSectionLoader.hpp>

#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/ecs/world_section/detail/ComponentLoadSerialization.hpp>
#include <lux/engine/ecs/world_section/detail/WorldSectionTransactionAccess.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace lux::ecs
{
    namespace detail
    {
        class SectionResidency final
        {
          public:
            struct ResolvedColumn final
            {
                std::uint64_t storage{};
                std::uint32_t image_column{};
            };

            WorldSectionImage image;
            std::vector<ResolvedColumn> columns;
            std::vector<std::uint32_t> overlay_heads;
            std::vector<std::shared_ptr<const void>> pins;

            void reserveOverlay(std::size_t entity_count)
            {
                overlay_heads.assign(entity_count, InvalidNode);
            }

            [[nodiscard]] ResidencyMutationToken prepareAdd(
                std::uint32_t ordinal,
                std::uint64_t storage
            )
            {
                const NodeIndex existing = findOverlay(ordinal, storage);
                if (existing != InvalidNode)
                {
                    return nodes_[existing].kind == EOverlayKind::REMOVED
                        ? token(ETokenAction::UNLINK, existing)
                        : 0U;
                }
                if (baseContains(ordinal, storage))
                    return 0U;
                return token(
                    ETokenAction::LINK,
                    allocateNode(storage, EOverlayKind::ADDED)
                );
            }

            [[nodiscard]] ResidencyMutationToken prepareRemove(
                std::uint32_t ordinal,
                std::uint64_t storage
            )
            {
                const NodeIndex existing = findOverlay(ordinal, storage);
                if (existing != InvalidNode)
                {
                    return nodes_[existing].kind == EOverlayKind::ADDED
                        ? token(ETokenAction::UNLINK, existing)
                        : 0U;
                }
                if (!baseContains(ordinal, storage))
                    return 0U;
                return token(
                    ETokenAction::LINK,
                    allocateNode(storage, EOverlayKind::REMOVED)
                );
            }

            void commitMutation(
                std::uint32_t ordinal,
                ResidencyMutationToken value
            ) noexcept
            {
                const auto [action, node] = decode(value);
                if (action == ETokenAction::LINK)
                {
                    nodes_[node].next = overlay_heads[ordinal];
                    overlay_heads[ordinal] = node;
                    return;
                }
                require(action == ETokenAction::UNLINK);
                unlink(ordinal, node);
            }

            void cancelMutation(ResidencyMutationToken value) noexcept
            {
                const auto [action, node] = decode(value);
                if (action == ETokenAction::LINK)
                    releaseNode(node);
            }

            template <class Fn>
            void forEachActual(std::uint32_t ordinal, Fn&& fn) const noexcept
            {
                for (const ResolvedColumn& resolved : columns)
                {
                    if (baseContains(ordinal, resolved.storage) &&
                        !hasOverlay(
                            ordinal,
                            resolved.storage,
                            EOverlayKind::REMOVED
                        ))
                    {
                        fn(resolved.storage);
                    }
                }
                for (NodeIndex node = overlay_heads[ordinal];
                     node != InvalidNode;
                     node = nodes_[node].next)
                {
                    if (nodes_[node].kind == EOverlayKind::ADDED)
                        fn(nodes_[node].storage);
                }
            }

            void deactivate(std::uint32_t ordinal) noexcept
            {
                NodeIndex node = overlay_heads[ordinal];
                while (node != InvalidNode)
                {
                    const NodeIndex next = nodes_[node].next;
                    releaseNode(node);
                    node = next;
                }
                overlay_heads[ordinal] = InvalidNode;
            }

            [[nodiscard]] bool basePresent(
                std::uint32_t ordinal,
                std::uint64_t storage
            ) const noexcept
            {
                return baseContains(ordinal, storage) &&
                    !hasOverlay(
                        ordinal,
                        storage,
                        EOverlayKind::REMOVED
                    );
            }

            template <class Fn>
            void forEachAdded(std::uint32_t ordinal, Fn&& fn) const noexcept
            {
                for (NodeIndex node = overlay_heads[ordinal];
                     node != InvalidNode;
                     node = nodes_[node].next)
                {
                    if (nodes_[node].kind == EOverlayKind::ADDED)
                        fn(nodes_[node].storage);
                }
            }

            [[nodiscard]] static const SectionResidencyPort& port() noexcept
            {
                static const SectionResidencyPort value{
                    [](void* context, std::uint32_t ordinal,
                       std::uint64_t storage) -> ResidencyMutationToken
                    {
                        return static_cast<SectionResidency*>(context)
                            ->prepareAdd(ordinal, storage);
                    },
                    [](void* context, std::uint32_t ordinal,
                       std::uint64_t storage) -> ResidencyMutationToken
                    {
                        return static_cast<SectionResidency*>(context)
                            ->prepareRemove(ordinal, storage);
                    },
                    [](void* context, std::uint32_t ordinal,
                       ResidencyMutationToken mutation) noexcept
                    {
                        static_cast<SectionResidency*>(context)
                            ->commitMutation(ordinal, mutation);
                    },
                    [](void* context,
                       ResidencyMutationToken mutation) noexcept
                    {
                        static_cast<SectionResidency*>(context)
                            ->cancelMutation(mutation);
                    },
                    [](const void* context, std::uint32_t ordinal,
                       void* visitor,
                       void (*visit)(void*, std::uint64_t) noexcept) noexcept
                    {
                        static_cast<const SectionResidency*>(context)
                            ->forEachActual(
                                ordinal,
                                [&](std::uint64_t storage) noexcept
                                {
                                    visit(visitor, storage);
                                }
                            );
                    },
                    [](void* context, std::uint32_t ordinal) noexcept
                    {
                        static_cast<SectionResidency*>(context)
                            ->deactivate(ordinal);
                    }
                };
                return value;
            }

          private:
            using NodeIndex = std::uint32_t;
            static constexpr NodeIndex InvalidNode =
                std::numeric_limits<NodeIndex>::max();

            enum class EOverlayKind : std::uint8_t
            {
                ADDED,
                REMOVED,
            };

            enum class ETokenAction : std::uint32_t
            {
                LINK = 1U,
                UNLINK = 2U,
            };

            struct Node final
            {
                std::uint64_t storage{};
                NodeIndex next{InvalidNode};
                EOverlayKind kind{EOverlayKind::ADDED};
            };

            [[nodiscard]] const ResolvedColumn* findColumn(
                std::uint64_t storage
            ) const noexcept
            {
                const auto found = std::lower_bound(
                    columns.begin(),
                    columns.end(),
                    storage,
                    [](const ResolvedColumn& column, std::uint64_t value)
                    {
                        return column.storage < value;
                    }
                );
                return found != columns.end() && found->storage == storage
                    ? std::addressof(*found)
                    : nullptr;
            }

            [[nodiscard]] bool baseContains(
                std::uint32_t ordinal,
                std::uint64_t storage
            ) const noexcept
            {
                const ResolvedColumn* resolved = findColumn(storage);
                if (resolved == nullptr)
                    return false;
                const auto& column = image.columns()[resolved->image_column];
                if (column.ordinalEncoding() ==
                    EWorldSectionOrdinalEncoding::DENSE)
                {
                    return ordinal < image.entityCount();
                }
                std::size_t first{};
                std::size_t count = column.rowCount();
                while (count != 0U)
                {
                    const std::size_t step = count / 2U;
                    const std::size_t current = first + step;
                    const std::uint32_t value = readColumnU32(
                        column.ordinalBytes(),
                        current * sizeof(std::uint32_t)
                    );
                    if (value < ordinal)
                    {
                        first = current + 1U;
                        count -= step + 1U;
                    }
                    else
                    {
                        count = step;
                    }
                }
                return first < column.rowCount() &&
                    readColumnU32(
                        column.ordinalBytes(),
                        first * sizeof(std::uint32_t)
                    ) == ordinal;
            }

            [[nodiscard]] NodeIndex findOverlay(
                std::uint32_t ordinal,
                std::uint64_t storage
            ) const noexcept
            {
                for (NodeIndex node = overlay_heads[ordinal];
                     node != InvalidNode;
                     node = nodes_[node].next)
                {
                    if (nodes_[node].storage == storage)
                        return node;
                }
                return InvalidNode;
            }

            [[nodiscard]] bool hasOverlay(
                std::uint32_t ordinal,
                std::uint64_t storage,
                EOverlayKind kind
            ) const noexcept
            {
                const NodeIndex node = findOverlay(ordinal, storage);
                return node != InvalidNode && nodes_[node].kind == kind;
            }

            [[nodiscard]] NodeIndex allocateNode(
                std::uint64_t storage,
                EOverlayKind kind
            )
            {
                NodeIndex result{};
                if (free_ != InvalidNode)
                {
                    result = free_;
                    free_ = nodes_[result].next;
                }
                else
                {
                    if (nodes_.size() >= InvalidNode)
                        throw std::bad_alloc{};
                    result = static_cast<NodeIndex>(nodes_.size());
                    nodes_.push_back(Node{});
                }
                nodes_[result] = Node{storage, InvalidNode, kind};
                return result;
            }

            void releaseNode(NodeIndex node) noexcept
            {
                nodes_[node] = Node{0U, free_, EOverlayKind::ADDED};
                free_ = node;
            }

            void unlink(std::uint32_t ordinal, NodeIndex target) noexcept
            {
                NodeIndex* link = &overlay_heads[ordinal];
                while (*link != InvalidNode && *link != target)
                    link = &nodes_[*link].next;
                require(*link == target);
                *link = nodes_[target].next;
                releaseNode(target);
            }

            [[nodiscard]] static ResidencyMutationToken token(
                ETokenAction action,
                NodeIndex node
            ) noexcept
            {
                return (static_cast<std::uint64_t>(action) << 32U) |
                    (static_cast<std::uint64_t>(node) + 1U);
            }

            [[nodiscard]] static std::pair<ETokenAction, NodeIndex> decode(
                ResidencyMutationToken value
            ) noexcept
            {
                require(value != 0U);
                return {
                    static_cast<ETokenAction>(value >> 32U),
                    static_cast<NodeIndex>(value - 1U)
                };
            }

            std::vector<Node> nodes_;
            NodeIndex free_{InvalidNode};
        };
    } // namespace detail

    WorldSectionInstance::WorldSectionInstance(
        WorldSectionInstance&& other
    ) noexcept
        : id_(std::move(other.id_)),
          entities_(std::move(other.entities_)),
          residency_(std::move(other.residency_)),
          world_identity_(std::exchange(other.world_identity_, 0U)),
          lease_(std::exchange(other.lease_, 0U)),
          state_(std::exchange(other.state_, EState::INACTIVE))
    {
        detail::require(state_ != EState::STAGED);
        other.id_ = {};
        other.entities_.clear();
        other.residency_.reset();
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
            WorldSectionImage image;
            WorldSectionInstance* output{};
            std::vector<const ComponentLoadBinding*> plan;
            std::vector<std::shared_ptr<const void>> pins;
            std::shared_ptr<detail::SectionResidency> residency;
            std::size_t component_rows{};
            bool activated{};
        };

        World* world{};
        WorldEdit edit;
        WorldSectionLoadScratchBudget scratch{};
        lux::serialization::SerializationLimits limits{};
        std::vector<LoadOperation> loads;
        std::vector<WorldSectionInstance*> unloads;
        detail::EntityAllocatorCheckpoint entity_checkpoint;
        std::size_t total_entities{};
        std::size_t total_columns{};
        std::size_t total_component_rows{};

        void resetStaged() noexcept
        {
            for (auto& operation : loads)
            {
                auto& output = *operation.output;
                output.id_ = {};
                output.entities_.clear();
                output.residency_.reset();
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
                for (std::size_t ordinal{};
                     ordinal < output.entities_.size();
                     ++ordinal)
                {
                    const Entity entity = output.entities_[ordinal];
                    if (entity == NullEntity ||
                        !detail::WorldSectionTransactionAccess::matches(
                            edit,
                            entity,
                            output.lease_
                        ))
                        continue;
                    operation.residency->forEachActual(
                        static_cast<std::uint32_t>(ordinal),
                        [&](std::uint64_t storage) noexcept
                        {
                            detail::WorldSectionTransactionAccess::removeComponent(
                                edit,
                                entity,
                                storage
                            );
                        }
                    );
                    detail::WorldSectionTransactionAccess::deactivateEntity(
                        edit,
                        entity
                    );
                }
                if (operation.residency && operation.activated)
                {
                    detail::WorldSectionTransactionAccess::releaseResidency(
                        edit,
                        output.lease_
                    );
                    operation.activated = false;
                }
            }
            if (entity_checkpoint.captured)
            {
                detail::WorldSectionTransactionAccess::restoreEntityAllocator(
                    edit,
                    entity_checkpoint
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
            operation.image = image;
            operation.output = &inactive_output;
            operation.residency =
                std::make_shared<detail::SectionResidency>();
            operation.residency->image = image;
            operation.residency->reserveOverlay(image.entityCount());
            operation.plan.reserve(image.columns().size());
            operation.residency->columns.reserve(image.columns().size());

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
                if (column.valueEncoding() !=
                        EWorldSectionValueEncoding::TAG &&
                    impl_->scratch.decode_bytes < binding->value_size_)
                {
                    return lux::cxx::unexpected(failure(
                        EWorldSectionError::LIMIT_EXCEEDED,
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
                operation.residency->columns.push_back(
                    detail::SectionResidency::ResolvedColumn{
                        binding->storage_,
                        static_cast<std::uint32_t>(index)
                    }
                );
                appendUniquePin(
                    operation.pins,
                    operation.loads.codeLifetime(*binding)
                );
                appendUniquePin(
                    operation.pins,
                    binding->schema().code_lifetime
                );
            }

            std::sort(
                operation.residency->columns.begin(),
                operation.residency->columns.end(),
                [](const auto& left, const auto& right) noexcept
                {
                    return left.storage < right.storage;
                }
            );
            operation.residency->pins = operation.pins;

            if (image.entityCount() >
                    std::numeric_limits<std::size_t>::max() -
                        impl_->total_entities ||
                image.columns().size() >
                    std::numeric_limits<std::size_t>::max() -
                        impl_->total_columns ||
                operation.component_rows >
                    std::numeric_limits<std::size_t>::max() -
                        impl_->total_component_rows)
            {
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::LIMIT_EXCEEDED)
                );
            }

            inactive_output.id_ = image.id();
            inactive_output.entities_.resize(image.entityCount(), NullEntity);
            inactive_output.world_identity_ =
                detail::WorldColdAccess::identity(*impl_->world);
            inactive_output.lease_ =
                detail::WorldSectionTransactionAccess::allocateLease(
                    impl_->edit
                );
            inactive_output.residency_ = operation.residency;
            impl_->loads.push_back(std::move(operation));
            impl_->total_entities += image.entityCount();
            impl_->total_columns += image.columns().size();
            impl_->total_component_rows +=
                impl_->loads.back().component_rows;
            inactive_output.state_ = WorldSectionInstance::EState::STAGED;
            return {};
        }
        catch (const std::bad_alloc&)
        {
            inactive_output.id_ = {};
            inactive_output.entities_.clear();
            inactive_output.world_identity_ = 0U;
            inactive_output.lease_ = 0U;
            inactive_output.residency_.reset();
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
            inactive_output.residency_.reset();
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
            detail::WorldSectionTransactionAccess::captureEntityAllocator(
                impl_->edit,
                impl_->entity_checkpoint
            );
            for (auto& operation : impl_->loads)
            {
                detail::WorldSectionTransactionAccess::createEntities(
                    impl_->edit,
                    operation.output->entities_
                );
                detail::WorldSectionTransactionAccess::reserveResidency(
                    impl_->edit,
                    operation.output->entities_,
                    impl_->loads.size()
                );
                detail::WorldSectionTransactionAccess::activateResidency(
                    impl_->edit,
                    operation.output->lease_,
                    operation.residency,
                    operation.residency.get(),
                    detail::SectionResidency::port(),
                    operation.output->entities_
                );
                operation.activated = true;
            }

            for (auto& operation : impl_->loads)
            {
                const auto columns = operation.image.columns();
                for (std::size_t index{}; index < columns.size(); ++index)
                {
                    const auto& column = columns[index];
                    if (column.rowCount() == 0U)
                        continue;

                    auto loaded = operation.plan[index]->load_(
                        impl_->edit,
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
            detail::require(instance->residency_ != nullptr);
            auto& residency = *instance->residency_;
            for (const auto& resolved : residency.columns)
            {
                const auto& column =
                    residency.image.columns()[resolved.image_column];
                for (std::size_t row{}; row < column.rowCount(); ++row)
                {
                    const std::uint32_t ordinal =
                        column.ordinalEncoding() ==
                            EWorldSectionOrdinalEncoding::DENSE
                        ? static_cast<std::uint32_t>(row)
                        : detail::readColumnU32(
                            column.ordinalBytes(),
                            row * sizeof(std::uint32_t)
                        );
                    const Entity entity = instance->entities_[ordinal];
                    if (!detail::WorldSectionTransactionAccess::matches(
                            impl_->edit,
                            entity,
                            instance->lease_
                        ) ||
                        !residency.basePresent(ordinal, resolved.storage) ||
                        !detail::WorldSectionTransactionAccess::hasComponent(
                            impl_->edit,
                            entity,
                            resolved.storage
                        ))
                        continue;
                    component_change(
                        resolved.storage,
                        entity,
                        EComponentChangeKind::REMOVED
                    );
                    detail::WorldSectionTransactionAccess::removeComponent(
                        impl_->edit,
                        entity,
                        resolved.storage
                    );
                }
            }
            for (std::size_t ordinal{};
                 ordinal < instance->entities_.size();
                 ++ordinal)
            {
                const Entity entity = instance->entities_[ordinal];
                if (!detail::WorldSectionTransactionAccess::matches(
                        impl_->edit,
                        entity,
                        instance->lease_
                    ))
                    continue;
                residency.forEachAdded(
                    static_cast<std::uint32_t>(ordinal),
                    [&](std::uint64_t storage) noexcept
                    {
                        if (!detail::WorldSectionTransactionAccess::hasComponent(
                                impl_->edit,
                                entity,
                                storage
                            ))
                            return;
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
            const auto columns = operation.image.columns();
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
            output.residency_ = operation.residency;
            output.state_ = WorldSectionInstance::EState::ACTIVE;
            detail::WorldColdAccess::acquireSection(*impl_->world);
        }
        for (WorldSectionInstance* instance : impl_->unloads)
        {
            instance->id_ = {};
            instance->entities_.clear();
            detail::WorldSectionTransactionAccess::releaseResidency(
                impl_->edit,
                instance->lease_
            );
            instance->residency_.reset();
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
            impl->edit = detail::WorldColdAccess::sectionEdit(world);
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
