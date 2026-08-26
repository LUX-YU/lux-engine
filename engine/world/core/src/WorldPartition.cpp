#include <lux/engine/world/WorldPartition.hpp>
#include <lux/engine/world/WorldPartitioner.hpp>
#include <lux/engine/world/detail/WorldFailureInjection.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

namespace lux::world
{
    WorldPartitionWorkspace::WorldPartitionWorkspace() noexcept = default;
    WorldPartitionWorkspace::~WorldPartitionWorkspace() = default;
    WorldPartitioner::WorldPartitioner() noexcept = default;
    WorldPartitioner::~WorldPartitioner() = default;

    EWorldPartitionWorkspaceState WorldPartitionWorkspace::state() const noexcept
    {
        return state_;
    }

    lux::cxx::expected<void, WorldPartitionFailure>
    WorldPartitionWorkspace::rebuild(const WorldDescription& world) noexcept
    {
        auto result = doRebuild(world);
        state_ = result ? EWorldPartitionWorkspaceState::SYNCHRONIZED
                        : EWorldPartitionWorkspaceState::STALE;
        return result;
    }

    namespace
    {
        [[nodiscard]] WorldPartitionFailure staleWorkspaceFailure() noexcept
        {
            WorldPartitionFailure failure;
            failure.code = EWorldPartitionError::WORKSPACE_STALE;
            return failure;
        }
    }

    lux::cxx::expected<void, WorldPartitionFailure>
    WorldPartitionWorkspace::objectAdded(WorldObjectSnapshotView object) noexcept
    {
        if (state_ != EWorldPartitionWorkspaceState::SYNCHRONIZED)
            return lux::cxx::unexpected(staleWorkspaceFailure());
        auto result = doObjectAdded(object);
        if (!result)
            state_ = EWorldPartitionWorkspaceState::STALE;
        return result;
    }

    lux::cxx::expected<void, WorldPartitionFailure>
    WorldPartitionWorkspace::objectChanged(WorldObjectSnapshotView object) noexcept
    {
        if (state_ != EWorldPartitionWorkspaceState::SYNCHRONIZED)
            return lux::cxx::unexpected(staleWorkspaceFailure());
        auto result = doObjectChanged(object);
        if (!result)
            state_ = EWorldPartitionWorkspaceState::STALE;
        return result;
    }

    lux::cxx::expected<void, WorldPartitionFailure>
    WorldPartitionWorkspace::objectRemoved(WorldObjectId object) noexcept
    {
        if (state_ != EWorldPartitionWorkspaceState::SYNCHRONIZED)
            return lux::cxx::unexpected(staleWorkspaceFailure());
        auto result = doObjectRemoved(object);
        if (!result)
            state_ = EWorldPartitionWorkspaceState::STALE;
        return result;
    }

    lux::cxx::expected<WorldPartitionBuildProduct, WorldPartitionFailure>
    WorldPartitionWorkspace::freeze(const WorldDescription& world) const noexcept
    {
        if (state_ != EWorldPartitionWorkspaceState::SYNCHRONIZED)
            return lux::cxx::unexpected(staleWorkspaceFailure());
        return doFreeze(world);
    }

    lux::cxx::expected<
        std::unique_ptr<WorldPartitionWorkspace>,
        WorldPartitionFailure>
    WorldPartitioner::createWorkspace(
        const WorldDescription& world
    ) const noexcept
    {
        auto workspace = createWorkspaceImplementation();
        if (!workspace)
            return lux::cxx::unexpected(workspace.error());
        if (!*workspace)
        {
            WorldPartitionFailure failure;
            failure.code = EWorldPartitionError::IMPLEMENTATION_FAILURE;
            return lux::cxx::unexpected(std::move(failure));
        }
        auto rebuilt = (*workspace)->rebuild(world);
        if (!rebuilt)
            return lux::cxx::unexpected(rebuilt.error());
        return std::move(*workspace);
    }

    WorldPartitionView::WorldPartitionView(
        const WorldPartitionLayout& layout,
        std::size_t partition_index
    ) noexcept
        : layout_(&layout), partition_index_(partition_index)
    {
    }

