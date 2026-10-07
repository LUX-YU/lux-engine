#include <lux/engine/editor/ScenePackageFile.hpp>
#include <lux/engine/editor/detail/FileIo.hpp>

namespace lux::editor
{
    namespace
    {
        SceneFileFailure fileFailure(detail::FileIoFailure value) noexcept
        {
            switch (value.code)
            {
            case detail::EFileIoError::INVALID_PATH:
                return {ESceneFileError::INVALID_PATH, value.system};
            case detail::EFileIoError::LIMIT:
                return {ESceneFileError::LIMIT, value.system};
            case detail::EFileIoError::CANCELLED:
                return {ESceneFileError::CANCELLED, value.system};
            case detail::EFileIoError::IO:
                return {ESceneFileError::IO, value.system};
            case detail::EFileIoError::DESTINATION_EXISTS:
                return {ESceneFileError::DESTINATION_EXISTS, value.system};
            case detail::EFileIoError::PUBLICATION_UNKNOWN:
                return {ESceneFileError::PUBLICATION_UNKNOWN, value.system};
            }
            std::terminate();
        }
    } // namespace

    SceneFileResult<scene::ScenePackage> readScenePackageFile(
        const std::filesystem::path& path,
        std::size_t max_bytes,
        std::stop_token stop
    ) noexcept
    {
        auto bytes = detail::readFileBounded(path, max_bytes, stop);
        if (!bytes)
        {
            return cxx::unexpected(VSceneFileFailure{fileFailure(bytes.error())});
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
        ESceneWrite mode,
        std::size_t max_bytes,
        std::stop_token stop
    ) noexcept
    {
        auto bytes = scene::encodeScenePackage(package, max_bytes, stop);
        if (!bytes)
        {
            return cxx::unexpected(VSceneFileFailure{std::move(bytes.error())});
        }
        auto written = detail::writeFileAtomic(
            path,
            *bytes,
            mode == ESceneWrite::CREATE ? detail::EFileWriteMode::CREATE : detail::EFileWriteMode::REPLACE,
            stop
        );
        if (!written)
        {
            return cxx::unexpected(VSceneFileFailure{fileFailure(written.error())});
        }
        return {};
    }
} // namespace lux::editor
