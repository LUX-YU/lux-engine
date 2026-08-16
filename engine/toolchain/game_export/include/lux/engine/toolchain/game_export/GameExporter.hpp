#pragma once

#include <lux/engine/toolchain/game_export/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace lux::toolchain
{
    /** Deployment target. This is never inferred from the build host. */
    enum class ETargetPlatform : std::uint8_t
    {
        UNSPECIFIED,
        WINDOWS,
        LINUX,
        MACOS,
        ANDROID
    };

    /**
     * How the final application binary enters assembly.
     *
     * REFERENCE_PLAYER is today's compatibility path. PREBUILT_BINARY allows
     * a separately compiled game host to enter the same deployment pipeline.
     * NATIVE_PROJECT reserves the future compile/link pipeline and currently
     * fails loudly instead of silently falling back to Player.
     */
    enum class EGameBinaryMode : std::uint8_t
    {
        REFERENCE_PLAYER,
        PREBUILT_BINARY,
        NATIVE_PROJECT
    };

    enum class EGameExportError : std::uint8_t
    {
        INVALID_ARGUMENT,
        PROJECT_INVALID,
        OUTPUT_NOT_EMPTY,
        COOK_RECEIPT_INVALID,
        TARGET_PLATFORM_UNSUPPORTED,
        BINARY_MODE_UNSUPPORTED,
        BINARY_CUSTOMIZATION_UNSUPPORTED,
        RUNTIME_INVENTORY_MISSING,
        RUNTIME_FILE_MISSING,
        FORBIDDEN_PRODUCT_DEPENDENCY,
        COOK_FAILED,
        AUTHORING_PAYLOAD_PRESENT,
        EXTENSION_INVALID,
        EXTENSION_INVENTORY_MISSING,
        EXTENSION_DEPENDENCY_MISSING,
        FILESYSTEM_ERROR,
        MANIFEST_WRITE_FAILED
    };

    struct GameExportFailure final
    {
        EGameExportError code{EGameExportError::INVALID_ARGUMENT};
        std::string      detail;
    };

    struct GameBinaryMetadata final
    {
        std::string display_name;
        std::string version;
        std::string publisher;
        std::string description;
    };

    struct GameBinaryRecipe final
    {
        EGameBinaryMode mode{EGameBinaryMode::REFERENCE_PLAYER};

        // PREBUILT_BINARY inputs. The inventory uses the same one-filename-per-
        // line contract as CMake's *.runtime-files output.
        std::filesystem::path binary_file;
        std::filesystem::path runtime_inventory;

        // Reserved NATIVE_PROJECT inputs. They are part of the stable seam but
        // are rejected until the native compile/link adapter is implemented.
        std::filesystem::path native_project_file;
        std::filesystem::path icon_file;
        GameBinaryMetadata    metadata;
    };

    struct GameCookRequest final
    {
        std::filesystem::path project_file;
        std::filesystem::path output_directory;
    };

    struct GameCookReport final
    {
        std::filesystem::path cook_receipt;
        std::filesystem::path game_pak;
        std::string           binary_name;
        std::size_t           asset_count{0u};
        std::uintmax_t        payload_bytes{0u};
    };

    struct GameAssemblyRequest final
    {
        std::filesystem::path cooked_directory;
        std::filesystem::path runtime_root;
        std::filesystem::path output_directory;
        ETargetPlatform       target_platform{ETargetPlatform::UNSPECIFIED};
        std::string           configuration;
        GameBinaryRecipe      binary;
    };

    struct GameAssemblyReport final
    {
        std::filesystem::path executable;
        std::filesystem::path runtime_manifest;
        std::filesystem::path game_pak;
        std::size_t           runtime_file_count{0u};
        std::size_t           extension_count{0u};
    };

    struct GameExportRequest final
    {
        std::filesystem::path project_file;
        std::filesystem::path runtime_root;
        std::filesystem::path output_directory;
        ETargetPlatform       target_platform{ETargetPlatform::UNSPECIFIED};
        std::string           configuration;
        GameBinaryRecipe      binary;
    };

    struct GameExportReport final
    {
        std::filesystem::path executable;
        std::filesystem::path runtime_manifest;
        std::filesystem::path game_pak;
        std::size_t           asset_count{0u};
        std::uintmax_t        payload_bytes{0u};
        std::size_t           runtime_file_count{0u};
        std::size_t           extension_count{0u};
    };

    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_GAME_EXPORT_PUBLIC
    std::string_view targetPlatformName(ETargetPlatform platform) noexcept;

    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_GAME_EXPORT_PUBLIC
    std::optional<ETargetPlatform> parseTargetPlatform(
        std::string_view name) noexcept;

    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_GAME_EXPORT_PUBLIC
    lux::cxx::expected<GameCookReport, GameExportFailure>
    cookGame(const GameCookRequest& request) noexcept;

    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_GAME_EXPORT_PUBLIC
    lux::cxx::expected<GameAssemblyReport, GameExportFailure>
    assembleGame(const GameAssemblyRequest& request) noexcept;

    /** Convenience composition of cookGame() followed by assembleGame(). */
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_GAME_EXPORT_PUBLIC
    lux::cxx::expected<GameExportReport, GameExportFailure>
    exportGame(const GameExportRequest& request) noexcept;
} // namespace lux::toolchain
