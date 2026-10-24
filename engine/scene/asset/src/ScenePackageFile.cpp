#include <lux/engine/scene/ScenePackageFile.hpp>

namespace lux::scene
{
    SceneFileResult<ScenePackage> readScenePackageFile(
        const std::filesystem::path& path,
        std::size_t max_bytes,
        std::stop_token stop
    ) noexcept
    {
        auto bytes = platform::readFileBounded(path, max_bytes, stop);
        if (!bytes)
        {
            return cxx::unexpected(VSceneFileFailure{bytes.error()});
        }
        auto backing = std::make_shared<const std::vector<std::byte>>(std::move(*bytes));
        auto package = decodeScenePackage(cxx::SharedBytes<>::fromOwner(backing, *backing), stop);
        if (!package)
        {
            return cxx::unexpected(VSceneFileFailure{std::move(package.error())});
        }
        return std::move(*package);
    }

    SceneFileResult<void> writeScenePackageAtomic(
        const std::filesystem::path& path,
        const ScenePackage& package,
        platform::EFileWriteMode mode,
        std::size_t max_bytes,
        std::stop_token stop
    ) noexcept
    {
        auto bytes = encodeScenePackage(package, max_bytes, stop);
        if (!bytes)
        {
            return cxx::unexpected(VSceneFileFailure{std::move(bytes.error())});
        }
        auto written = platform::writeFileAtomic(path, *bytes, mode, stop);
        if (!written)
        {
            return cxx::unexpected(VSceneFileFailure{written.error()});
        }
        return {};
    }
} // namespace lux::scene
