#include <lux/engine/asset/PakCook.hpp>
#include <lux/engine/asset/AssetHeaderProbe.hpp>
#include <lux/engine/asset/PakCodec.hpp>
#include <lux/engine/asset/VirtualPath.hpp>

#include <algorithm>
#include <lux/engine/platform/FormatCompat.h>
#include <fstream>
#include <map>
#include <system_error>
#include <unordered_map>

namespace lux::asset
{
    lux::cxx::expected<PakCookResult, std::string>
    cookDirectoryToPak(const std::filesystem::path& content_dir,
                       const std::filesystem::path& out_pak,
                       std::string_view             mount_hint)
    {
        if (!VirtualPath::isLegalRoot(mount_hint))
            return lux::cxx::unexpected(
                lux::format("illegal mount hint '{}'", mount_hint));

        std::error_code ec;
        if (!std::filesystem::is_directory(content_dir, ec) || ec)
            return lux::cxx::unexpected(
                lux::format("'{}' is not a directory", content_dir.string()));

        // Walk with the SAME byte-for-byte rules as the editor scan
        // (extension filter + header probe + extensionless relative vpath),
        // but collect violations instead of picking winners.
        std::vector<detail::PakWriteEntry> entries;
        std::vector<std::string>           violations;

        std::unordered_map<std::string, std::string> by_vpath;  // vpath -> rel file
        std::unordered_map<std::string, std::string> by_folded; // folded -> vpath
        std::unordered_map<asset_id_t,  std::string> by_id;     // id -> rel file

        // Deterministic walk: collect + sort by relative path bytes first.
        std::vector<std::filesystem::path> files;
        for (const auto& de :
             std::filesystem::recursive_directory_iterator(content_dir, ec))
        {
            if (!de.is_regular_file(ec))
                continue;
            const auto ext = de.path().extension().string();
            if (ext != ".luxasset" && ext != ".luxmodel")
                continue;
            files.push_back(de.path());
        }
        std::sort(files.begin(), files.end(),
                  [&](const std::filesystem::path& a,
                      const std::filesystem::path& b)
                  {
                      return a.lexically_relative(content_dir).generic_string()
                           < b.lexically_relative(content_dir).generic_string();
                  });

        for (const auto& file : files)
        {
            const std::string rel =
                file.lexically_relative(content_dir).generic_string();

            auto vrel = file.lexically_relative(content_dir);
            vrel.replace_extension();
            const std::string vpath = vrel.generic_string();

            if (auto verr = VirtualPath::validateRelative(vpath))
            {
                violations.push_back(lux::format(
                    "'{}': not addressable as a virtual path (err={})",
                    rel, static_cast<int>(*verr)));
                continue;
            }

            const auto probe = readAssetHeader(file);
            const auto type  = assetTypeOfMagic(probe.magic);
            if (type == EAssetType::UNKNOWN || probe.id.is_nil())
            {
                violations.push_back(lux::format(
                    "'{}': unrecognized or corrupt asset header", rel));
                continue;
            }

            if (const auto it = by_vpath.find(vpath); it != by_vpath.end())
            {
                violations.push_back(lux::format(
                    "duplicate vpath '{}': '{}' vs '{}'",
                    vpath, rel, it->second));
                continue;
            }
            if (const auto it = by_folded.find(foldCaseAscii(vpath));
                it != by_folded.end() && it->second != vpath)
            {
                violations.push_back(lux::format(
                    "case-insensitive vpath clash: '{}' vs '{}'",
                    vpath, it->second));
                continue;
            }
            if (const auto it = by_id.find(probe.id); it != by_id.end())
            {
                violations.push_back(lux::format(
                    "duplicate uuid: '{}' vs '{}'", rel, it->second));
                continue;
            }

            by_vpath.emplace(vpath, rel);
            by_folded.emplace(foldCaseAscii(vpath), vpath);
            by_id.emplace(probe.id, rel);
            entries.push_back(detail::PakWriteEntry{
                probe.id, probe.magic, vpath, file });
        }

        if (!violations.empty())
        {
            std::string msg = lux::format(
                "cook rejected — {} violation(s):", violations.size());
            for (const auto& v : violations)
            {
                msg += "\n  ";
                msg += v;
            }
            return lux::cxx::unexpected(std::move(msg));
        }
        if (entries.empty())
            return lux::cxx::unexpected(
                lux::format("no assets under '{}'", content_dir.string()));

        std::uintmax_t payload_bytes = 0;
        for (const auto& e : entries)
        {
            payload_bytes += std::filesystem::file_size(e.source_file, ec);
            if (ec)
                return lux::cxx::unexpected(lux::format(
                    "cannot stat '{}'", e.source_file.string()));
        }

        const std::size_t count = entries.size();
        std::string write_err;
        if (!detail::writePakFile(out_pak, std::move(entries), mount_hint,
                                  &write_err))
        {
            return lux::cxx::unexpected(std::move(write_err));
        }
        return PakCookResult{ count, payload_bytes };
    }

    lux::cxx::expected<PakInspectInfo, std::string>
    inspectPak(const std::filesystem::path& pak_path)
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
            return lux::cxx::unexpected(std::move(err));

        // Join PATH rows onto entries (entries are uuid-sorted).
        PakInspectInfo info;
        info.mount_hint = index.mount_hint;
        info.entries.reserve(index.entries.size());

        std::map<asset_id_t, std::string> path_by_id;
        for (auto& row : index.paths)
            path_by_id.emplace(row.id, std::move(row.vpath));

        for (const auto& e : index.entries)
        {
            PakInspectEntry out;
            out.id           = e.id;
            out.type         = assetTypeOfMagic(e.asset_magic);
            out.offset       = e.offset;
            out.size         = e.size;
            out.compression  = e.compression;
            out.tombstone    = e.tombstone();
            out.content_hash = e.content_hash;
            if (const auto it = path_by_id.find(e.id); it != path_by_id.end())
                out.vpath = it->second;
            info.entries.push_back(std::move(out));
        }
        return info;
    }
}
