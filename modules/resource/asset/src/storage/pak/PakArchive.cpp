#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>

#include <lux/engine/resource/asset/storage/VirtualPath.hpp>
#include <lux/engine/resource/asset/storage/pak/PakCodec.hpp>

#include <lux/cxx/core/Format.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace lux::asset
{
namespace
{
bool fail(std::string *error_out, std::string message)
{
    if (error_out != nullptr)
    {
        *error_out = std::move(message);
    }
    return false;
}
} // namespace

bool writePakFile(const std::filesystem::path &out_pak, std::vector<PakWriteEntry> entries, std::string_view mount_hint,
                  std::string *error_out)
{
    if (out_pak.empty())
    {
        return fail(error_out, "Pak output path is empty");
    }
    if (!VirtualPath::isLegalRoot(mount_hint))
    {
        return fail(error_out, lux::format("illegal mount hint '{}'", mount_hint));
    }
    if (entries.empty())
    {
        return fail(error_out, "no Pak entries");
    }

    std::unordered_map<AssetId, std::size_t> by_id;
    std::unordered_map<std::string, std::size_t> by_path;
    std::unordered_map<std::string, std::string> by_folded_path;
    for (std::size_t index = 0u; index < entries.size(); ++index)
    {
        const auto &entry = entries[index];
        if (entry.id.isNull())
        {
            return fail(error_out, "Pak entry has nil UUID");
        }
        if (!by_id.emplace(entry.id, index).second)
        {
            return fail(error_out, "duplicate Pak entry UUID");
        }
        if (entry.asset_magic == 0u)
        {
            return fail(error_out, "Pak entry has zero magic");
        }
        if (const auto path_error = VirtualPath::validateRelative(entry.vpath); !entry.vpath.empty() && path_error)
        {
            return fail(error_out,
                        lux::format("non-canonical vpath '{}' (err={})", entry.vpath, static_cast<int>(*path_error)));
        }
        if (entry.tombstone)
        {
            if (!entry.source_file.empty() || !entry.source_bytes.empty())
            {
                return fail(error_out, "Pak tombstone cannot carry a payload");
            }
            continue;
        }
        if (!entry.vpath.empty() && !by_path.emplace(entry.vpath, index).second)
        {
            return fail(error_out, "duplicate Pak virtual path");
        }
        const auto folded = foldCaseAscii(entry.vpath);
        if (const auto found = by_folded_path.find(folded);
            !entry.vpath.empty() && found != by_folded_path.end() && found->second != entry.vpath)
        {
            return fail(error_out, "case-insensitive Pak virtual path clash");
        }
        by_folded_path.emplace(folded, entry.vpath);

        const bool has_file = !entry.source_file.empty();
        const bool has_bytes = !entry.source_bytes.empty();
        if (has_file == has_bytes)
        {
            return fail(error_out, "Pak entry must have exactly one payload source");
        }
        if (has_file)
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(entry.source_file, error);
            if (error || size == 0u)
            {
                return fail(error_out, "Pak entry source file is empty or unreadable");
            }
        }
    }

    return detail::writePakFileImpl(out_pak, std::move(entries), mount_hint, error_out);
}

lux::cxx::expected<std::vector<std::byte>, std::string> encodePak(const std::vector<PakWriteEntry> &entries,
                                                                  std::size_t byte_limit, std::string_view mount_hint)
{
    if (!VirtualPath::isLegalRoot(mount_hint) || entries.empty() || !byte_limit)
    {
        return lux::cxx::unexpected(std::string{"invalid Pak encoding arguments"});
    }
    std::unordered_map<AssetId, std::size_t> by_id;
    std::unordered_map<std::string, std::size_t> by_path;
    for (std::size_t index{}; index < entries.size(); ++index)
    {
        const auto &entry = entries[index];
        const bool invalid_payload =
            !entry.source_file.empty() || (entry.tombstone ? !entry.source_bytes.empty() : entry.source_bytes.empty());
        if (entry.id.isNull() || !entry.asset_magic || invalid_payload)
        {
            return lux::cxx::unexpected(std::string{"Pak encoding requires typed, owned byte payloads"});
        }
        if (!entry.vpath.empty() && VirtualPath::validateRelative(entry.vpath))
        {
            return lux::cxx::unexpected(std::string{"non-canonical Pak virtual path"});
        }
        if (!by_id.emplace(entry.id, index).second ||
            (!entry.tombstone && !entry.vpath.empty() && !by_path.emplace(foldCaseAscii(entry.vpath), index).second))
        {
            return lux::cxx::unexpected(std::string{"duplicate Pak identity or virtual path"});
        }
    }
    return detail::encodePakImpl(entries, byte_limit, mount_hint);
}

