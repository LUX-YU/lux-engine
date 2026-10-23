#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace lux::system
{
    enum class ESystemDataKind : std::uint8_t
    {
        SCHEMA,
        PREPARED_DATA
    };

    // Publication boundaries shared by Engine systems; no World format or Render type participates.
    enum class ESystemDataPhase : std::uint8_t
    {
        SIMULATION,
        SYNCHRONIZATION,
        STABLE,
        PUBLICATION
    };

    // A cold semantic requirement, not an ECS access declaration or a runtime readiness flag.
    // SCHEMA describes allowed data even when there are no entities of that type.
    struct SystemDataContract final
    {
        std::string_view name;
        std::uint32_t version{1};
        ESystemDataKind kind{ESystemDataKind::SCHEMA};
        ESystemDataPhase phase{ESystemDataPhase::SIMULATION};

        friend constexpr bool operator==(const SystemDataContract&, const SystemDataContract&) = default;
    };

    [[nodiscard]] constexpr bool validDataContracts(std::span<const SystemDataContract> values) noexcept
    {
        for (std::size_t index{}; index < values.size(); ++index)
        {
            const auto& value = values[index];
            const bool is_invalid_identity = value.name.empty() || value.version == 0;
            const bool is_invalid_kind =
                value.kind != ESystemDataKind::SCHEMA && value.kind != ESystemDataKind::PREPARED_DATA;
            const bool is_invalid_phase = value.phase > ESystemDataPhase::PUBLICATION;
            if (is_invalid_identity || is_invalid_kind || is_invalid_phase)
            {
                return false;
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (values[previous].kind == value.kind && values[previous].name == value.name)
                {
                    return false;
                }
            }
        }
        return true;
    }
} // namespace lux::system
