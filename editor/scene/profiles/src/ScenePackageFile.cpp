#include <lux/engine/editor/ScenePackageFile.hpp>
#include <lux/engine/editor/detail/ProjectFiles.hpp>

namespace lux::editor
{
    SceneFileResult<scene::ScenePackage> readScenePackageFile(
        const std::filesystem::path& path,
        std::size_t max_bytes,
        std::stop_token stop
    ) noexcept
    {
        auto bytes = detail::readProjectBytes(path, max_bytes, stop);
        if (!bytes)
        {
            return cxx::unexpected(VSceneFileFailure{bytes.error()});
        }
        auto owner = std::make_shared<const std::vector<std::byte>>(std::move(*bytes));
        auto package = scene::decodeScenePackage(cxx::SharedBytes<>::fromOwner(owner, *owner), stop);
        if (!package)
        {
            return cxx::unexpected(VSceneFileFailure{std::move(package.error())});
        }
        return std::move(*package);
    }
    SceneFileResult<void> writeScenePackageAtomic(
        const std::filesystem::path& path,
        const scene::ScenePackage& package,
        EProjectWrite mode,
        std::size_t max_bytes,
        std::stop_token stop
    ) noexcept
    {
        auto bytes = scene::encodeScenePackage(package, max_bytes, stop);
        if (!bytes)
        {
            return cxx::unexpected(VSceneFileFailure{std::move(bytes.error())});
        }
        auto written = detail::writeProjectBytesAtomic(path, *bytes, mode, stop);
        if (!written)
        {
            return cxx::unexpected(VSceneFileFailure{written.error()});
        }
        return {};
    }
} // namespace lux::editor
