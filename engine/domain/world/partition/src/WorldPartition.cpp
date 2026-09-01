#include <lux/engine/world/WorldPartition.hpp>
#include <lux/engine/world/detail/WorldPartitionFailureInjection.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

namespace lux::world
{
    using partition::PartitionIndexTypeId;
    using partition::PartitionOrdinal;

    std::span<const WorldPartitionTablePageDescription> WorldPartitionTable::pages() const noexcept
    {
        return pages_;
    }

    const WorldPartitionTablePageDescription*
    WorldPartitionTable::findPage(PartitionOrdinal partition) const noexcept
    {
        const auto iterator = std::upper_bound(
            pages_.begin(),
            pages_.end(),
            partition.value,
            [](std::uint32_t value, const WorldPartitionTablePageDescription& page) noexcept {
                return value < page.first.value;
            }
        );
        if (iterator == pages_.begin())
            return nullptr;

        const auto& page = *std::prev(iterator);
        const std::uint64_t end = static_cast<std::uint64_t>(page.first.value) + page.count;
        return partition.value >= page.first.value && partition.value < end ? &page : nullptr;
    }

    WorldPartitionView::WorldPartitionView(const WorldPartitionLayout& layout, std::size_t partition_index) noexcept
        : layout_(&layout), partition_index_(partition_index)
    {
    }

    PartitionOrdinal WorldPartitionView::ordinal() const noexcept
    {
        return PartitionOrdinal{static_cast<std::uint32_t>(partition_index_)};
    }

    WorldPartitionId WorldPartitionView::id() const noexcept
    {
        return layout_->partitions_[partition_index_].id;
    }

    std::span<const WorldObjectId> WorldPartitionView::objects() const noexcept
    {
        const auto& record = layout_->partitions_[partition_index_];
        return std::span<const WorldObjectId>(layout_->objects_).subspan(record.first_object, record.object_count);
    }

    bool WorldPartitionLayout::empty() const noexcept
    {
        return partitions_.empty();
    }

    std::size_t WorldPartitionLayout::partitionCount() const noexcept
    {
        return partitions_.size();
    }

    WorldPartitionView WorldPartitionLayout::partitionAt(std::size_t index) const noexcept
    {
        return index < partitions_.size() ? WorldPartitionView(*this, index) : WorldPartitionView{};
    }

    WorldPartitionView WorldPartitionLayout::findPartition(WorldPartitionId id) const noexcept
    {
        if (!id.valid())
            return {};
        const auto iterator = std::lower_bound(
            partitions_.begin(),
            partitions_.end(),
            id,
            [](const PartitionRecord& record, const WorldPartitionId& value) {
                return WorldPartitionIdLess{}(record.id, value);
            }
        );
        if (iterator == partitions_.end() || iterator->id != id)
            return {};
        return WorldPartitionView(*this, static_cast<std::size_t>(std::distance(partitions_.begin(), iterator)));
    }

    struct WorldPartitionLayoutBuilder::Impl final
    {
        struct PendingPartition final
        {
            WorldPartitionId id;
            std::vector<WorldObjectId> objects;
        };

        std::vector<WorldObjectId> objects;
        std::vector<PendingPartition> partitions;
        std::unordered_set<WorldPartitionId, WorldPartitionIdHash> partition_ids;
        std::unordered_set<WorldObjectId, WorldObjectIdHash> assigned_objects;
        EWorldPartitionError universe_error{EWorldPartitionError::IMPLEMENTATION_FAILURE};
        WorldObjectId universe_error_object;
        bool invalid_universe{};
    };

    namespace
    {
        template <class Rollback>
        class MutationRollback final
        {
        public:
            explicit MutationRollback(Rollback rollback) noexcept : rollback_(std::move(rollback))
            {
            }

            MutationRollback(const MutationRollback&) = delete;
            MutationRollback& operator=(const MutationRollback&) = delete;

            ~MutationRollback()
            {
                if (active_)
                    rollback_();
            }

            void commit() noexcept
            {
                active_ = false;
            }

        private:
            Rollback rollback_;
            bool active_{true};
        };

        [[nodiscard]] WorldPartitionFailure partitionFailure(
            EWorldPartitionError code,
            WorldObjectId object = {},
            WorldPartitionId partition = {},
            PartitionIndexTypeId index_type = {}
        ) noexcept
        {
            return WorldPartitionFailure{code, object, partition, std::move(index_type), 0U};
        }
    } // namespace

    WorldPartitionLayoutBuilder::WorldPartitionLayoutBuilder(std::span<const WorldObjectId> objects)
        : impl_(std::make_unique<Impl>())
    {
        impl_->objects.assign(objects.begin(), objects.end());
        std::sort(impl_->objects.begin(), impl_->objects.end(), WorldObjectIdLess{});
        for (std::size_t index{}; index < impl_->objects.size(); ++index)
        {
            if (!impl_->objects[index].valid())
            {
                impl_->invalid_universe = true;
                impl_->universe_error = EWorldPartitionError::INVALID_OBJECT_ID;
                impl_->universe_error_object = impl_->objects[index];
                break;
            }
            if (index != 0U && impl_->objects[index - 1U] == impl_->objects[index])
            {
                impl_->invalid_universe = true;
                impl_->universe_error = EWorldPartitionError::DUPLICATE_OBJECT_ID;
                impl_->universe_error_object = impl_->objects[index];
                break;
            }
        }
        impl_->assigned_objects.reserve(impl_->objects.size());
    }

