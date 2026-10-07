#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <lux/engine/scene/ScenePackage.hpp>
#include <stop_token>
#include <system_error>
#include <variant>

namespace lux::editor
{
    enum class ESceneFileError : std::uint8_t
    {
        INVALID_PATH,
        LIMIT,
        CANCELLED,
        IO,
        DESTINATION_EXISTS,
        PUBLICATION_UNKNOWN
    };
    struct SceneFileFailure final
    {
        ESceneFileError code{};
        std::error_code system;
    };
    enum class ESceneWrite : std::uint8_t
    {
        CREATE,
        REPLACE
    };
    using VSceneFileFailure = std::variant<SceneFileFailure, scene::ScenePackageFailure>;
    template <class T> using SceneFileResult = cxx::expected<T, VSceneFileFailure>;
    // IO wraps the existing ScenePackage codec. No project runtime or alternate scene format.
    [[nodiscard]] SceneFileResult<scene::ScenePackage> readScenePackageFile(
        const std::filesystem::path&,
        std::size_t max_bytes,
        std::stop_token = {}
    ) noexcept;
    [[nodiscard]] SceneFileResult<void> writeScenePackageAtomic(
        const std::filesystem::path&,
        const scene::ScenePackage&,
        ESceneWrite,
        std::size_t max_bytes,
        std::stop_token = {}
    ) noexcept;
} // namespace lux::editor
