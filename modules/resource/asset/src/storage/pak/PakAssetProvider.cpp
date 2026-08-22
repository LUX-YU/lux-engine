#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/resource/asset/AssetHeaderProbe.hpp>
#include <lux/engine/resource/asset/storage/pak/PakCodec.hpp>

#include <algorithm>
#include <fstream>
#include <list>
#include <limits>
#include <mutex>
#include <unordered_map>

#include <lux/cxx/core/Format.hpp>

namespace lux::asset
{
    struct PakAssetProvider::Data
    {
        static constexpr std::size_t kPageCacheCapacity = 256u;

        struct CachedPage final
        {
            detail::PakPage page;
            lux::cxx::algorithm::Sha256Digest digest;
            std::list<std::uint64_t>::iterator lru;
        };

        std::filesystem::path pak_path;
        std::uint64_t file_size{0u};
        detail::PakHeader header;
        std::string mount_hint;

        mutable std::mutex cache_mutex;
        mutable std::list<std::uint64_t> page_lru;
        mutable std::unordered_map<std::uint64_t, CachedPage> page_cache;
        mutable PakProviderStats stats;

        [[nodiscard]] lux::cxx::expected<detail::PakPage, std::string>
        loadPage(
            std::uint64_t offset,
            const lux::cxx::algorithm::Sha256Digest& expected) const
        {
            {
                std::lock_guard lock(cache_mutex);
                const auto found = page_cache.find(offset);
                if (found != page_cache.end())
                {
                    if (found->second.digest != expected)
                        return lux::cxx::unexpected(
                            std::string{"conflicting Pak page digest"});
                    page_lru.splice(
                        page_lru.begin(), page_lru, found->second.lru);
                    ++stats.index_page_hits;
                    return found->second.page;
                }
                ++stats.index_page_misses;
            }

            detail::PakPage loaded;
            std::ifstream stream(pak_path, std::ios::binary);
            std::string error;
            if (!stream
                || !detail::readPakPage(
                    stream, file_size, offset, loaded, &error))
            {
                return lux::cxx::unexpected(
                    error.empty() ? std::string{"cannot open Pak index"} : error);
            }
            if (!detail::verifyPakPageDigest(loaded, expected))
                return lux::cxx::unexpected(
                    std::string{"Pak index page digest mismatch"});

            std::lock_guard lock(cache_mutex);
            const auto found = page_cache.find(offset);
            if (found != page_cache.end())
            {
                if (found->second.digest != expected)
                    return lux::cxx::unexpected(
                        std::string{"conflicting Pak page digest"});
                page_lru.splice(
                    page_lru.begin(), page_lru, found->second.lru);
                return found->second.page;
            }
            page_lru.push_front(offset);
            page_cache.emplace(
                offset,
                CachedPage{loaded, expected, page_lru.begin()});
            while (page_cache.size() > kPageCacheCapacity)
            {
                const auto evicted = page_lru.back();
                page_lru.pop_back();
                page_cache.erase(evicted);
                ++stats.index_pages_evicted;
            }
            stats.index_pages_resident = page_cache.size();
            stats.metadata_resident_bytes =
                page_cache.size() * detail::kPakPageSize;
            stats.metadata_resident_high_water = std::max(
                stats.metadata_resident_high_water,
                stats.metadata_resident_bytes);
            return loaded;
        }

        [[nodiscard]] lux::cxx::expected<
            std::optional<detail::PakEntry>, std::string>
        findEntry(const asset_id_t& id) const
        {
            auto offset = header.entry_root_offset;
            auto digest = header.entry_root_digest;
            for (std::uint32_t depth = 0u; depth < 64u; ++depth)
            {
                auto page_result = loadPage(offset, digest);
                if (!page_result)
                    return lux::cxx::unexpected(page_result.error());
                const auto& page = page_result.value();
                const auto page_header = detail::pakPageHeader(page);
                std::string error;
                if (page_header.kind == detail::EPakPageKind::ENTRY_LEAF)
                {
                    std::vector<detail::PakEntry> rows;
                    if (!detail::decodeEntryLeaf(page, rows, &error))
                        return lux::cxx::unexpected(std::move(error));
                    if (!std::is_sorted(
                            rows.begin(), rows.end(), [](const auto& a, const auto& b)
                            {
                                return a.id < b.id;
                            }))
                    {
                        return lux::cxx::unexpected(
                            std::string{"unordered Pak entry leaf"});
                    }
                    const auto found = std::lower_bound(
                        rows.begin(), rows.end(), id,
                        [](const auto& row, const auto& key)
                        {
                            return row.id < key;
                        });
                    if (found == rows.end() || found->id != id)
                        return std::optional<detail::PakEntry>{};
                    return std::optional<detail::PakEntry>{*found};
                }
                if (page_header.kind != detail::EPakPageKind::ENTRY_INTERNAL)
                    return lux::cxx::unexpected(
                        std::string{"wrong page kind in Pak entry tree"});
                std::vector<detail::PakEntryChild> children;
                if (!detail::decodeEntryInternal(page, children, &error))
                    return lux::cxx::unexpected(std::move(error));
                if (!std::is_sorted(
                        children.begin(), children.end(), [](const auto& a, const auto& b)
                        {
                            return a.maximum_key < b.maximum_key;
                        }))
                {
                    return lux::cxx::unexpected(
                        std::string{"unordered Pak entry internal page"});
                }
                const auto child = std::lower_bound(
                    children.begin(), children.end(), id,
                    [](const auto& row, const auto& key)
                    {
                        return row.maximum_key < key;
                    });
                if (child == children.end())
                    return std::optional<detail::PakEntry>{};
                offset = child->offset;
                digest = child->digest;
            }
            return lux::cxx::unexpected(
                std::string{"Pak entry tree depth limit exceeded"});
        }

