#pragma once

#include <lux/engine/simulation/SimulationDataSchemaId.hpp>
#include <lux/engine/simulation/description/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::simulation
{
    class SimulationDescription;
    class SimulationDescriptionBuilder;

    class LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC SimulationDataView final
    {
      public:
        SimulationDataView() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return description_ != nullptr;
        }

        [[nodiscard]] const SimulationDataSchemaId& schema() const noexcept;
        [[nodiscard]] std::uint32_t version() const noexcept;
        [[nodiscard]] std::span<const std::byte> payload() const noexcept;

      private:
        SimulationDataView(
            const SimulationDescription& description,
            std::size_t data_index
        ) noexcept;

        const SimulationDescription* description_{};
        std::size_t data_index_{};

        friend class SimulationDescription;
    };

    class LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC SimulationDescription final
    {
      public:
        SimulationDescription() noexcept = default;
        SimulationDescription(SimulationDescription&&) noexcept = default;
        SimulationDescription& operator=(SimulationDescription&&) noexcept = default;
        ~SimulationDescription() = default;

        SimulationDescription(const SimulationDescription&) = delete;
        SimulationDescription& operator=(const SimulationDescription&) = delete;

        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] std::size_t dataCount() const noexcept;
        [[nodiscard]] std::size_t payloadBytes() const noexcept;
        [[nodiscard]] std::span<const SimulationDataSchemaId> schemas() const noexcept;
        [[nodiscard]] SimulationDataView dataAt(std::size_t index) const noexcept;
        [[nodiscard]] SimulationDataView findData(
            const SimulationDataSchemaId& schema
        ) const noexcept;

      private:
        struct DataRecord final
        {
            std::size_t schema_ordinal{};
            std::uint32_t version{};
            std::size_t payload_offset{};
            std::size_t payload_size{};
        };

        std::vector<SimulationDataSchemaId> schemas_;
        std::vector<DataRecord> data_;
        std::vector<std::byte> payload_;

        friend class SimulationDataView;
        friend class SimulationDescriptionBuilder;
    };
}
