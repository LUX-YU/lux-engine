#pragma once

#include <lux/engine/ecs/WorldSectionId.hpp>
#include <lux/engine/ecs/WorldSectionTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace lux::ecs::world_section::test
{
    [[nodiscard]] constexpr WorldSectionValidationBudget
    fixtureValidationBudget() noexcept
    {
        return WorldSectionValidationBudget{
            std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint64_t>::max(),
            std::numeric_limits<std::uint32_t>::max(),
            std::numeric_limits<std::size_t>::max(),
        };
    }

    [[nodiscard]] constexpr WorldSectionLoadScratchBudget
    fixtureLoadScratchBudget() noexcept
    {
        // Test policy, deliberately outside the L1 runtime contract.
        return WorldSectionLoadScratchBudget{256U * 1024U};
    }

    struct FixtureColumn final
    {
        std::string schema_name;
        std::uint32_t schema_version{1U};
        EWorldSectionValueEncoding value_encoding{
            EWorldSectionValueEncoding::TAG};
        EWorldSectionOrdinalEncoding ordinal_encoding{
            EWorldSectionOrdinalEncoding::DENSE};
        std::uint32_t fixed_stride{};
        std::vector<std::uint32_t> ordinals;
        std::vector<std::uint32_t> offsets;
        std::vector<std::byte> payload;
    };

    [[nodiscard]] std::vector<std::byte> buildFixture(
        WorldSectionId id,
        std::uint32_t entity_count,
        std::vector<FixtureColumn> columns
    );

    void patchU32(
        std::vector<std::byte>& bytes,
        std::size_t offset,
        std::uint32_t value
    );

    void patchU64(
        std::vector<std::byte>& bytes,
        std::size_t offset,
        std::uint64_t value
    );
} // namespace lux::ecs::world_section::test
