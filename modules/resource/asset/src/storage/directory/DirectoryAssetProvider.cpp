#include <lux/engine/resource/asset/storage/directory/DirectoryAssetProvider.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/platform/FileIo.hpp>
#include <lux/engine/resource/asset/storage/AssetReadSourceImpl.hpp>
#include <lux/engine/resource/asset/storage/AssetContentWriterImpl.hpp>
#include <lux/engine/resource/asset/storage/VirtualPath.hpp>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <map>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace lux::asset
{
    namespace
    {
        cxx::expected<ProviderEntry, EAssetStorageError>
        readEntry(const std::filesystem::path& path, std::string relative) noexcept
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error)
            {
                return cxx::unexpected(EAssetStorageError::IO_FAILURE);
            }
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                return cxx::unexpected(EAssetStorageError::IO_FAILURE);
            }
            std::array<std::byte, 400> prefix;
            input.read(reinterpret_cast<char*>(prefix.data()), prefix.size());
            const auto count = static_cast<std::size_t>(input.gcount());
            CookedAssetMetadata metadata;
            std::uint32_t magic;
            if (!inspectCookedAssetHeader(std::span(prefix).first(count), size, metadata, magic))
            {
                return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
            }
            if (relative.ends_with(".luxasset"))
            {
                relative.resize(relative.size() - std::string_view(".luxasset").size());
            }
            if (VirtualPath::validateRelative(relative))
            {
                return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
            }
            return ProviderEntry{metadata.id, magic, std::move(relative), false};
        }
    }

    struct DirectoryAssetProvider::Impl final
    {
        std::filesystem::path root;
        mutable std::shared_mutex mutex;
        std::mutex publication;
        std::unordered_map<AssetId, ProviderEntry> entries;
        std::map<std::string, AssetId, std::less<>> paths;
    };

    DirectoryAssetProvider::DirectoryAssetProvider(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    DirectoryAssetProvider::~DirectoryAssetProvider() = default;

    DirectoryAssetProvider::CreateResult DirectoryAssetProvider::create(const std::filesystem::path& root) noexcept
    {
        std::error_code error;
        auto absolute = std::filesystem::weakly_canonical(root, error);
        if (error || !std::filesystem::is_directory(absolute, error))
        {
            return cxx::unexpected(EAssetStorageError::NOT_FOUND);
        }
        auto impl = std::make_unique<Impl>();
        impl->root = std::move(absolute);
        std::filesystem::recursive_directory_iterator current(impl->root, error), end;
        while (!error && current != end)
        {
            if (current->is_regular_file(error) && current->path().extension() == ".luxasset")
            {
                auto entry = readEntry(
                    current->path(), current->path().lexically_relative(impl->root).generic_string()
                );
                if (!entry)
                {
                    return cxx::unexpected(entry.error());
                }
                if (!impl->entries.emplace(entry->id, *entry).second)
                {
                    return cxx::unexpected(EAssetStorageError::ID_CONFLICT);
                }
                impl->paths.emplace(entry->vpath, entry->id);
            }
            current.increment(error);
        }
        if (error)
        {
            return cxx::unexpected(EAssetStorageError::IO_FAILURE);
        }
        return std::shared_ptr<DirectoryAssetProvider>{new DirectoryAssetProvider(std::move(impl))};
    }

    std::optional<AssetId> DirectoryAssetProvider::resolve(std::string_view path) const
    {
        std::shared_lock lock{impl_->mutex};
        const auto found = impl_->paths.find(path);
        return found == impl_->paths.end() ? std::nullopt : std::optional{found->second};
    }

    bool DirectoryAssetProvider::contains(const AssetId& id) const
    {
        std::shared_lock lock{impl_->mutex};
        return impl_->entries.contains(id);
    }

    cxx::expected<ProviderEntry, EAssetStorageError> DirectoryAssetProvider::describe(AssetId id) const
    {
        std::shared_lock lock{impl_->mutex};
        const auto found = impl_->entries.find(id);
        if (found == impl_->entries.end())
        {
            return cxx::unexpected(EAssetStorageError::NOT_FOUND);
        }
        return found->second;
    }

    cxx::expected<AssetBlob, EAssetStorageError>
    DirectoryAssetProvider::open(const AssetId& id, std::size_t max_bytes) const
    {
        auto entry = describe(id);
        if (!entry)
        {
            return cxx::unexpected(entry.error());
        }
        auto bytes = platform::readFileBounded(
            impl_->root / std::filesystem::u8path(entry->vpath + ".luxasset"), max_bytes
        );
        if (!bytes)
        {
            const auto failure = bytes.error().code == platform::EFileIoError::LIMIT
                ? EAssetStorageError::LIMIT_EXCEEDED : EAssetStorageError::IO_FAILURE;
            return cxx::unexpected(failure);
        }
        CookedAssetMetadata metadata;
        std::uint32_t magic;
        const auto valid = inspectCookedAssetHeader(*bytes, bytes->size(), metadata, magic);
        if (!valid || metadata.id != id || magic != entry->magic_number)
        {
            return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
        }
        auto backing = std::make_shared<const std::vector<std::byte>>(std::move(*bytes));
        return AssetBlob::fromShared(cxx::SharedBytes<>::fromOwner(backing, std::span<const std::byte>(*backing)));
    }

    cxx::expected<AssetReadSource, EAssetStorageError> DirectoryAssetProvider::openSource(AssetId id) noexcept
    {
        auto entry = describe(id);
        if (!entry)
        {
            return cxx::unexpected(entry.error());
        }
        return AssetReadSource::openRoot(impl_->root / std::filesystem::u8path(entry->vpath + ".luxasset"));
    }

    void DirectoryAssetProvider::enumerate(const std::function<void(const ProviderEntry&)>& fn) const
    {
        std::vector<ProviderEntry> entries;
        {
            std::shared_lock lock{impl_->mutex};
            entries.reserve(impl_->entries.size());
            for (const auto& [id, entry] : impl_->entries)
            {
                entries.push_back(entry);
            }
        }
        for (const auto& entry : entries)
        {
            fn(entry);
        }
    }

    std::optional<std::string> DirectoryAssetProvider::pathOf(const AssetId& id) const
    {
        auto entry = describe(id);
        return entry ? std::optional{std::move(entry->vpath)} : std::nullopt;
    }

    cxx::expected<void, EAssetStorageError>
    DirectoryAssetProvider::replace(AssetId id, std::span<const std::byte> bytes)
    {
        // Writers serialize publication, but metadata queries never wait on disk IO under the index lock.
        std::lock_guard publication{impl_->publication};
        auto entry = describe(id);
        if (!entry)
        {
            return cxx::unexpected(EAssetStorageError::NOT_FOUND);
        }
        CookedAssetMetadata metadata;
        std::uint32_t magic;
        const auto valid = inspectCookedAssetHeader(bytes, bytes.size(), metadata, magic);
        const bool mismatched = valid && (metadata.id != id || magic != entry->magic_number);
        if (!valid || mismatched)
        {
            return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
        }
        auto written = platform::writeFileAtomic(
            impl_->root / std::filesystem::u8path(entry->vpath + ".luxasset"), bytes, platform::EFileWriteMode::REPLACE
        );
        if (!written)
        {
            const auto failure = written.error().code == platform::EFileIoError::PUBLICATION_UNKNOWN
                ? EAssetStorageError::PUBLICATION_UNKNOWN : EAssetStorageError::IO_FAILURE;
            return cxx::unexpected(failure);
        }
        return {};
    }

    cxx::expected<AssetContentWriter, EAssetStorageError>
    DirectoryAssetProvider::beginContentWrite(
        AssetId id, const AssetReadSource* base, EAssetContentWrite mode, std::string_view new_path
    ) noexcept
    {
        if (id.isNull())
        {
            return cxx::unexpected(EAssetStorageError::NOT_FOUND);
        }
        std::filesystem::path destination;
        if (mode == EAssetContentWrite::CREATE)
        {
            const bool invalid = base != nullptr || VirtualPath::validateRelative(new_path).has_value();
            if (invalid)
            {
                return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
            }
            destination = impl_->root / std::filesystem::u8path(std::string(new_path) + ".luxasset");
            std::error_code error;
            const auto parent = std::filesystem::canonical(destination.parent_path(), error);
            if (error)
            {
                return cxx::unexpected(EAssetStorageError::IO_FAILURE);
            }
            const auto relative = parent.lexically_relative(impl_->root);
            const bool outside = relative.empty() || relative.is_absolute() || *relative.begin() == "..";
            if (outside)
            {
                return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
            }
            destination = parent / destination.filename();
            const bool exists = std::filesystem::exists(destination, error);
            if (error)
            {
                return cxx::unexpected(EAssetStorageError::IO_FAILURE);
            }
            if (contains(id) || exists)
            {
                return cxx::unexpected(EAssetStorageError::ID_CONFLICT);
            }
        }
        else
        {
            const bool invalid = !base || !new_path.empty() ||
                (mode != EAssetContentWrite::APPEND && mode != EAssetContentWrite::COMPACT);
            if (invalid)
            {
                return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
            }
            auto entry = describe(id);
            if (!entry)
            {
                return cxx::unexpected(entry.error());
            }
            destination = impl_->root / std::filesystem::u8path(entry->vpath + ".luxasset");
            if (base->impl_->content_path.empty())
            {
                return cxx::unexpected(EAssetStorageError::UNSUPPORTED);
            }
            auto valid = AssetContentWriter::validateBase(*base, destination);
            if (!valid)
            {
                return cxx::unexpected(valid.error());
            }
        }
        auto impl = std::make_unique<AssetContentWriter::Impl>();
        impl->id = id;
        impl->base = base;
        impl->mode = mode;
        impl->root_path = std::move(destination);
        AssetContentWriter writer(std::move(impl));
        auto created = writer.createMember();
        if (!created)
        {
            return cxx::unexpected(created.error());
        }
        return writer;
    }

    cxx::expected<void, EAssetStorageError>
    DirectoryAssetProvider::publishContentRoot(
        AssetId id, const AssetReadSource* base, const std::filesystem::path& destination,
        std::span<const std::byte> root
    ) noexcept
    {
        // Validation and publication share the existing writer lock; no lock crosses suspension.
        std::lock_guard publication{impl_->publication};
        CookedAssetMetadata metadata;
        std::uint32_t magic{};
        const auto valid = inspectCookedAssetHeader(root, root.size(), metadata, magic);
        if (!valid || metadata.id != id)
        {
            return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
        }
        auto relative = destination.lexically_relative(impl_->root);
        const bool outside = relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
            relative.extension() != ".luxasset";
        if (outside)
        {
            return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
        }
        const auto key = relative.replace_extension().generic_string();
        if (VirtualPath::validateRelative(key))
        {
            return cxx::unexpected(EAssetStorageError::CORRUPT_IMAGE);
        }
        if (base)
        {
            const auto entry = describe(id);
            const bool mismatch = !entry || entry->vpath != key || entry->magic_number != magic;
            if (mismatch)
            {
                return cxx::unexpected(EAssetStorageError::CONTENT_CHANGED);
            }
            auto unchanged = AssetContentWriter::validateBase(*base, destination);
            if (!unchanged)
            {
                return unchanged;
            }
        }
        else if (contains(id) || resolve(key))
        {
            return cxx::unexpected(EAssetStorageError::ID_CONFLICT);
        }
        auto published = platform::writeFileAtomic(
            destination, root, base ? platform::EFileWriteMode::REPLACE : platform::EFileWriteMode::CREATE
        );
        if (!published)
        {
            const auto failure = published.error().code;
            if (failure != platform::EFileIoError::PUBLICATION_UNKNOWN)
            {
                return cxx::unexpected(failure == platform::EFileIoError::DESTINATION_EXISTS
                    ? EAssetStorageError::ID_CONFLICT : EAssetStorageError::IO_FAILURE);
            }
            // Publication may be visible despite a durability failure. Reconcile the index only when
            // the actual root matches; keep UNKNOWN for the caller even when visibility is established.
            const auto actual = platform::readFileBounded(destination, root.size());
            if (!actual || cxx::algorithm::Sha256::hash(std::span(*actual)) != cxx::algorithm::Sha256::hash(root))
            {
                return cxx::unexpected(EAssetStorageError::PUBLICATION_UNKNOWN);
            }
        }
        {
            std::unique_lock lock{impl_->mutex};
            impl_->entries.insert_or_assign(id, ProviderEntry{id, magic, key, false});
            impl_->paths.insert_or_assign(key, id);
        }
        if (!published)
        {
            return cxx::unexpected(EAssetStorageError::PUBLICATION_UNKNOWN);
        }
        return {};
    }

    cxx::expected<std::vector<AssetId>, EAssetStorageError>
    DirectoryAssetProvider::refresh(std::span<const std::filesystem::path> paths)
    {
        std::lock_guard publication{impl_->publication};
        // Expand only the affected directory subtrees. The ordered path index handles removed
        // directories without scanning every asset, while file notifications remain direct lookups.
        std::set<std::string> affected;
        for (const auto& path : paths)
        {
            auto relative = path.lexically_normal().lexically_relative(impl_->root);
            const bool outside = relative.empty() || relative.is_absolute() || *relative.begin() == "..";
            if (outside)
            {
                continue;
            }
            if (relative.extension() == ".luxasset")
            {
                affected.insert(relative.replace_extension().generic_string());
                continue;
            }
            const auto prefix = relative == "." ? std::string{} : relative.generic_string() + "/";
            {
                std::shared_lock lock{impl_->mutex};
                for (auto entry = impl_->paths.lower_bound(prefix);
                     entry != impl_->paths.end() && entry->first.starts_with(prefix); ++entry)
                {
                    affected.insert(entry->first);
                }
            }
            std::error_code error;
            if (std::filesystem::is_directory(impl_->root / relative, error))
            {
                std::filesystem::recursive_directory_iterator current(impl_->root / relative, error), end;
                while (!error && current != end)
                {
                    if (current->is_regular_file(error) && current->path().extension() == ".luxasset")
                    {
                        affected.insert(
                            current->path().lexically_relative(impl_->root).replace_extension().generic_string()
                        );
                    }
                    current.increment(error);
                }
                if (error)
                {
                    return cxx::unexpected(EAssetStorageError::IO_FAILURE);
                }
            }
        }
        std::unordered_map<std::string, std::optional<ProviderEntry>> changes;
        for (const auto& key : affected)
        {
            const auto relative = std::filesystem::u8path(key + ".luxasset");
            std::error_code error;
            const bool exists = std::filesystem::exists(impl_->root / relative, error);
            if (error)
            {
                return cxx::unexpected(EAssetStorageError::IO_FAILURE);
            }
            if (!exists)
            {
                changes.emplace(key, std::nullopt);
                continue;
            }
            auto entry = readEntry(impl_->root / relative, key);
            if (!entry)
            {
                return cxx::unexpected(entry.error());
            }
            changes.emplace(key, std::move(*entry));
        }
        std::unique_lock lock{impl_->mutex};
        std::unordered_set<AssetId> new_ids;
        for (const auto& [path, value] : changes)
        {
            if (!value)
            {
                continue;
            }
            const auto existing = impl_->entries.find(value->id);
            const bool occupied = existing != impl_->entries.end() && !changes.contains(existing->second.vpath);
            if (occupied || !new_ids.insert(value->id).second)
            {
                return cxx::unexpected(EAssetStorageError::ID_CONFLICT);
            }
        }
        std::vector<AssetId> changed;
        changed.reserve(changes.size() * 2);
        for (const auto& [path, value] : changes)
        {
            const auto old = impl_->paths.find(path);
            if (old != impl_->paths.end())
            {
                changed.push_back(old->second);
                impl_->entries.erase(old->second);
                impl_->paths.erase(old);
            }
        }
        for (auto& [path, value] : changes)
        {
            if (value)
            {
                changed.push_back(value->id);
                impl_->paths.emplace(path, value->id);
                impl_->entries.emplace(value->id, std::move(*value));
            }
        }
        return changed;
    }
}
