#pragma once

#include <lux/engine/platform/FileIo.hpp>
#include <lux/engine/scene/ScenePackage.hpp>

namespace lux::scene
{
    using VSceneFileFailure = std::variant<platform::FileFailure, ScenePackageFailure>;
    template <class T> using SceneFileResult = cxx::expected<T, VSceneFileFailure>;

    /// Synchronous file read and decode. Decoded assets retain their immutable backing.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC SceneFileResult<ScenePackage> readScenePackageFile(
        const std::filesystem::path& path,
        std::size_t max_bytes,
        std::stop_token stop = {}
    ) noexcept;

    /// Synchronous encoding and atomic publication; cancellation never undoes a published file.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC SceneFileResult<void> writeScenePackageAtomic(
        const std::filesystem::path& path,
        const ScenePackage& package,
        platform::EFileWriteMode mode,
        std::size_t max_bytes,
        std::stop_token stop = {}
    ) noexcept;
} // namespace lux::scene