    WorldPartitionOrdinal WorldPartitionView::ordinal() const noexcept
    {
        return WorldPartitionOrdinal{
            static_cast<std::uint32_t>(partition_index_)};
    }

    WorldPartitionId WorldPartitionView::id() const noexcept
    {
        return layout_->partitions_[partition_index_].id;
    }

    std::span<const WorldObjectId> WorldPartitionView::objects() const noexcept
    {
        const auto& record = layout_->partitions_[partition_index_];
        return std::span<const WorldObjectId>(layout_->objects_).subspan(
            record.first_object,
            record.object_count
        );
    }

    bool WorldPartitionLayout::empty() const noexcept
    {
        return partitions_.empty();
    }

    std::size_t WorldPartitionLayout::partitionCount() const noexcept
    {
        return partitions_.size();
    }

    WorldPartitionView WorldPartitionLayout::partitionAt(
        std::size_t index
    ) const noexcept
    {
        return index < partitions_.size() ? WorldPartitionView(*this, index)
                                          : WorldPartitionView{};
    }

    WorldPartitionView WorldPartitionLayout::findPartition(
        WorldPartitionId id
    ) const noexcept
    {
        if (!id.valid())
            return {};
        const auto iterator = std::lower_bound(
            partitions_.begin(),
            partitions_.end(),
            id,
            [](const PartitionRecord& record, const WorldPartitionId& value)
            {
                return WorldPartitionIdLess{}(record.id, value);
            }
        );
        if (iterator == partitions_.end() || iterator->id != id)
            return {};
        return WorldPartitionView(
            *this,
            static_cast<std::size_t>(std::distance(partitions_.begin(), iterator))
        );
    }

    struct WorldPartitionLayoutBuilder::Impl final
    {
        struct PendingPartition final
        {
            WorldPartitionId id;
            std::vector<WorldObjectId> objects;
        };

        const WorldDescription* world{};
        std::vector<PendingPartition> partitions;
        std::unordered_set<WorldPartitionId, WorldPartitionIdHash> partition_ids;
        std::unordered_set<WorldObjectId, WorldObjectIdHash> assigned_objects;
    };

    namespace
    {
        [[nodiscard]] WorldPartitionFailure partitionFailure(
            EWorldPartitionError code,
            WorldObjectId object = {},
            WorldPartitionId partition = {},
            WorldPartitionIndexTypeId index_type = {}
        ) noexcept
        {
            return WorldPartitionFailure{
                code,
                object,
                partition,
                std::move(index_type),
                0U};
        }
    } // namespace

    WorldPartitionLayoutBuilder::WorldPartitionLayoutBuilder(
        const WorldDescription& world
    )
        : impl_(std::make_unique<Impl>())
    {
        impl_->world = &world;
        impl_->assigned_objects.reserve(world.objectCount());
    }

    WorldPartitionLayoutBuilder::~WorldPartitionLayoutBuilder() = default;
    WorldPartitionLayoutBuilder::WorldPartitionLayoutBuilder(
        WorldPartitionLayoutBuilder&&
    ) noexcept = default;
    WorldPartitionLayoutBuilder& WorldPartitionLayoutBuilder::operator=(
        WorldPartitionLayoutBuilder&&
    ) noexcept = default;

