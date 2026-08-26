#pragma once

#include <lux/engine/simulation/SimulationDescription.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::simulation
{
    enum class ESimulationDescriptionError : std::uint8_t
    {
        INVALID_SCHEMA_ID,
        SCHEMA_HASH_COLLISION,
        INVALID_SCHEMA_VERSION,
        DUPLICATE_DATA,
        DATA_NOT_FOUND,
        SIZE_OVERFLOW,
        ALLOCATION_FAILURE,
    };

    struct SimulationDescriptionFailure final
    {
        ESimulationDescriptionError code{
            ESimulationDescriptionError::ALLOCATION_FAILURE};
        SimulationDataSchemaId schema;
    };

    class LUX_ENGINE_SIMULATION_DESCRIPTION_PUBLIC
        SimulationDescriptionBuilder final
    {
      public:
        SimulationDescriptionBuilder();
        ~SimulationDescriptionBuilder();
        SimulationDescriptionBuilder(SimulationDescriptionBuilder&&) noexcept;
        SimulationDescriptionBuilder& operator=(
            SimulationDescriptionBuilder&&
        ) noexcept;

        SimulationDescriptionBuilder(const SimulationDescriptionBuilder&) = delete;
        SimulationDescriptionBuilder& operator=(
            const SimulationDescriptionBuilder&
        ) = delete;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        addData(
            SimulationDataSchemaId schema,
            std::uint32_t version,
            std::span<const std::byte> payload
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        setData(
            SimulationDataSchemaId schema,
            std::uint32_t version,
            std::span<const std::byte> payload
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, SimulationDescriptionFailure>
        eraseData(const SimulationDataSchemaId& schema) noexcept;

        void clear() noexcept;

        [[nodiscard]] lux::cxx::expected<
            SimulationDescription,
            SimulationDescriptionFailure>
        build() && noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