    WorldPartitionLayoutBuilder::~WorldPartitionLayoutBuilder() = default;
    WorldPartitionLayoutBuilder::WorldPartitionLayoutBuilder(WorldPartitionLayoutBuilder&&) noexcept = default;
    WorldPartitionLayoutBuilder&
    WorldPartitionLayoutBuilder::operator=(WorldPartitionLayoutBuilder&&) noexcept = default;

    lux::cxx::expected<void, WorldPartitionFailure>
    WorldPartitionLayoutBuilder::addPartition(WorldPartitionId id, std::span<const WorldObjectId> objects) noexcept
    {
        if (impl_->invalid_universe)
        {
            return lux::cxx::unexpected(
                partitionFailure(impl_->universe_error, impl_->universe_error_object)
            );
        }
        if (!id.valid())
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::INVALID_PARTITION_ID, {}, id));
        if (objects.empty())
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::EMPTY_PARTITION, {}, id));
        if (impl_->partition_ids.contains(id))
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::DUPLICATE_PARTITION_ID, {}, id));
        if (detail::consumeWorldPartitionFailureForTest(
                detail::EWorldPartitionFailurePoint::MUTATION_ALLOCATION))
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::ALLOCATION_FAILURE, {}, id));

        try
        {
            std::vector<WorldObjectId> copied(objects.begin(), objects.end());
            std::sort(copied.begin(), copied.end(), WorldObjectIdLess{});
            for (std::size_t index{}; index < copied.size(); ++index)
            {
                const WorldObjectId object = copied[index];
                if (!std::binary_search(
                        impl_->objects.begin(),
                        impl_->objects.end(),
                        object,
                        WorldObjectIdLess{}
                    ))
                {
                    return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::UNKNOWN_OBJECT, object, id));
                }
                if ((index != 0U && copied[index - 1U] == object) || impl_->assigned_objects.contains(object))
                {
                    return lux::cxx::unexpected(
                        partitionFailure(EWorldPartitionError::DUPLICATE_OBJECT_ASSIGNMENT, object, id)
                    );
                }
            }

            const bool is_partition_count_overflow =
                impl_->partitions.size() == std::numeric_limits<std::size_t>::max();
            const bool is_partition_id_count_overflow =
                impl_->partition_ids.size() == std::numeric_limits<std::size_t>::max();
            const bool is_assignment_count_overflow =
                copied.size() > std::numeric_limits<std::size_t>::max() - impl_->assigned_objects.size();
            const bool is_size_overflow = is_partition_count_overflow ||
                is_partition_id_count_overflow ||
                is_assignment_count_overflow;
            if (is_size_overflow)
                return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::SIZE_OVERFLOW, {}, id));

            impl_->partitions.reserve(impl_->partitions.size() + 1U);
            impl_->partition_ids.reserve(impl_->partition_ids.size() + 1U);
            impl_->assigned_objects.reserve(impl_->assigned_objects.size() + copied.size());

            bool partition_inserted = false;
            std::size_t assigned_inserted{};
            MutationRollback rollback([&]() noexcept {
                for (std::size_t rollback_index{}; rollback_index < assigned_inserted; ++rollback_index)
                    impl_->assigned_objects.erase(copied[rollback_index]);
                if (partition_inserted)
                    impl_->partition_ids.erase(id);
            });

            partition_inserted = impl_->partition_ids.insert(id).second;
            for (; assigned_inserted < copied.size(); ++assigned_inserted)
            {
                if (!impl_->assigned_objects.insert(copied[assigned_inserted]).second)
                    break;
            }
            if (!partition_inserted || assigned_inserted != copied.size())
            {
                const WorldObjectId duplicate =
                    assigned_inserted < copied.size() ? copied[assigned_inserted] : WorldObjectId{};
                return lux::cxx::unexpected(
                    partitionFailure(EWorldPartitionError::DUPLICATE_OBJECT_ASSIGNMENT, duplicate, id)
                );
            }

            impl_->partitions.push_back(Impl::PendingPartition{id, std::move(copied)});
            rollback.commit();
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::ALLOCATION_FAILURE, {}, id));
        }
    }

    lux::cxx::expected<WorldPartitionLayout, WorldPartitionFailure> WorldPartitionLayoutBuilder::build() && noexcept
    {
        if (impl_->invalid_universe)
        {
            return lux::cxx::unexpected(
                partitionFailure(impl_->universe_error, impl_->universe_error_object)
            );
        }
        if (impl_->assigned_objects.size() != impl_->objects.size())
        {
            for (const WorldObjectId object : impl_->objects)
            {
                if (!impl_->assigned_objects.contains(object))
                {
                    return lux::cxx::unexpected(
                        partitionFailure(EWorldPartitionError::MISSING_OBJECT_ASSIGNMENT, object)
                    );
                }
            }
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::MISSING_OBJECT_ASSIGNMENT));
        }
        if (detail::consumeWorldPartitionFailureForTest(detail::EWorldPartitionFailurePoint::BUILD_ALLOCATION))
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::ALLOCATION_FAILURE));
        if (detail::consumeWorldPartitionFailureForTest(detail::EWorldPartitionFailurePoint::BUILD_SIZE_OVERFLOW) ||
            impl_->partitions.size() > std::numeric_limits<std::uint32_t>::max())
        {
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::SIZE_OVERFLOW));
        }

        try
        {
            WorldPartitionLayout result;
            std::sort(
                impl_->partitions.begin(),
                impl_->partitions.end(),
                [](const Impl::PendingPartition& left, const Impl::PendingPartition& right) noexcept {
                    return WorldPartitionIdLess{}(left.id, right.id);
                }
            );

            result.partitions_.reserve(impl_->partitions.size());
            result.objects_.reserve(impl_->objects.size());
            for (auto& pending : impl_->partitions)
            {
                WorldPartitionLayout::PartitionRecord record;
                record.id = pending.id;
                record.first_object = result.objects_.size();
                record.object_count = pending.objects.size();
                result.objects_.insert(result.objects_.end(), pending.objects.begin(), pending.objects.end());
                result.partitions_.push_back(record);
            }
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<WorldPartitionBuildProduct, WorldPartitionFailure> WorldPartitionBuildProduct::build(
        WorldPartitionerDescriptor partitioner,
        WorldPartitionLayout layout,
        std::vector<WorldPartitionIndexArtifact> indexes
    ) noexcept
    {
        if (!partitioner.id.valid())
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::INVALID_PARTITIONER_ID));
        if (partitioner.version == 0U)
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::INVALID_PARTITIONER_VERSION));

        try
        {
            std::sort(
                indexes.begin(),
                indexes.end(),
                [](const WorldPartitionIndexArtifact& left, const WorldPartitionIndexArtifact& right) noexcept {
                    return left.type.hash < right.type.hash ||
                           (left.type.hash == right.type.hash && left.type.name < right.type.name);
                }
            );
            for (std::size_t index{}; index < indexes.size(); ++index)
            {
                if (!indexes[index].type.valid())
                {
                    return lux::cxx::unexpected(
                        partitionFailure(EWorldPartitionError::INVALID_INDEX_TYPE, {}, {}, indexes[index].type)
                    );
                }
                if (indexes[index].version == 0U)
                {
                    return lux::cxx::unexpected(
                        partitionFailure(EWorldPartitionError::INVALID_INDEX_VERSION, {}, {}, indexes[index].type)
                    );
                }
                if (index != 0U && indexes[index - 1U].type == indexes[index].type)
                {
                    return lux::cxx::unexpected(
                        partitionFailure(EWorldPartitionError::DUPLICATE_INDEX_TYPE, {}, {}, indexes[index].type)
                    );
                }
                if (index != 0U && indexes[index - 1U].type.hash == indexes[index].type.hash &&
                    indexes[index - 1U].type.name != indexes[index].type.name)
                {
                    return lux::cxx::unexpected(
                        partitionFailure(EWorldPartitionError::INVALID_INDEX_TYPE, {}, {}, indexes[index].type)
                    );
                }
            }

            return WorldPartitionBuildProduct(std::move(partitioner), std::move(layout), std::move(indexes));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(partitionFailure(EWorldPartitionError::ALLOCATION_FAILURE));
        }
    }

    WorldPartitionBuildProduct::WorldPartitionBuildProduct(
        WorldPartitionerDescriptor partitioner,
        WorldPartitionLayout layout,
        std::vector<WorldPartitionIndexArtifact> indexes
    ) noexcept
        : partitioner_(std::move(partitioner)), layout_(std::move(layout)), indexes_(std::move(indexes))
    {
    }

    const WorldPartitionerDescriptor& WorldPartitionBuildProduct::partitioner() const noexcept
    {
        return partitioner_;
    }

    const WorldPartitionLayout& WorldPartitionBuildProduct::layout() const noexcept
    {
        return layout_;
    }

    std::span<const WorldPartitionIndexArtifact> WorldPartitionBuildProduct::indexes() const noexcept
    {
        return indexes_;
    }

    const WorldPartitionIndexArtifact*
    WorldPartitionBuildProduct::findIndex(const PartitionIndexTypeId& type) const noexcept
    {
        if (!type.valid())
            return nullptr;
        const auto iterator = std::lower_bound(
            indexes_.begin(),
            indexes_.end(),
            type,
            [](const WorldPartitionIndexArtifact& artifact, const PartitionIndexTypeId& value) noexcept {
                return artifact.type.hash < value.hash ||
                       (artifact.type.hash == value.hash && artifact.type.name < value.name);
            }
        );
        return iterator != indexes_.end() && iterator->type == type ? &*iterator : nullptr;
    }
} // namespace lux::world