    lux::cxx::expected<void, WorldPartitionFailure>
    WorldPartitionLayoutBuilder::addPartition(
        WorldPartitionId id,
        std::span<const WorldObjectId> objects
    ) noexcept
    {
        if (!id.valid())
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::INVALID_PARTITION_ID,
                {},
                id
            ));
        if (objects.empty())
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::EMPTY_PARTITION,
                {},
                id
            ));
        if (impl_->partition_ids.contains(id))
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::DUPLICATE_PARTITION_ID,
                {},
                id
            ));
        if (detail::consumeWorldFailureForTest(
                detail::EWorldFailurePoint::PARTITION_MUTATION_ALLOCATION
            ))
        {
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::ALLOCATION_FAILURE,
                {},
                id
            ));
        }

        try
        {
            std::vector<WorldObjectId> copied(objects.begin(), objects.end());
            std::sort(copied.begin(), copied.end(), WorldObjectIdLess{});
            for (std::size_t index{}; index < copied.size(); ++index)
            {
                const WorldObjectId object = copied[index];
                if (!impl_->world->findObject(object))
                {
                    return lux::cxx::unexpected(partitionFailure(
                        EWorldPartitionError::UNKNOWN_OBJECT,
                        object,
                        id
                    ));
                }
                if ((index != 0U && copied[index - 1U] == object) ||
                    impl_->assigned_objects.contains(object))
                {
                    return lux::cxx::unexpected(partitionFailure(
                        EWorldPartitionError::DUPLICATE_OBJECT_ASSIGNMENT,
                        object,
                        id
                    ));
                }
            }

            impl_->partitions.reserve(impl_->partitions.size() + 1U);
            impl_->partition_ids.reserve(impl_->partition_ids.size() + 1U);
            impl_->assigned_objects.reserve(
                impl_->assigned_objects.size() + copied.size()
            );

            bool partition_inserted = false;
            std::size_t assigned_inserted{};
            try
            {
                partition_inserted = impl_->partition_ids.insert(id).second;
                for (; assigned_inserted < copied.size(); ++assigned_inserted)
                {
                    const bool inserted = impl_->assigned_objects.insert(
                        copied[assigned_inserted]
                    ).second;
                    if (!inserted)
                        break;
                }
                if (!partition_inserted || assigned_inserted != copied.size())
                {
                    for (std::size_t rollback{}; rollback < assigned_inserted; ++rollback)
                        impl_->assigned_objects.erase(copied[rollback]);
                    if (partition_inserted)
                        impl_->partition_ids.erase(id);
                    const WorldObjectId duplicate = assigned_inserted < copied.size()
                        ? copied[assigned_inserted]
                        : WorldObjectId{};
                    return lux::cxx::unexpected(partitionFailure(
                        EWorldPartitionError::DUPLICATE_OBJECT_ASSIGNMENT,
                        duplicate,
                        id
                    ));
                }

                impl_->partitions.push_back(Impl::PendingPartition{
                    id,
                    std::move(copied)}
                );
            }
            catch (...)
            {
                for (std::size_t rollback{}; rollback < assigned_inserted; ++rollback)
                    impl_->assigned_objects.erase(copied[rollback]);
                if (partition_inserted)
                    impl_->partition_ids.erase(id);
                throw;
            }
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::ALLOCATION_FAILURE,
                {},
                id
            ));
        }
    }

    lux::cxx::expected<WorldPartitionLayout, WorldPartitionFailure>
    WorldPartitionLayoutBuilder::build() && noexcept
    {
        if (impl_->assigned_objects.size() != impl_->world->objectCount())
        {
            for (std::size_t index{}; index < impl_->world->objectCount(); ++index)
            {
                const WorldObjectId object = impl_->world->objectAt(index).id();
                if (!impl_->assigned_objects.contains(object))
                {
                    return lux::cxx::unexpected(partitionFailure(
                        EWorldPartitionError::MISSING_OBJECT_ASSIGNMENT,
                        object
                    ));
                }
            }
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::MISSING_OBJECT_ASSIGNMENT
            ));
        }
        if (detail::consumeWorldFailureForTest(
                detail::EWorldFailurePoint::PARTITION_BUILD_ALLOCATION
            ))
        {
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::ALLOCATION_FAILURE
            ));
        }
        if (detail::consumeWorldFailureForTest(
                detail::EWorldFailurePoint::PARTITION_BUILD_SIZE_OVERFLOW
            ) ||
            impl_->partitions.size() >
                std::numeric_limits<std::uint32_t>::max())
        {
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::SIZE_OVERFLOW
            ));
        }

        try
        {
            WorldPartitionLayout result;
            std::sort(
                impl_->partitions.begin(),
                impl_->partitions.end(),
                [](const Impl::PendingPartition& left,
                   const Impl::PendingPartition& right) noexcept
                {
                    return WorldPartitionIdLess{}(left.id, right.id);
                }
            );

            result.partitions_.reserve(impl_->partitions.size());
            result.objects_.reserve(impl_->world->objectCount());
            for (auto& pending : impl_->partitions)
            {
                WorldPartitionLayout::PartitionRecord record;
                record.id = pending.id;
                record.first_object = result.objects_.size();
                record.object_count = pending.objects.size();
                result.objects_.insert(
                    result.objects_.end(),
                    pending.objects.begin(),
                    pending.objects.end()
                );
                result.partitions_.push_back(record);
            }
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::ALLOCATION_FAILURE
            ));
        }
    }

    lux::cxx::expected<WorldPartitionBuildProduct, WorldPartitionFailure>
    WorldPartitionBuildProduct::build(
        WorldPartitionerDescriptor partitioner,
        WorldPartitionLayout layout,
        std::vector<WorldPartitionIndexArtifact> indexes
    ) noexcept
    {
        if (!partitioner.id.valid())
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::INVALID_PARTITIONER_ID
            ));
        if (partitioner.version == 0U)
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::INVALID_PARTITIONER_VERSION
            ));

        try
        {
            std::sort(
                indexes.begin(),
                indexes.end(),
                [](const WorldPartitionIndexArtifact& left,
                   const WorldPartitionIndexArtifact& right) noexcept
                {
                    return left.type.hash < right.type.hash ||
                        (left.type.hash == right.type.hash &&
                         left.type.name < right.type.name);
                }
            );
            for (std::size_t index{}; index < indexes.size(); ++index)
            {
                if (!indexes[index].type.valid())
                {
                    return lux::cxx::unexpected(partitionFailure(
                        EWorldPartitionError::INVALID_INDEX_TYPE,
                        {},
                        {},
                        indexes[index].type
                    ));
                }
                if (indexes[index].version == 0U)
                {
                    return lux::cxx::unexpected(partitionFailure(
                        EWorldPartitionError::INVALID_INDEX_VERSION,
                        {},
                        {},
                        indexes[index].type
                    ));
                }
                if (index != 0U && indexes[index - 1U].type == indexes[index].type)
                {
                    return lux::cxx::unexpected(partitionFailure(
                        EWorldPartitionError::DUPLICATE_INDEX_TYPE,
                        {},
                        {},
                        indexes[index].type
                    ));
                }
                if (index != 0U &&
                    indexes[index - 1U].type.hash == indexes[index].type.hash &&
                    indexes[index - 1U].type.name != indexes[index].type.name)
                {
                    return lux::cxx::unexpected(partitionFailure(
                        EWorldPartitionError::INVALID_INDEX_TYPE,
                        {},
                        {},
                        indexes[index].type
                    ));
                }
            }

            return WorldPartitionBuildProduct(
                std::move(partitioner),
                std::move(layout),
                std::move(indexes)
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(partitionFailure(
                EWorldPartitionError::ALLOCATION_FAILURE
            ));
        }
    }

    WorldPartitionBuildProduct::WorldPartitionBuildProduct(
        WorldPartitionerDescriptor partitioner,
        WorldPartitionLayout layout,
        std::vector<WorldPartitionIndexArtifact> indexes
    ) noexcept
        : partitioner_(std::move(partitioner)),
          layout_(std::move(layout)),
          indexes_(std::move(indexes))
    {
    }

    const WorldPartitionerDescriptor&
    WorldPartitionBuildProduct::partitioner() const noexcept
    {
        return partitioner_;
    }

    const WorldPartitionLayout& WorldPartitionBuildProduct::layout() const noexcept
    {
        return layout_;
    }

    std::span<const WorldPartitionIndexArtifact>
    WorldPartitionBuildProduct::indexes() const noexcept
    {
        return indexes_;
    }

    const WorldPartitionIndexArtifact* WorldPartitionBuildProduct::findIndex(
        const WorldPartitionIndexTypeId& type
    ) const noexcept
    {
        if (!type.valid())
            return nullptr;
        const auto iterator = std::lower_bound(
            indexes_.begin(),
            indexes_.end(),
            type,
            [](const WorldPartitionIndexArtifact& artifact,
               const WorldPartitionIndexTypeId& value) noexcept
            {
                return artifact.type.hash < value.hash ||
                    (artifact.type.hash == value.hash &&
                     artifact.type.name < value.name);
            }
        );
        return iterator != indexes_.end() && iterator->type == type
            ? &*iterator
            : nullptr;
    }
} // namespace lux::world
