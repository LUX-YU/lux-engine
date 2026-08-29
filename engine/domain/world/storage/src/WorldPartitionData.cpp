#include <lux/engine/world/WorldPartitionData.hpp>

namespace lux::world
{
    WorldPartitionObjectView::WorldPartitionObjectView(
        const WorldPartitionData& data,
        std::size_t object_index
    ) noexcept
        : data_(&data), object_index_(object_index)
    {
    }

    WorldObjectId WorldPartitionObjectView::id() const noexcept
    {
        return data_->objects_[object_index_].id;
    }

    WorldBundleId WorldPartitionObjectView::bundle() const noexcept
    {
        return data_->bundle_;
    }

    WorldBundleGeneration WorldPartitionObjectView::generation() const noexcept
    {
        return data_->generation_;
    }

    std::size_t WorldPartitionObjectView::dataCount() const noexcept
    {
        return data_->objects_[object_index_].data_count;
    }

    std::uint32_t WorldPartitionObjectView::schemaOrdinalAt(std::size_t index) const noexcept
    {
        if (data_ == nullptr)
            return {};
        const auto& object = data_->objects_[object_index_];
        if (index >= object.data_count)
            return {};
        return data_->data_[object.first_data + index].schema_ordinal;
    }

    std::uint32_t WorldPartitionObjectView::schemaVersionAt(std::size_t index) const noexcept
    {
        if (data_ == nullptr)
            return {};
        const auto& object = data_->objects_[object_index_];
        if (index >= object.data_count)
            return {};
        return data_->data_[object.first_data + index].version;
    }

    std::span<const std::byte> WorldPartitionObjectView::payloadAt(std::size_t index) const noexcept
    {
        if (data_ == nullptr)
            return {};
        const auto& object = data_->objects_[object_index_];
        if (index >= object.data_count)
            return {};
        const auto& record = data_->data_[object.first_data + index];
        return std::span<const std::byte>(data_->payload_).subspan(
            record.payload_offset,
            record.payload_size
        );
    }

    WorldBundleId WorldPartitionData::bundle() const noexcept
    {
        return bundle_;
    }

    WorldBundleGeneration WorldPartitionData::generation() const noexcept
    {
        return generation_;
    }

    WorldPartitionOrdinal WorldPartitionData::partition() const noexcept
    {
        return partition_;
    }

    std::size_t WorldPartitionData::objectCount() const noexcept
    {
        return objects_.size();
    }

    WorldPartitionObjectView WorldPartitionData::objectAt(std::size_t index) const noexcept
    {
        return index < objects_.size() ? WorldPartitionObjectView(*this, index) : WorldPartitionObjectView{};
    }
} // namespace lux::world