lux::cxx::expected<PakInspectInfo, std::string> inspectPak(const std::filesystem::path &pak_path)
{
    std::error_code error;
    const auto file_size = std::filesystem::file_size(pak_path, error);
    if (error)
    {
        return lux::cxx::unexpected(lux::format("cannot stat '{}'", pak_path.string()));
    }

    std::ifstream stream(pak_path, std::ios::binary);
    if (!stream)
    {
        return lux::cxx::unexpected(lux::format("cannot open '{}'", pak_path.string()));
    }

    detail::PakHeader header;
    std::string message;
    if (!detail::readPakHeader(stream, file_size, header, &message))
    {
        return lux::cxx::unexpected(std::move(message));
    }

    std::vector<detail::PakEntry> entries;
    if (!detail::readAllPakEntries(stream, file_size, header, entries, &message))
    {
        return lux::cxx::unexpected(std::move(message));
    }

    PakInspectInfo info;
    info.mount_hint.assign(header.mount_hint, header.mount_hint + header.mount_hint_size);
    info.entries.reserve(entries.size());
    for (const auto &entry : entries)
    {
        info.entries.push_back(PakInspectEntry{
            entry.id,
            entry.asset_magic,
            entry.vpath,
            entry.offset,
            entry.size,
            entry.compression,
            entry.tombstone(),
            entry.content_digest,
        });
    }
    return info;
}

lux::cxx::expected<PakDecodedImage, std::string> decodePak(const lux::cxx::SharedBytes<> &image,
                                                           std::size_t entry_limit)
{
    detail::PakHeader header;
    std::string error;
    if (!detail::readPakHeader(image.view(), header, &error))
    {
        return lux::cxx::unexpected(std::move(error));
    }
    if (header.entry_count > entry_limit)
    {
        return lux::cxx::unexpected(std::string{"Pak entry limit exceeded"});
    }
    std::vector<detail::PakEntry> entries;
    std::vector<detail::PakPathRow> paths;
    if (!detail::readAllPakEntries(image.view(), header, entries, &error) ||
        !detail::readAllPakPaths(image.view(), header, paths, &error))
    {
        return lux::cxx::unexpected(std::move(error));
    }
    PakDecodedImage decoded;
    decoded.mount_hint.assign(header.mount_hint, header.mount_hint_size);
    if (!VirtualPath::isLegalRoot(decoded.mount_hint))
    {
        return lux::cxx::unexpected(std::string{"invalid Pak mount hint"});
    }
    std::size_t live_paths{};
    for (const auto &entry : entries)
    {
        if (entry.id.isNull() || !entry.asset_magic)
        {
            return lux::cxx::unexpected(std::string{"invalid Pak entry identity"});
        }
        if (!entry.tombstone() && !entry.vpath.empty())
        {
            if (VirtualPath::validateRelative(entry.vpath))
            {
                return lux::cxx::unexpected(std::string{"non-canonical Pak virtual path"});
            }
            ++live_paths;
        }
    }
    if (paths.size() != live_paths)
    {
        return lux::cxx::unexpected(std::string{"Pak path and entry cardinality mismatch"});
    }
    for (const auto &path : paths)
    {
        const auto found = std::lower_bound(entries.begin(), entries.end(), path.id,
                                            [](const auto &entry, const auto &id) { return entry.id < id; });
        const bool missing = found == entries.end() || found->id != path.id;
        if (missing || found->tombstone() || found->vpath != path.vpath)
        {
            return lux::cxx::unexpected(std::string{"Pak path and entry identity mismatch"});
        }
    }
    decoded.entries.reserve(entries.size());
    for (auto &entry : entries)
    {
        lux::cxx::SharedBytes<> payload;
        if (!entry.tombstone())
        {
            if (entry.compression != detail::kPakCompressionNone)
            {
                return lux::cxx::unexpected(std::string{"unsupported Pak payload compression"});
            }
            const bool invalid_size = !entry.size || entry.size != entry.uncompressed_size;
            const bool invalid_range = entry.offset < 256 || entry.offset > header.payload_end ||
                                       entry.size > header.payload_end - entry.offset;
            if (invalid_size || invalid_range)
            {
                return lux::cxx::unexpected(std::string{"Pak payload range is out of bounds"});
            }
            payload = image.subspan(static_cast<std::size_t>(entry.offset), static_cast<std::size_t>(entry.size));
            if (lux::cxx::algorithm::Sha256::hash(payload.view()) != entry.content_digest)
            {
                return lux::cxx::unexpected(std::string{"Pak payload digest mismatch"});
            }
        }
        decoded.entries.push_back({{entry.id, entry.asset_magic, std::move(entry.vpath), entry.offset, entry.size,
                                    entry.compression, entry.tombstone(), entry.content_digest},
                                   std::move(payload)});
    }
    return decoded;
}
} // namespace lux::asset