        [[nodiscard]] lux::cxx::expected<
            std::optional<asset_id_t>, std::string>
        findPath(std::string_view path) const
        {
            auto offset = header.path_root_offset;
            auto digest = header.path_root_digest;
            for (std::uint32_t depth = 0u; depth < 64u; ++depth)
            {
                auto page_result = loadPage(offset, digest);
                if (!page_result)
                    return lux::cxx::unexpected(page_result.error());
                const auto& page = page_result.value();
                const auto page_header = detail::pakPageHeader(page);
                std::string error;
                if (page_header.kind == detail::EPakPageKind::PATH_LEAF)
                {
                    std::vector<detail::PakPathRow> rows;
                    if (!detail::decodePathLeaf(page, rows, &error))
                        return lux::cxx::unexpected(std::move(error));
                    if (!std::is_sorted(
                            rows.begin(), rows.end(), [](const auto& a, const auto& b)
                            {
                                return a.vpath < b.vpath;
                            }))
                    {
                        return lux::cxx::unexpected(
                            std::string{"unordered Pak path leaf"});
                    }
                    const auto found = std::lower_bound(
                        rows.begin(), rows.end(), path,
                        [](const auto& row, const auto& key)
                        {
                            return row.vpath < key;
                        });
                    if (found == rows.end() || found->vpath != path)
                        return std::optional<asset_id_t>{};
                    return std::optional<asset_id_t>{found->id};
                }
                if (page_header.kind != detail::EPakPageKind::PATH_INTERNAL)
                    return lux::cxx::unexpected(
                        std::string{"wrong page kind in Pak path tree"});
                std::vector<detail::PakPathChild> children;
                if (!detail::decodePathInternal(page, children, &error))
                    return lux::cxx::unexpected(std::move(error));
                if (!std::is_sorted(
                        children.begin(), children.end(), [](const auto& a, const auto& b)
                        {
                            return a.maximum_key < b.maximum_key;
                        }))
                {
                    return lux::cxx::unexpected(
                        std::string{"unordered Pak path internal page"});
                }
                const auto child = std::lower_bound(
                    children.begin(), children.end(), path,
                    [](const auto& row, const auto& key)
                    {
                        return row.maximum_key < key;
                    });
                if (child == children.end())
                    return std::optional<asset_id_t>{};
                offset = child->offset;
                digest = child->digest;
            }
            return lux::cxx::unexpected(
                std::string{"Pak path tree depth limit exceeded"});
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

        detail::PakHeader header;
        std::string error;
        if (!detail::readPakHeader(stream, file_size, header, &error))
            return lux::cxx::unexpected(
                lux::format("'{}': {}", pak_path.string(), error));

        auto provider = std::shared_ptr<PakAssetProvider>(new PakAssetProvider());
        auto& data = *provider->d_;
        data.pak_path = pak_path;
        data.file_size = file_size;
        data.header = header;
        data.mount_hint.assign(
            header.mount_hint,
            header.mount_hint + header.mount_hint_size);

        // Validate and retain only the two roots. No leaf or complete-path
        // table is touched during startup, so metadata remains O(cache budget).
        auto entry_root = data.loadPage(
            header.entry_root_offset, header.entry_root_digest);
        if (!entry_root)
            return lux::cxx::unexpected(entry_root.error());
        const auto entry_kind = detail::pakPageHeader(entry_root.value()).kind;
        if (entry_kind != detail::EPakPageKind::ENTRY_LEAF
            && entry_kind != detail::EPakPageKind::ENTRY_INTERNAL)
        {
            return lux::cxx::unexpected(
                std::string{"invalid Pak entry root kind"});
        }
        auto path_root = data.loadPage(
            header.path_root_offset, header.path_root_digest);
        if (!path_root)
            return lux::cxx::unexpected(path_root.error());
        const auto path_kind = detail::pakPageHeader(path_root.value()).kind;
        if (path_kind != detail::EPakPageKind::PATH_LEAF
            && path_kind != detail::EPakPageKind::PATH_INTERNAL)
        {
            return lux::cxx::unexpected(
                std::string{"invalid Pak path root kind"});
        }
        return provider;
    }

