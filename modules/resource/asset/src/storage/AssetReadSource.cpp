#include <lux/engine/resource/asset/storage/AssetReadSourceImpl.hpp>

#include <limits>
#include <utility>

namespace lux::asset
{
    namespace
    {
        bool validateMemberName(std::string_view name) noexcept
        {
            const bool invalid = name.empty() || name == "." || name == ".." ||
                name.find_first_of("/\\:") != std::string_view::npos || name.find('\0') != std::string_view::npos;
            return !invalid;
        }
    }

    AssetReadSource::AssetReadSource(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    AssetReadSource::~AssetReadSource() noexcept = default;

    AssetReadSource::AssetReadSource(AssetReadSource&& other) noexcept = default;

    AssetReadSource& AssetReadSource::operator=(AssetReadSource&& other) noexcept = default;

    cxx::expected<AssetReadSource, EAssetStorageError>
    AssetReadSource::openRoot(const std::filesystem::path& path) noexcept
    {
        auto root = platform::FileHandle::openRead(path);
        if (!root)
        {
            return cxx::unexpected(EAssetStorageError::IO_FAILURE);
        }
        auto size = root->size();
        if (!size)
        {
            return cxx::unexpected(EAssetStorageError::IO_FAILURE);
        }
        auto impl = std::make_unique<Impl>();
        impl->root = std::move(*root);
        impl->root_path = path;
        impl->root_size = *size;
        return AssetReadSource(std::move(impl));
    }

    void AssetReadSource::attachProvider(std::shared_ptr<IAssetProvider> provider) noexcept
    {
        impl_->provider = std::move(provider);
    }

    cxx::expected<AssetBlob, EAssetStorageError> AssetReadSource::readRoot(std::size_t limit) noexcept
    {
        if (impl_->root_size > limit)
        {
            return cxx::unexpected(EAssetStorageError::LIMIT_EXCEEDED);
        }
        auto bytes = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(impl_->root_size));
        auto read = impl_->root.readAt(0, *bytes);
        if (!read)
        {
            return cxx::unexpected(EAssetStorageError::IO_FAILURE);
        }
        impl_->root_digest = cxx::algorithm::Sha256::hash(std::span<const std::byte>(*bytes));
        return AssetBlob::fromShared(cxx::SharedBytes<>::fromOwner(bytes, std::span<const std::byte>(*bytes)));
    }

    cxx::expected<void, EAssetStorageError> AssetReadSource::bindContent(std::string_view name) noexcept
    {
        if (!validateMemberName(name))
        {
            return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
        }
        std::filesystem::path path;
        try
        {
            path = impl_->root_path.parent_path() / std::filesystem::u8path(name);
        }
        catch (const std::filesystem::filesystem_error&)
        {
            return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
        }
        std::error_code error;
        const auto physical = std::filesystem::weakly_canonical(path, error);
        if (error || physical.parent_path() != impl_->root_path.parent_path())
        {
            return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
        }
        auto content = platform::FileHandle::openRead(path);
        if (!content)
        {
            return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
        }
        auto size = content->size();
        auto current = platform::FileHandle::openRead(impl_->root_path);
        if (!size || !current)
        {
            return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
        }
        const auto same = current->sameFile(impl_->root);
        if (!same || !*same)
        {
            return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
        }
        const auto is_root = content->sameFile(impl_->root);
        if (!is_root || *is_root)
        {
            return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
        }
        impl_->content_path = path;
        impl_->content_size = *size;
        impl_->content = std::move(*content);
        return {};
    }

    cxx::expected<AssetBlob, EAssetStorageError>
    AssetReadSource::readRange(std::uint64_t offset, std::uint64_t size, std::size_t limit) const noexcept
    {
        if (impl_->content_path.empty())
        {
            return cxx::unexpected(EAssetStorageError::UNSUPPORTED);
        }
        const bool invalid_range = offset > impl_->content_size || size > impl_->content_size - offset;
        if (invalid_range)
        {
            return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
        }
        if (size > limit)
        {
            return cxx::unexpected(EAssetStorageError::LIMIT_EXCEEDED);
        }
        auto bytes = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(size));
        auto read = impl_->content.readAt(offset, *bytes);
        if (!read)
        {
            return cxx::unexpected(EAssetStorageError::IO_FAILURE);
        }
        return AssetBlob::fromShared(cxx::SharedBytes<>::fromOwner(bytes, std::span<const std::byte>(*bytes)));
    }

    std::uint64_t AssetReadSource::contentSize() const noexcept
    {
        return impl_->content_size;
    }

    const cxx::algorithm::Sha256Digest& AssetReadSource::rootDigest() const noexcept
    {
        return impl_->root_digest;
    }

    cxx::expected<AssetReadSource, EAssetStorageError> IAssetProvider::openSource(AssetId) noexcept
    {
        return cxx::unexpected(EAssetStorageError::UNSUPPORTED);
    }
} // namespace lux::asset
