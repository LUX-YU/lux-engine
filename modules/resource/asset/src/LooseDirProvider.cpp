#include <lux/engine/asset/LooseDirProvider.hpp>
#include <lux/engine/asset/AssetHeaderProbe.hpp>
#include <lux/engine/asset/VirtualPath.hpp>

#include <algorithm>
#include <lux/engine/platform/FormatCompat.h>
#include <fstream>
#include <system_error>
#include <unordered_map>

namespace lux::asset
{
    LooseDirProvider::LooseDirProvider(std::filesystem::path root_dir)
        : root_dir_(std::move(root_dir))
    {
    }

    std::size_t LooseDirProvider::rescan()
    {
        entries_.clear();
        by_path_.clear();
        by_id_.clear();
        diagnostics_.clear();

        std::error_code ec;
        if (root_dir_.empty() || !std::filesystem::exists(root_dir_, ec) || ec)
            return 0;

        // Collect candidates first so collision winners are deterministic
        // (byte-order over the relative file path), independent of the
        // directory-iteration order the host filesystem happens to return.
        struct Candidate
        {
            asset_id_t            id;
            EAssetType            type;
            std::string           vpath;    // mount-relative canonical
            std::string           rel_file; // tie-break key, with extension
            std::filesystem::path file;
        };
        std::vector<Candidate> candidates;

        for (const auto& de :
             std::filesystem::recursive_directory_iterator(root_dir_, ec))
        {
            if (!de.is_regular_file(ec))
                continue;
            const auto ext = de.path().extension().string();
            if (ext != ".luxasset" && ext != ".luxmodel")
                continue;

            std::error_code rel_ec;
            const auto rel =
                std::filesystem::relative(de.path(), root_dir_, rel_ec);
            if (rel_ec)
                continue;

            // vpath = relative path minus the storage extension.
            auto vrel = rel;
            vrel.replace_extension();
            const std::string vpath = vrel.generic_string();
            if (auto err = VirtualPath::validateRelative(vpath))
            {
                diagnostics_.push_back(lux::format(
                    "skip '{}': not addressable as a virtual path (err={})",
                    rel.generic_string(), static_cast<int>(*err)));
                continue;
            }

            const auto probe = readAssetHeader(de.path());
            const auto type  = assetTypeOfMagic(probe.magic);
            if (type == EAssetType::UNKNOWN || probe.id.is_nil())
            {
                diagnostics_.push_back(lux::format(
                    "skip '{}': unrecognized or corrupt asset header",
                    rel.generic_string()));
                continue;
            }

            candidates.push_back(Candidate{
                probe.id, type, vpath, rel.generic_string(), de.path() });
        }

        std::sort(
            candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b)
            {
                return a.rel_file < b.rel_file;
            }
        );

        // Case-insensitive clash detection (diagnostic only — resolution
        // stays byte-sensitive; cook is where the clash becomes a hard
        // error). Maps folded vpath -> first byte-exact vpath seen.
        std::unordered_map<std::string, std::string> folded_seen;

        for (auto& c : candidates)
        {
            if (const auto it = by_path_.find(c.vpath); it != by_path_.end())
            {
                diagnostics_.push_back(lux::format(
                    "vpath collision: '{}' also stored as '{}' — keeping '{}'",
                    c.rel_file, entries_[it->second].file.filename().string(),
                    entries_[it->second].file.filename().string()));
                continue;
            }
            if (const auto it = by_id_.find(c.id); it != by_id_.end())
            {
                diagnostics_.push_back(lux::format(
                    "uuid collision: '{}' duplicates the id of '{}' — keeping '{}'",
                    c.rel_file, entries_[it->second].vpath,
                    entries_[it->second].vpath));
                continue;
            }

            const auto [fit, fresh] =
                folded_seen.try_emplace(foldCaseAscii(c.vpath), c.vpath);
            if (!fresh && fit->second != c.vpath)
            {
                diagnostics_.push_back(lux::format(
                    "case-insensitive clash: '{}' vs '{}' (both resolvable "
                    "now; cook will reject)",
                    c.vpath, fit->second));
            }

            const std::size_t idx = entries_.size();
            entries_.push_back(Entry{ c.id, c.type, c.vpath, c.file });
            by_path_.emplace(entries_[idx].vpath, idx);
            by_id_.emplace(entries_[idx].id, idx);
        }

        return entries_.size();
    }

    std::optional<std::filesystem::path>
    LooseDirProvider::filePathOf(const asset_id_t& id) const
    {
        const auto it = by_id_.find(id);
        if (it == by_id_.end())
            return std::nullopt;
        return entries_[it->second].file;
    }

    std::optional<asset_id_t>
    LooseDirProvider::resolve(std::string_view rel_vpath) const
    {
        // Transparent-lookup-free map: one short-lived key string.
        const auto it = by_path_.find(std::string{ rel_vpath });
        if (it == by_path_.end())
            return std::nullopt;
        return entries_[it->second].id;
    }

    bool LooseDirProvider::contains(const asset_id_t& id) const
    {
        return by_id_.contains(id);
    }

    lux::cxx::expected<AssetBlob, EAssetError>
    LooseDirProvider::open(const asset_id_t& id) const
    {
        const auto it = by_id_.find(id);
        if (it == by_id_.end())
            return lux::cxx::unexpected(EAssetError::ASSET_NOT_EXIST);

        const auto& file = entries_[it->second].file;
        std::ifstream ifs(file, std::ios::binary);
        if (!ifs)
            return lux::cxx::unexpected(EAssetError::FILE_OPEN_FAIL);

        ifs.seekg(0, std::ios::end);
        const std::streamoff end = ifs.tellg();
        if (end <= 0)
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
        ifs.seekg(0, std::ios::beg);

        const auto size = static_cast<std::size_t>(end);
        // std::make_shared<T[]> needs GCC 12+; shared_ptr's array ctor works on GCC 11.
        auto bytes = std::shared_ptr<std::byte[]>(new std::byte[size]());
        if (!ifs.read(reinterpret_cast<char*>(bytes.get()),
                      static_cast<std::streamsize>(size)))
        {
            return lux::cxx::unexpected(EAssetError::READ_FILE_FAIL);
        }
        return AssetBlob{ std::move(bytes), size };
    }

    void LooseDirProvider::enumerate(
        const std::function<void(const ProviderEntry&)>& fn) const
    {
        for (const Entry& e : entries_)
            fn(ProviderEntry{ e.id, e.type, e.vpath, /*tombstone=*/false });
    }

    std::optional<std::string>
    LooseDirProvider::pathOf(const asset_id_t& id) const
    {
        const auto it = by_id_.find(id);
        if (it == by_id_.end())
            return std::nullopt;
        return entries_[it->second].vpath;
    }
}