    const std::string& PakAssetProvider::mountHint() const noexcept
    {
        return d_->mount_hint;
    }

    const std::filesystem::path& PakAssetProvider::pakPath() const noexcept
    {
        return d_->pak_path;
    }

    std::size_t PakAssetProvider::assetCount() const noexcept
    {
        return static_cast<std::size_t>(d_->header.entry_count);
    }

    PakProviderStats PakAssetProvider::stats() const noexcept
    {
        std::lock_guard lock(d_->cache_mutex);
        return d_->stats;
    }

    std::optional<asset_id_t>
    PakAssetProvider::resolve(std::string_view rel_vpath) const
    {
        const auto result = d_->findPath(rel_vpath);
        if (!result)
            return std::nullopt;
        return result.value();
    }

    bool PakAssetProvider::contains(const asset_id_t& id) const
    {
        const auto result = d_->findEntry(id);
        return result && result.value().has_value();
    }

    lux::cxx::expected<AssetBlob, EAssetError>
    PakAssetProvider::open(const asset_id_t& id) const
    {
        const auto found = d_->findEntry(id);
        if (!found)
            return lux::cxx::unexpected(EAssetError::READ_FILE_FAIL);
        if (!found.value() || found.value()->tombstone())
            return lux::cxx::unexpected(EAssetError::ASSET_NOT_EXIST);
        const auto& entry = *found.value();
        if (entry.compression != detail::kPakCompressionNone)
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED);
        if (entry.size == 0u
            || entry.size > std::numeric_limits<std::size_t>::max()
            || entry.offset < 256u
            || entry.offset > d_->header.payload_end
            || entry.size > d_->header.payload_end - entry.offset)
        {
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
        }

        auto bytes = std::shared_ptr<std::byte[]>(
            new std::byte[static_cast<std::size_t>(entry.size)]());
        std::ifstream stream(d_->pak_path, std::ios::binary);
        if (!stream)
            return lux::cxx::unexpected(EAssetError::FILE_OPEN_FAIL);
        stream.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
        if (!stream.read(
                reinterpret_cast<char*>(bytes.get()),
                static_cast<std::streamsize>(entry.size)))
        {
            return lux::cxx::unexpected(EAssetError::READ_FILE_FAIL);
        }
        if (lux::cxx::algorithm::Sha256::hash(std::span<const std::byte>{
                bytes.get(), static_cast<std::size_t>(entry.size)})
            != entry.content_digest)
        {
            return lux::cxx::unexpected(EAssetError::READ_FILE_FAIL);
        }
        return AssetBlob::fromSharedArray(
            std::move(bytes), static_cast<std::size_t>(entry.size));
    }

    void PakAssetProvider::enumerate(
        const std::function<void(const ProviderEntry&)>& fn) const
    {
        std::ifstream stream(d_->pak_path, std::ios::binary);
        std::vector<detail::PakEntry> entries;
        std::string error;
        if (!stream
            || !detail::readAllPakEntries(
                stream, d_->file_size, d_->header, entries, &error))
        {
            return;
        }
        for (const auto& entry : entries)
        {
            if (entry.tombstone())
            {
                fn(ProviderEntry{
                    entry.id,
                    entry.asset_magic,
                    std::string{},
                    true});
            }
            else if (!entry.vpath.empty())
            {
                fn(ProviderEntry{
                    entry.id,
                    entry.asset_magic,
                    entry.vpath,
                    false});
            }
        }
    }

    std::optional<std::string>
    PakAssetProvider::pathOf(const asset_id_t& id) const
    {
        const auto found = d_->findEntry(id);
        if (!found || !found.value() || found.value()->tombstone()
            || found.value()->vpath.empty())
        {
            return std::nullopt;
        }
        return found.value()->vpath;
    }
} // namespace lux::asset
