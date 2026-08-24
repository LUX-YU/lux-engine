#pragma once

#include <lux/game/visibility.h>

#include <lux/engine/extensions/ExtensionAbi.hpp>
#include <lux/engine/function/render/Capacity.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lux::game
{
    struct ExtensionRequirement final
    {
        lux::extensions::ExtensionId id;
        std::filesystem::path path;
        std::uint16_t required_major{0u};
        std::uint16_t minimum_minor{0u};
    };

    /**
     * Cooked game-launch contract shared by exporters and game front ends.
     *
     * The manifest describes only deployable game inputs. It contains no
     * authoring project, editor state or build-host paths.
     */
    struct LUX_GAME_PUBLIC LaunchManifest final
    {
        static constexpr std::uint32_t kSchemaVersion = 5u;
        static constexpr std::uint32_t kLegacySchemaVersion = 4u;

        std::string title{"Lux Player"};
        std::filesystem::path game_pak;
        std::filesystem::path base_pak;
        std::string boot_scene;
        lux::render::CapacityRequest render_capacity{};
        std::vector<ExtensionRequirement> extensions;

        [[nodiscard]] static lux::cxx::expected<LaunchManifest, std::string>
        loadFromFile(const std::filesystem::path& path) noexcept;

        [[nodiscard]] lux::cxx::expected<void, std::string>
        saveToFile(const std::filesystem::path& path) const noexcept;
    };
} // namespace lux::game
