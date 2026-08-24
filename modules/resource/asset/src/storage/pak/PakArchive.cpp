#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>

#include <lux/engine/resource/asset/storage/VirtualPath.hpp>
#include <lux/engine/resource/asset/storage/pak/PakCodec.hpp>

#include <lux/cxx/core/Format.hpp>

#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace lux::asset
{
    namespace
    {
        bool fail(std::string* error_out, std::string message)
        {
            if (error_out != nullptr)
                *error_out = std::move(message);
            return false;
        }
    } // namespace

    bool writePakFile(
        const std::filesystem::path& out_pak,
        std::vector<PakWriteEntry> entries,
        std::string_view mount_hint,
        std::string* error_out)
    {
        if (out_pak.empty())
            return fail(error_out, "Pak output path is empty");
        if (!VirtualPath::isLegalRoot(mount_hint))
            return fail(error_out, lux::format("illegal mount hint '{}'", mount_hint));
        if (entries.empty())
            return fail(error_out, "no Pak entries");

        std::unordered_map<AssetId, std::size_t> by_id;
        std::unordered_map<std::string, std::size_t> by_path;
        std::unordered_map<std::string, std::string> by_folded_path;
        for (std::size_t index = 0u; index < entries.size(); ++index)
        {
            const auto& entry = entries[index];
            if (entry.id.isNull())
                return fail(error_out, "Pak entry has nil UUID");
            if (!by_id.emplace(entry.id, index).second)
                return fail(error_out, "duplicate Pak entry UUID");
            if (entry.asset_magic == 0u)
                return fail(error_out, "Pak entry has zero magic");
            if (const auto path_error = VirtualPath::validateRelative(entry.vpath))
            {
                return fail(
                    error_out,
                    lux::format(
                        "non-canonical vpath '{}' (err={})",
                        entry.vpath,
                        static_cast<int>(*path_error)
                    )
                );
            }
            if (!by_path.emplace(entry.vpath, index).second)
                return fail(error_out, "duplicate Pak virtual path");
            const auto folded = foldCaseAscii(entry.vpath);
            if (const auto found = by_folded_path.find(folded);
                found != by_folded_path.end() && found->second != entry.vpath)
            {
                return fail(error_out, "case-insensitive Pak virtual path clash");
            }
            by_folded_path.emplace(folded, entry.vpath);

            const bool has_file = !entry.source_file.empty();
            const bool has_bytes = !entry.source_bytes.empty();
            if (has_file == has_bytes)
                return fail(error_out, "Pak entry must have exactly one payload source");
            if (has_file)
            {
                std::error_code error;
                const auto size = std::filesystem::file_size(entry.source_file, error);
                if (error || size == 0u)
                    return fail(error_out, "Pak entry source file is empty or unreadable");
            }
        }

        return detail::writePakFileImpl(
            out_pak,
            std::move(entries),
            mount_hint,
            error_out
        );
    }

    lux::cxx::expected<PakInspectInfo, std::string>
    inspectPak(const std::filesystem::path& pak_path)
    {
        std::error_code error;
        const auto file_size = std::filesystem::file_size(pak_path, error);
        if (error)
        {
            return lux::cxx::unexpected(
                lux::format("cannot stat '{}'", pak_path.string())
            );
        }

        std::ifstream stream(pak_path, std::ios::binary);
        if (!stream)
        {
            return lux::cxx::unexpected(
                lux::format("cannot open '{}'", pak_path.string())
            );
        }

        detail::PakHeader header;
        std::string message;
        if (!detail::readPakHeader(stream, file_size, header, &message))
            return lux::cxx::unexpected(std::move(message));

        std::vector<detail::PakEntry> entries;
        if (!detail::readAllPakEntries(
                stream,
                file_size,
                header,
                entries,
                &message
            ))
        {
            return lux::cxx::unexpected(std::move(message));
        }

        PakInspectInfo info;
        info.mount_hint.assign(
            header.mount_hint,
            header.mount_hint + header.mount_hint_size
        );
        info.entries.reserve(entries.size());
        for (const auto& entry : entries)
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
} // namespace lux::asset
