#include <lux/engine/asset/PakAssetProvider.hpp>
#include <lux/engine/asset/AssetHeaderProbe.hpp>
#include <lux/engine/asset/PakCodec.hpp>

#include <algorithm>
#include <lux/engine/platform/FormatCompat.h>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_map>

namespace lux::asset
{
    struct PakAssetProvider::Data
    {
        std::filesystem::path pak_path;
        detail::PakIndex      index;

        /// vpath -> entry index (non-tombstone rows only).
        std::unordered_map<std::string, std::size_t> by_path;
        /// entry index -> vpath (for enumerate/pathOf).
        std::unordered_map<std::size_t, std::string> path_of;

        /// One shared read stream guarded by a lock — the seam for a future
        /// async loader (multiple readers would each take the lock briefly).
        mutable std::ifstream stream;
        mutable std::mutex    stream_mutex;

        [[nodiscard]] const detail::PakEntry* find(const asset_id_t& id) const
        {
            const auto it = std::lower_bound(
                index.entries.begin(), index.entries.end(), id,
                [](const detail::PakEntry& e, const asset_id_t& key)
                {
                    return e.id < key;
                });
            if (it == index.entries.end() || !(it->id == id))
                return nullptr;
            return &*it;
        }
    };

    PakAssetProvider::PakAssetProvider()
        : d_(std::make_unique<Data>())
    {
    }

    PakAssetProvider::~PakAssetProvider() = default;

    lux::cxx::expected<std::shared_ptr<PakAssetProvider>, std::string>
    PakAssetProvider::loadFromFile(const std::filesystem::path& pak_path)
    {
        std::error_code ec;
        const auto file_size = std::filesystem::file_size(pak_path, ec);
        if (ec)
            return lux::cxx::unexpected(
                lux::format("cannot stat '{}'", pak_path.string()));

        std::ifstream stream(pak_path, std::ios::binary);
        if (!stream)
            return lux::cxx::unexpected(
                lux::format("cannot open '{}'", pak_path.string()));

        detail::PakIndex index;
        std::string err;
        if (!detail::readPakIndex(stream, file_size, index, &err))
            return lux::cxx::unexpected(
                lux::format("'{}': {}", pak_path.string(), err));

        auto provider = std::shared_ptr<PakAssetProvider>(new PakAssetProvider());
        Data& d = *provider->d_;
        d.pak_path = pak_path;
        d.index    = std::move(index);
        d.stream   = std::move(stream);

        for (const auto& row : d.index.paths)
        {
            const auto it = std::lower_bound(
                d.index.entries.begin(), d.index.entries.end(), row.id,
                [](const detail::PakEntry& e, const asset_id_t& key)
                {
                    return e.id < key;
                });
            const auto idx = static_cast<std::size_t>(
                it - d.index.entries.begin());
            if (it->tombstone())
                continue; // tombstones never resolve
            d.by_path.emplace(row.vpath, idx);
            d.path_of.emplace(idx, row.vpath);
        }
        return provider;
    }

    const std::string& PakAssetProvider::mountHint() const noexcept
    {
        return d_->index.mount_hint;
    }

    const std::filesystem::path& PakAssetProvider::pakPath() const noexcept
    {
        return d_->pak_path;
    }

    std::size_t PakAssetProvider::assetCount() const noexcept
    {
        return d_->index.entries.size();
    }

    std::optional<asset_id_t>
    PakAssetProvider::resolve(std::string_view rel_vpath) const
    {
        const auto it = d_->by_path.find(std::string{ rel_vpath });
        if (it == d_->by_path.end())
            return std::nullopt;
        return d_->index.entries[it->second].id;
    }

    bool PakAssetProvider::contains(const asset_id_t& id) const
    {
        return d_->find(id) != nullptr;
    }

    lux::cxx::expected<AssetBlob, EAssetError>
    PakAssetProvider::open(const asset_id_t& id) const
    {
        const auto* entry = d_->find(id);
        if (!entry)
            return lux::cxx::unexpected(EAssetError::ASSET_NOT_EXIST);
        if (entry->tombstone())
            return lux::cxx::unexpected(EAssetError::ASSET_NOT_EXIST);
        if (entry->compression != detail::kPakCompressionNone)
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED);
        if (entry->size == 0)
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        // std::make_shared<T[]> needs GCC 12+; shared_ptr's array ctor works on GCC 11.
        auto bytes = std::shared_ptr<std::byte[]>(
            new std::byte[static_cast<std::size_t>(entry->size)]());
        {
            std::lock_guard lock(d_->stream_mutex);
            d_->stream.clear();
            d_->stream.seekg(static_cast<std::streamoff>(entry->offset),
                             std::ios::beg);
            if (!d_->stream.read(reinterpret_cast<char*>(bytes.get()),
                                 static_cast<std::streamsize>(entry->size)))
            {
                return lux::cxx::unexpected(EAssetError::READ_FILE_FAIL);
            }
        }

        if (entry->content_hash != 0
            && detail::fnv1a64(bytes.get(),
                               static_cast<std::size_t>(entry->size))
                   != entry->content_hash)
        {
            return lux::cxx::unexpected(EAssetError::READ_FILE_FAIL);
        }
        return AssetBlob{ std::move(bytes),
                          static_cast<std::size_t>(entry->size) };
    }

    void PakAssetProvider::enumerate(
        const std::function<void(const ProviderEntry&)>& fn) const
    {
        for (std::size_t i = 0; i < d_->index.entries.size(); ++i)
        {
            const auto& e = d_->index.entries[i];
            if (e.tombstone())
            {
                fn(ProviderEntry{ e.id, EAssetType::UNKNOWN, std::string{},
                                  /*tombstone=*/true });
                continue;
            }
            const auto it = d_->path_of.find(i);
            if (it == d_->path_of.end())
                continue; // openable by id, but not addressable — not listed
            fn(ProviderEntry{ e.id, assetTypeOfMagic(e.asset_magic),
                              it->second, /*tombstone=*/false });
        }
    }

    std::optional<std::string>
    PakAssetProvider::pathOf(const asset_id_t& id) const
    {
        const auto* entry = d_->find(id);
        if (!entry || entry->tombstone())
            return std::nullopt;
        const auto idx = static_cast<std::size_t>(entry - d_->index.entries.data());
        const auto it  = d_->path_of.find(idx);
        if (it == d_->path_of.end())
            return std::nullopt;
        return it->second;
    }
}
