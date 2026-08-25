#pragma once

#include <lux/engine/ecs/world_section/visibility.h>

#include <cstdint>

namespace lux::ecs
{
    [[nodiscard]] LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC
    std::uint32_t worldSectionFormatVersion() noexcept;

    [[nodiscard]] LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC
    std::uint32_t worldSectionLoaderContractVersion() noexcept;
} // namespace lux::ecs
