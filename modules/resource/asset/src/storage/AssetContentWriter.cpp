#include <lux/engine/resource/asset/storage/AssetContentWriterImpl.hpp>
#include <lux/engine/resource/asset/storage/AssetReadSourceImpl.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace lux::asset
{
    AssetContentWriter::AssetContentWriter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    AssetContentWriter::~AssetContentWriter() noexcept
    {
        if (impl_)
        {
            discardUnpublishedMember();
        }
    }

    AssetContentWriter::AssetContentWriter(AssetContentWriter&& other) noexcept = default;

    AssetContentWriter& AssetContentWriter::operator=(AssetContentWriter&& other) noexcept
    {
        if (this != &other)
        {
            AssetContentWriter previous(std::move(*this));
            impl_ = std::move(other.impl_);
        }
        return *this;
    }

    void AssetContentWriter::attachProvider(std::shared_ptr<IAssetProvider> provider) noexcept
    {
        impl_->provider = std::move(provider);
    }

    cxx::expected<void, EAssetStorageError>
    AssetContentWriter::validateBase(const AssetReadSource& base, const std::filesystem::path& root) noexcept
    {
        if (base.impl_->root_path != root)
        {
            return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
        }
        auto current = AssetReadSource::openRoot(root);
        if (!current)
        {
            return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
        }
        const auto same = current->impl_->root.sameFile(base.impl_->root);
        if (!same || !*same || current->impl_->root_size != base.impl_->root_size)
        {
            return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
        }
        auto bytes = current->readRoot(static_cast<std::size_t>(base.impl_->root_size));
        if (!bytes || current->rootDigest() != base.rootDigest())
        {
            return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
        }
        return {};
    }

    cxx::expected<void, EAssetStorageError> AssetContentWriter::createMember() noexcept
    {
        if (impl_->mode == EAssetContentWrite::APPEND)
        {
            impl_->content_path = impl_->base->impl_->content_path;
            impl_->content_name = impl_->content_path.filename().string();
            auto content = platform::FileHandle::openAppend(impl_->content_path);
            if (!content)
            {
                return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
            }
            const auto same = content->sameFile(impl_->base->impl_->content);
            if (!same || !*same)
            {
                return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
            }
            impl_->content = std::move(*content);
            return {};
        }
        // UUID generation is a foreign, fallible factory operation; no exception crosses this boundary.
        try
        {
            for (;;)
            {
                const auto suffix = uuids::uuid_system_generator{}();
                const auto name = uuids::to_string(impl_->id.uuid()) + "." + uuids::to_string(suffix) + ".content";
                const auto path = impl_->root_path.parent_path() / name;
                auto content = platform::FileHandle::createExclusive(path);
                if (content)
                {
                    impl_->content_name = name;
                    impl_->content_path = path;
                    impl_->content = std::move(*content);
                    return {};
                }
                if (content.error().code != platform::EFileIoError::DESTINATION_EXISTS)
                {
                    return cxx::unexpected(EAssetStorageError::IO_FAILURE);
                }
            }
        }
        catch (const std::system_error&)
        {
            return cxx::unexpected(EAssetStorageError::IO_FAILURE);
        }
    }

    std::string_view AssetContentWriter::contentName() const noexcept
    {
        return impl_->content_name;
    }

    EAssetPublication AssetContentWriter::publication() const noexcept
    {
        return impl_->state;
    }

    cxx::expected<std::uint64_t, EAssetStorageError>
    AssetContentWriter::append(std::span<const std::byte> bytes) noexcept
    {
        if (impl_->state != EAssetPublication::UNPUBLISHED)
        {
            return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
        }
        auto appended = impl_->content.append(bytes);
        if (!appended)
        {
            const auto failure = appended.error().code == platform::EFileIoError::LIMIT
                ? EAssetStorageError::LIMIT_EXCEEDED : EAssetStorageError::IO_FAILURE;
            return cxx::unexpected(failure);
        }
        return *appended;
    }

    cxx::expected<void, EAssetStorageError> AssetContentWriter::publish(std::span<const std::byte> root) noexcept
    {
        if (impl_->state != EAssetPublication::UNPUBLISHED)
        {
            return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
        }
        if (!impl_->content.flush())
        {
            return cxx::unexpected(EAssetStorageError::IO_FAILURE);
        }
        auto published = impl_->provider->publishContentRoot(impl_->id, impl_->base, impl_->root_path, root);
        if (!published)
        {
            if (published.error() == EAssetStorageError::PUBLICATION_UNKNOWN)
            {
                impl_->state = EAssetPublication::UNKNOWN;
            }
            return published;
        }
        impl_->state = EAssetPublication::PUBLISHED;
        if (impl_->mode == EAssetContentWrite::COMPACT)
        {
            retireOldMember(root);
        }
        return {};
    }

    void AssetContentWriter::discardUnpublishedMember() noexcept
    {
        const bool discard = impl_->state == EAssetPublication::UNPUBLISHED &&
            impl_->mode != EAssetContentWrite::APPEND && !impl_->content_path.empty();
        impl_->content = platform::FileHandle{};
        if (discard)
        {
            std::error_code error;
            std::filesystem::remove(impl_->content_path, error);
            // A failed removal remains an unreachable, AssetId-prefixed candidate for explicit compaction.
        }
    }

    void AssetContentWriter::retireOldMember(std::span<const std::byte> published_root) noexcept
    {
        // Only explicit compaction reclaims the writer-owned filename namespace. Root bytes are generic:
        // this layer never parses Scene headers. Existing sources keep their opened physical handles.
        auto current = AssetReadSource::openRoot(impl_->root_path);
        if (!current) return;
        auto root = current->readRoot(published_root.size());
        if (!root || !std::ranges::equal(root->bytes.view(), published_root)) return;
        const auto prefix = std::filesystem::path(uuids::to_string(impl_->id.uuid()) + ".").native();
        const auto extension = std::filesystem::path(".content").native();
        std::error_code error;
        std::filesystem::directory_iterator entries(impl_->root_path.parent_path(), error), end;
        if (error) return;
        for (; entries != end; entries.increment(error))
        {
            if (error) return;
            const auto& entry = *entries;
            const auto name = entry.path().filename().native();
            const bool owned_name = name.starts_with(prefix) && name.ends_with(extension) &&
                name.size() == prefix.size() + 36 + 8;
            if (!owned_name || entry.path() == impl_->content_path) continue;
            std::array<char, 36> suffix{};
            bool ascii = true;
            for (std::size_t i = 0; i < suffix.size(); ++i)
            {
                const auto character = static_cast<std::uint32_t>(name[prefix.size() + i]);
                if (character > 0x7f) { ascii = false; break; }
                suffix[i] = static_cast<char>(character);
            }
            if (!ascii || !uuids::uuid::from_string(std::string_view(suffix.data(), suffix.size()))) continue;
            const auto type = entry.symlink_status(error).type();
            if (error) return;
            if (type != std::filesystem::file_type::regular) continue;
            // A changed root stops cleanup. Same-asset writes must use the Repository's serial admission.
            auto visible = platform::FileHandle::openRead(impl_->root_path);
            if (!visible) return;
            auto unchanged = visible->sameFile(current->impl_->root);
            if (!unchanged || !*unchanged) return;
            std::filesystem::remove(entry.path(), error);
            // Deletion failure leaves an unreachable owned member for the next explicit compaction.
            // A published root is never reclassified as a failed transaction because cleanup was deferred.
            error.clear();
        }
    }

    cxx::expected<AssetContentWriter, EAssetStorageError>
    IAssetProvider::beginContentWrite(AssetId, const AssetReadSource*, EAssetContentWrite, std::string_view) noexcept
    {
        return cxx::unexpected(EAssetStorageError::UNSUPPORTED);
    }

    cxx::expected<void, EAssetStorageError>
    IAssetProvider::publishContentRoot(
        AssetId, const AssetReadSource*, const std::filesystem::path&, std::span<const std::byte>
    ) noexcept
    {
        return cxx::unexpected(EAssetStorageError::UNSUPPORTED);
    }
} // namespace lux::asset
