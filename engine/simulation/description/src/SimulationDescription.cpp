#include <lux/engine/simulation/SimulationDescription.hpp>

#include <algorithm>

namespace lux::simulation
{
    SimulationDataView::SimulationDataView(
        const SimulationDescription& description,
        std::size_t data_index
    ) noexcept
        : description_(&description), data_index_(data_index)
    {
    }

    const SimulationDataSchemaId& SimulationDataView::schema() const noexcept
    {
        return description_->schemas_[
            description_->data_[data_index_].schema_ordinal];
    }

    std::uint32_t SimulationDataView::version() const noexcept
    {
        return description_->data_[data_index_].version;
    }

    std::span<const std::byte> SimulationDataView::payload() const noexcept
    {
        const auto& record = description_->data_[data_index_];
        return std::span<const std::byte>(description_->payload_).subspan(
            record.payload_offset,
            record.payload_size
        );
    }

    bool SimulationDescription::empty() const noexcept
    {
        return data_.empty();
    }

    std::size_t SimulationDescription::dataCount() const noexcept
    {
        return data_.size();
    }

    std::size_t SimulationDescription::payloadBytes() const noexcept
    {
        return payload_.size();
    }

    std::span<const SimulationDataSchemaId>
    SimulationDescription::schemas() const noexcept
    {
        return schemas_;
    }

    SimulationDataView SimulationDescription::dataAt(
        std::size_t index
    ) const noexcept
    {
        return index < data_.size() ? SimulationDataView(*this, index)
                                    : SimulationDataView{};
    }

    SimulationDataView SimulationDescription::findData(
        const SimulationDataSchemaId& schema
    ) const noexcept
    {
        if (!schema.valid())
            return {};
        const auto iterator = std::lower_bound(
            schemas_.begin(),
            schemas_.end(),
            schema,
            SimulationDataSchemaIdLess{}
        );
        if (iterator == schemas_.end() || *iterator != schema)
            return {};
        return SimulationDataView(
            *this,
            static_cast<std::size_t>(
                std::distance(schemas_.begin(), iterator)
            )
        );
    }
}
