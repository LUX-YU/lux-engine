#include <lux/engine/world/WorldDescription.hpp>

#include <algorithm>

namespace lux::world
{
    WorldDataView::WorldDataView(
        const WorldDescription& world,
        std::size_t data_index
    ) noexcept
        : world_(&world), data_index_(data_index)
    {
    }

    const WorldDataSchemaId& WorldDataView::schema() const noexcept
    {
        const auto& record = world_->data_[data_index_];
        return world_->schemas_[record.schema_ordinal];
    }

    std::uint32_t WorldDataView::version() const noexcept
    {
        return world_->data_[data_index_].version;
    }

    std::span<const std::byte> WorldDataView::payload() const noexcept
    {
        const auto& record = world_->data_[data_index_];
        return std::span<const std::byte>(world_->payload_).subspan(
            record.payload_offset,
            record.payload_size
        );
    }

    WorldObjectView::WorldObjectView(
        const WorldDescription& world,
        std::size_t object_index
    ) noexcept
        : world_(&world), object_index_(object_index)
    {
    }

    WorldObjectId WorldObjectView::id() const noexcept
    {
        return world_->objects_[object_index_].id;
    }

    std::size_t WorldObjectView::dataCount() const noexcept
    {
        return world_->objects_[object_index_].data_count;
    }

    WorldDataView WorldObjectView::dataAt(std::size_t index) const noexcept
    {
        if (world_ == nullptr)
            return {};
        const auto& object = world_->objects_[object_index_];
        if (index >= object.data_count)
            return {};
        return WorldDataView(*world_, object.first_data + index);
    }

    WorldDataView WorldObjectView::findData(
        const WorldDataSchemaId& schema
    ) const noexcept
    {
        if (world_ == nullptr || !schema.valid())
            return {};

        const auto schema_it = std::lower_bound(
            world_->schemas_.begin(),
            world_->schemas_.end(),
            schema,
            WorldDataSchemaIdLess{}
        );
        if (schema_it == world_->schemas_.end() || *schema_it != schema)
            return {};

        const std::size_t schema_ordinal = static_cast<std::size_t>(
            std::distance(world_->schemas_.begin(), schema_it)
        );
        const auto& object = world_->objects_[object_index_];
        const auto begin = world_->data_.begin() +
            static_cast<std::ptrdiff_t>(object.first_data);
        const auto end = begin + static_cast<std::ptrdiff_t>(object.data_count);
        const auto data_it = std::lower_bound(
            begin,
            end,
            schema_ordinal,
            [](const WorldDescription::DataRecord& record, std::size_t value)
            {
                return record.schema_ordinal < value;
            }
        );
        if (data_it == end || data_it->schema_ordinal != schema_ordinal)
            return {};
        return WorldDataView(
            *world_,
            static_cast<std::size_t>(
                std::distance(world_->data_.begin(), data_it)
            )
        );
    }

    bool WorldDescription::empty() const noexcept
    {
        return objects_.empty();
    }

    std::size_t WorldDescription::objectCount() const noexcept
    {
        return objects_.size();
    }

    std::size_t WorldDescription::dataCount() const noexcept
    {
        return data_.size();
    }

    std::size_t WorldDescription::payloadBytes() const noexcept
    {
        return payload_.size();
    }

    std::span<const WorldDataSchemaId> WorldDescription::schemas() const noexcept
    {
        return schemas_;
    }

    WorldObjectView WorldDescription::objectAt(std::size_t index) const noexcept
    {
        return index < objects_.size() ? WorldObjectView(*this, index)
                                       : WorldObjectView{};
    }

    WorldObjectView WorldDescription::findObject(WorldObjectId id) const noexcept
    {
        if (!id.valid())
            return {};
        const auto iterator = std::lower_bound(
            objects_.begin(),
            objects_.end(),
            id,
            [](const ObjectRecord& object, const WorldObjectId& value)
            {
                return WorldObjectIdLess{}(object.id, value);
            }
        );
        if (iterator == objects_.end() || iterator->id != id)
            return {};
        return WorldObjectView(
            *this,
            static_cast<std::size_t>(std::distance(objects_.begin(), iterator))
        );
    }
}
