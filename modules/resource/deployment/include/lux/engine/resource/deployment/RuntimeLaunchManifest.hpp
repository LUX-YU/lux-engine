#pragma once

#include <lux/engine/resource/deployment/visibility.h>
#include <lux/engine/resource/deployment/RuntimeCapacity.hpp>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/core/extension_abi/ModuleAbi.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lux::deployment
{
    struct RuntimeExtensionEntry final
    {
        lux::extensions::ExtensionId id;
        std::filesystem::path         path;
        std::uint16_t                 required_major{0u};
        std::uint16_t                 minimum_minor{0u};
    };

    /**
     * Target-neutral cooked-product contract consumed by runtime hosts.
     *
     * This belongs to Resource rather than Runtime because both the Toolchain
     * producer and every runtime host must agree on the exact disk schema.
     * It deliberately contains no project GUID, source directory, import
     * setting, build-host fact or editor module.
     */
    struct LUX_ENGINE_RESOURCE_DEPLOYMENT_PUBLIC RuntimeLaunchManifest final
    {
        static constexpr std::uint32_t kSchemaVersion = 4u;

        std::string           title{"Lux Player"};
        std::filesystem::path game_pak;
        std::filesystem::path engine_pak;
        std::string           boot_scene;
        RuntimeCapacityRequest capacity{};
        std::vector<RuntimeExtensionEntry> extensions;

        [[nodiscard]] static lux::cxx::expected<
            RuntimeLaunchManifest,
            std::string>
        loadFromFile(const std::filesystem::path& path) noexcept;

        [[nodiscard]] lux::cxx::expected<void, std::string>
        saveToFile(const std::filesystem::path& path) const noexcept;
    };
} // namespace lux::deployment
