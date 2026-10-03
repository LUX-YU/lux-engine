#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/platform/FilePath.hpp>

#include <algorithm>
#include <fstream>

namespace lux::editor
{
    namespace
    {
        // A fixed source location and version. A delayed first read cannot silently adopt a newer file.
        class SourceProvider final : public asset::IAssetProvider
        {
        public:
            SourceProvider(
                std::filesystem::path root,
                ProjectAssetEntry entry,
                std::size_t limit,
                std::string expected_digest
            )
                : root_(std::move(root)), entry_(std::move(entry)), limit_(limit), digest_(std::move(expected_digest))
            {}

            std::optional<asset::AssetId> resolve(std::string_view path) const override
            {
                return path == entry_.source_path ? std::optional{entry_.id} : std::nullopt;
            }
            bool contains(const asset::AssetId& id) const override
            {
                return id == entry_.id;
            }
            std::optional<std::string> pathOf(const asset::AssetId& id) const override
            {
                return contains(id) ? std::optional{entry_.source_path} : std::nullopt;
            }
            void enumerate(const std::function<void(const asset::ProviderEntry&)>& visit) const override
            {
                visit({entry_.id, 0, entry_.source_path});
            }
            lux::cxx::expected<asset::AssetBlob, asset::EAssetStorageError>
            open(const asset::AssetId& id, std::size_t max_bytes) const noexcept override
            {
                using Error = asset::EAssetStorageError;
                if (!contains(id))
                    return lux::cxx::unexpected(Error::NOT_FOUND);
                std::error_code error;
                const auto root = std::filesystem::canonical(lux::engine::platform::nativeFilePath(root_), error);
                if (error)
                    return lux::cxx::unexpected(Error::IO_FAILURE);
                const auto path = std::filesystem::canonical(
                    lux::engine::platform::nativeFilePath(root / std::filesystem::u8path(entry_.source_path)),
                    error
                );
                if (error)
                    return lux::cxx::unexpected(Error::IO_FAILURE);
                const auto relative = path.lexically_relative(root);
                const bool escapes = relative.empty() || relative.is_absolute() ||
                                     std::ranges::any_of(relative, [](const auto& part) { return part == ".."; });
                if (escapes)
                    return lux::cxx::unexpected(Error::UNSUPPORTED);
                const auto native_path = lux::engine::platform::nativeFilePath(path);
                const auto size = std::filesystem::file_size(native_path, error);
                if (error)
                    return lux::cxx::unexpected(Error::IO_FAILURE);
                if (size > std::min(limit_, max_bytes))
                    return lux::cxx::unexpected(Error::LIMIT_EXCEEDED);
                auto bytes = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(size));
                std::ifstream file(native_path, std::ios::binary);
                if (!file.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(size)) ||
                    file.peek() != std::char_traits<char>::eof() || file.bad())
                    return lux::cxx::unexpected(Error::IO_FAILURE);
                if (projectContentDigest(*bytes) != digest_)
                    return lux::cxx::unexpected(Error::CONTENT_CHANGED);
                return asset::AssetBlob::fromShared(lux::cxx::SharedBytes<>::fromOwner(bytes, *bytes));
            }

        private:
            std::filesystem::path root_;
            ProjectAssetEntry entry_;
            std::size_t limit_;
            std::string digest_;
        };
    }

    EditorResult<asset::AssetVfsView> ProjectStorage::captureSource(
        asset::AssetId id,
        std::size_t max_bytes,
        std::string expected_digest
    ) const noexcept
    {
        const auto* entry = asset(id);
        if (closing_ || !entry || !max_bytes || expected_digest.empty())
            return lux::cxx::unexpected(
                EditorFailure{closing_ ? EEditorError::CLOSING : EEditorError::INVALID_ARGUMENT, "source.capture"}
            );
        asset::AssetVfs sources;
        const auto mount = sources.mount(
            {"/sources", std::make_shared<SourceProvider>(root_, *entry, max_bytes, std::move(expected_digest)), 0}
        );
        if (mount == asset::kInvalidMountId)
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "source.mount"});
        return sources.view().capture();
    }
}
