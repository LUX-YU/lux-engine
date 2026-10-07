#pragma once
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/scene/ScenePackage.hpp>

namespace lux::editor
{
    using VSceneFileFailure = std::variant<ProjectFailure, scene::ScenePackageFailure>;
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
        EProjectWrite,
        std::size_t max_bytes,
        std::stop_token = {}
    ) noexcept;
} // namespace lux::editor
