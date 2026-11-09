#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lux::scene
{
    // Owned field encoding; version zero and empty bytes designate a type without configuration.
    // Factories decode the selected type synchronously. No reflection metadata is borrowed here.
    struct SceneSystemDescription final
    {
        std::string            type;
        std::string            name;
        std::uint32_t          configuration_version{};
        std::vector<std::byte> configuration;
    };
} // namespace lux::scene
