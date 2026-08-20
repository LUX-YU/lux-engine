#include <lux/engine/toolchain/asset/cook/PakCook.hpp>
#include <lux/engine/resource/asset/AssetHeaderProbe.hpp>
#include <lux/engine/resource/asset/PakCodec.hpp>
#include <lux/engine/resource/asset/VirtualPath.hpp>

#include <algorithm>
#include <lux/cxx/core/Format.hpp>
#include <fstream>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <unordered_map>

namespace lux::toolchain
{
    using lux::asset::EAssetType;
    using lux::asset::VirtualPath;
    using lux::asset::assetTypeOfMagic;
    using lux::asset::asset_id_t;
    using lux::asset::foldCaseAscii;
    using lux::asset::kEntitySceneImageMagic;
    using lux::asset::kEntitySectionImageMagic;
    using lux::asset::readAssetHeader;
    namespace
    {
        [[nodiscard]] std::uint32_t entryMagic(
            EAssetType type) noexcept
        {
            using lux::asset::asset_magic_number_of;
            switch (type)
            {
            case EAssetType::MESH:
                return asset_magic_number_of<EAssetType::MESH>::value;
            case EAssetType::MODEL:
                return asset_magic_number_of<EAssetType::MODEL>::value;
            case EAssetType::TEXTURE:
                return asset_magic_number_of<EAssetType::TEXTURE>::value;
            case EAssetType::SHADER:
                return asset_magic_number_of<EAssetType::SHADER>::value;
            case EAssetType::SCRIPT:
                return asset_magic_number_of<EAssetType::SCRIPT>::value;
            case EAssetType::SKELETON:
                return asset_magic_number_of<EAssetType::SKELETON>::value;
            case EAssetType::ANIMATION_CLIP:
                return asset_magic_number_of<
                    EAssetType::ANIMATION_CLIP>::value;
            case EAssetType::MATERIAL:
                return asset_magic_number_of<EAssetType::MATERIAL>::value;
            case EAssetType::MATERIAL_INSTANCE:
                return asset_magic_number_of<
                    EAssetType::MATERIAL_INSTANCE>::value;
            case EAssetType::TEXTURE_ATLAS:
                return asset_magic_number_of<
                    EAssetType::TEXTURE_ATLAS>::value;
            case EAssetType::FLIPBOOK_CLIP:
                return asset_magic_number_of<
                    EAssetType::FLIPBOOK_CLIP>::value;
            case EAssetType::FLOW_GRAPH:
                return asset_magic_number_of<EAssetType::FLOW_GRAPH>::value;
            case EAssetType::ENTITY_SCENE:
                return kEntitySceneImageMagic;
            case EAssetType::ENTITY_SECTION:
                return kEntitySectionImageMagic;
            default:
                return 0u;
            }
        }

        lux::cxx::expected<lux::cxx::SharedBytes<>, std::string>
        cookedAssetImage(
            const std::filesystem::path& file,
            std::uintmax_t               file_size,
            std::uintmax_t&              stripped_bytes)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
                return lux::cxx::unexpected(
                    lux::format("cannot open '{}'", file.string()));

            lux::asset::AssetFileHeader header{};
            stream.read(
                reinterpret_cast<char*>(&header),
                static_cast<std::streamsize>(sizeof(header))
            );
            if (!stream)
                return lux::cxx::unexpected(
                    lux::format("cannot read asset header '{}'", file.string()));

            // v1 predates auxiliary blocks. Leave it untouched; its smaller
            // header layout cannot be interpreted as the current header.
            if (header.version != lux::asset::current_asset_version)
                return lux::cxx::SharedBytes<>{};

            const auto sectionEnd = [](std::uint64_t offset, std::uint64_t size)
                -> std::optional<std::uint64_t>
            {
                if (size > std::numeric_limits<std::uint64_t>::max() - offset)
                    return std::nullopt;
                return offset + size;
            };
            const auto info_end = sectionEnd(header.info_offset, header.info_size);
            const auto data_end = sectionEnd(header.data_offset, header.data_size);
            if (!info_end || !data_end)
                return lux::cxx::unexpected(
                    lux::format("asset section overflow '{}'", file.string()));

            const std::uint64_t runtime_size = std::max(*info_end, *data_end);
            if (runtime_size < sizeof(header) || runtime_size > file_size)
                return lux::cxx::unexpected(
                    lux::format("invalid asset sections '{}'", file.string()));
            if (runtime_size == file_size)
                return lux::cxx::SharedBytes<>{};

            auto storage = std::make_shared<std::vector<std::byte>>(
                static_cast<std::size_t>(runtime_size)
            );
            stream.seekg(0, std::ios::beg);
            stream.read(
                reinterpret_cast<char*>(storage->data()),
                static_cast<std::streamsize>(storage->size())
            );
            if (!stream)
                return lux::cxx::unexpected(
                    lux::format("cannot read runtime asset image '{}'", file.string()));

            auto owned = lux::cxx::SharedBytes<>::fromOwner(
                std::shared_ptr<const void>{storage, storage->data()},
                std::span<const std::byte>{storage->data(), storage->size()}
            );
            if (owned.empty())
                return lux::cxx::unexpected(
                    lux::format("cannot own runtime asset image '{}'", file.string()));
            stripped_bytes += file_size - runtime_size;
            return owned;
        }

        struct PendingPakEntry final
        {
            lux::asset::detail::PakWriteEntry entry;
            std::string origin;
        };

        struct PakCookDraft final
        {
            std::vector<PendingPakEntry> entries;
            std::vector<std::string> violations;
            std::uintmax_t authoring_bytes_stripped{0u};
        };

        struct VpathOwner final
        {
            std::string vpath;
            std::string origin;
        };

        [[nodiscard]] std::string rejectionMessage(
            const std::vector<std::string>& violations)
        {
            std::string message = lux::format(
                "cook rejected — {} violation(s):",
                violations.size());
            for (const auto& violation : violations)
            {
                message += "\n  ";
                message += violation;
            }
            return message;
        }

        void collectSourceEntries(
            const std::vector<PakCookSource>& sources,
            PakCookDraft& draft)
        {
            for (const auto& source : sources)
            {
                std::error_code error;
                if (!std::filesystem::is_directory(source.dir, error) || error)
                {
                    draft.violations.push_back(lux::format(
                        "'{}' is not a directory",
                        source.dir.string()));
                    continue;
                }
                if (!source.vpath_prefix.empty())
                {
                    if (const auto prefix_error =
                            VirtualPath::validateRelative(source.vpath_prefix))
                    {
                        draft.violations.push_back(lux::format(
                            "source '{}': non-canonical vpath prefix '{}' (err={})",
                            source.dir.string(),
                            source.vpath_prefix,
                            static_cast<int>(*prefix_error)));
                        continue;
                    }
                }

                std::vector<std::filesystem::path> files;
                std::filesystem::recursive_directory_iterator iterator(
                    source.dir,
                    error);
                const std::filesystem::recursive_directory_iterator end;
                if (error)
                {
                    draft.violations.push_back(lux::format(
                        "cannot walk '{}': {}",
                        source.dir.string(),
                        error.message()));
                    continue;
                }
                while (iterator != end)
                {
                    std::error_code entry_error;
                    const auto& entry = *iterator;
                    if (entry.is_regular_file(entry_error))
                    {
                        const auto extension = entry.path().extension().string();
                        if (extension == ".luxasset" ||
                            extension == ".luxmodel")
                        {
                            files.push_back(entry.path());
                        }
                    }
                    else if (entry_error)
                    {
                        draft.violations.push_back(lux::format(
                            "cannot inspect '{}': {}",
                            entry.path().string(),
                            entry_error.message()));
                    }

                    error.clear();
                    iterator.increment(error);
                    if (error)
                    {
                        draft.violations.push_back(lux::format(
                            "cannot walk '{}': {}",
                            source.dir.string(),
                            error.message()));
                        break;
                    }
                }

                std::sort(
                    files.begin(),
                    files.end(),
                    [&](const auto& lhs, const auto& rhs)
                    {
                        return lhs.lexically_relative(source.dir).generic_string()
                            < rhs.lexically_relative(source.dir).generic_string();
                    });

                for (const auto& file : files)
                {
                    const auto relative_path =
                        file.lexically_relative(source.dir);
                    const std::string relative = relative_path.generic_string();
                    auto virtual_relative = relative_path;
                    virtual_relative.replace_extension();
                    std::string vpath = virtual_relative.generic_string();
                    if (!source.vpath_prefix.empty())
                        vpath = source.vpath_prefix + "/" + vpath;

                    const auto probe = readAssetHeader(file);
                    const auto type = assetTypeOfMagic(probe.magic);
                    if (type == EAssetType::UNKNOWN || probe.id.is_nil())
                    {
                        draft.violations.push_back(lux::format(
                            "'{}': unrecognized or corrupt asset header",
                            relative));
                        continue;
                    }

                    lux::asset::detail::PakWriteEntry pak_entry{
                        probe.id,
                        probe.magic,
                        std::move(vpath),
                        file};
                    error.clear();
                    const auto size = std::filesystem::file_size(file, error);
                    if (error)
                    {
                        draft.violations.push_back(lux::format(
                            "'{}': cannot stat asset image",
                            relative));
                        continue;
                    }
                    auto cooked = cookedAssetImage(
                        file,
                        size,
                        draft.authoring_bytes_stripped);
                    if (!cooked)
                    {
                        draft.violations.push_back(
                            std::move(cooked.error()));
                        continue;
                    }
                    pak_entry.source_bytes = std::move(*cooked);
                    draft.entries.push_back({
                        std::move(pak_entry),
                        lux::format("source '{}'", file.string())});
                }
            }
        }

        void collectMemoryEntries(
            std::vector<PakCookMemoryEntry> inputs,
            PakCookDraft& draft)
        {
            draft.entries.reserve(draft.entries.size() + inputs.size());
            for (auto& input : inputs)
            {
                const std::string origin = lux::format(
                    "explicit memory entry '{}'",
                    input.vpath);
                draft.entries.push_back({
                    lux::asset::detail::PakWriteEntry{
                        input.id,
                        entryMagic(input.type),
                        std::move(input.vpath),
                        {},
                        std::move(input.image)},
                    origin});
            }
        }

        void collectFileEntries(
            std::vector<PakCookFileEntry> inputs,
            PakCookDraft& draft)
        {
            draft.entries.reserve(draft.entries.size() + inputs.size());
            for (auto& input : inputs)
            {
                const std::string origin = lux::format(
                    "explicit file entry '{}' from '{}'",
                    input.vpath,
                    input.image_path.string());
                draft.entries.push_back({
                    lux::asset::detail::PakWriteEntry{
                        input.id,
                        entryMagic(input.type),
                        std::move(input.vpath),
                        std::move(input.image_path),
                        {}},
                    origin});
            }
        }

        [[nodiscard]] lux::cxx::expected<PakCookResult, std::string>
        publishDraft(
            PakCookDraft draft,
            const std::filesystem::path& out_pak,
            std::string_view mount_hint)
        {
            if (!VirtualPath::isLegalRoot(mount_hint))
            {
                return lux::cxx::unexpected(
                    lux::format("illegal mount hint '{}'", mount_hint));
            }
            if (draft.entries.empty() && draft.violations.empty())
                return lux::cxx::unexpected(std::string{"no Pak cook inputs"});

            std::unordered_map<std::string, std::string> by_vpath;
            std::unordered_map<std::string, VpathOwner> by_folded;
            std::unordered_map<asset_id_t, std::string> by_id;
            std::uintmax_t payload_bytes = 0u;

            for (const auto& pending : draft.entries)
            {
                const auto& entry = pending.entry;
                if (entry.id.is_nil())
                {
                    draft.violations.push_back(lux::format(
                        "{}: nil uuid",
                        pending.origin));
                }
                else if (const auto duplicate = by_id.find(entry.id);
                         duplicate != by_id.end())
                {
                    draft.violations.push_back(lux::format(
                        "duplicate uuid: {} vs {}",
                        pending.origin,
                        duplicate->second));
                }
                else
                {
                    by_id.emplace(entry.id, pending.origin);
                }

                if (entry.asset_magic == 0u)
                {
                    draft.violations.push_back(lux::format(
                        "{}: unsupported asset type",
                        pending.origin));
                }

                if (const auto path_error =
                        VirtualPath::validateRelative(entry.vpath))
                {
                    draft.violations.push_back(lux::format(
                        "{}: non-canonical vpath '{}' (err={})",
                        pending.origin,
                        entry.vpath,
                        static_cast<int>(*path_error)));
                }
                else
                {
                    const auto exact = by_vpath.find(entry.vpath);
                    if (exact != by_vpath.end())
                    {
                        draft.violations.push_back(lux::format(
                            "duplicate vpath '{}': {} vs {}",
                            entry.vpath,
                            pending.origin,
                            exact->second));
                    }
                    else
                    {
                        by_vpath.emplace(entry.vpath, pending.origin);
                        const auto folded = foldCaseAscii(entry.vpath);
                        const auto insensitive = by_folded.find(folded);
                        if (insensitive != by_folded.end() &&
                            insensitive->second.vpath != entry.vpath)
                        {
                            draft.violations.push_back(lux::format(
                                "case-insensitive vpath clash: '{}' ({}) vs '{}' ({})",
                                entry.vpath,
                                pending.origin,
                                insensitive->second.vpath,
                                insensitive->second.origin));
                        }
                        else
                        {
                            by_folded.emplace(
                                std::move(folded),
                                VpathOwner{entry.vpath, pending.origin});
                        }
                    }
                }

                std::uintmax_t entry_bytes = 0u;
                if (!entry.source_bytes.empty())
                {
                    entry_bytes = entry.source_bytes.size();
                }
                else if (!entry.source_file.empty())
                {
                    std::error_code error;
                    entry_bytes = std::filesystem::file_size(
                        entry.source_file,
                        error);
                    if (error || entry_bytes == 0u)
                    {
                        draft.violations.push_back(lux::format(
                            "{}: cannot stat a non-empty cooked image",
                            pending.origin));
                        continue;
                    }
                }
                else
                {
                    draft.violations.push_back(lux::format(
                        "{}: empty cooked image",
                        pending.origin));
                    continue;
                }
                if (entry_bytes >
                    std::numeric_limits<std::uintmax_t>::max() - payload_bytes)
                {
                    draft.violations.push_back(
                        std::string{"Pak payload byte overflow"});
                    continue;
                }
                payload_bytes += entry_bytes;
            }

            if (!draft.violations.empty())
            {
                return lux::cxx::unexpected(
                    rejectionMessage(draft.violations));
            }

            std::vector<lux::asset::detail::PakWriteEntry> entries;
            entries.reserve(draft.entries.size());
            for (auto& pending : draft.entries)
                entries.push_back(std::move(pending.entry));

            const auto count = entries.size();
            std::string write_error;
            if (!lux::asset::detail::writePakFile(
                    out_pak,
                    std::move(entries),
                    mount_hint,
                    &write_error))
            {
                return lux::cxx::unexpected(std::move(write_error));
            }
            return PakCookResult{
                count,
                payload_bytes,
                draft.authoring_bytes_stripped};
        }
    } // namespace

    lux::cxx::expected<PakCookResult, std::string>
    cookSourcesToPak(const std::vector<PakCookSource>& sources,
                     const std::filesystem::path&      out_pak,
                     std::string_view                  mount_hint)
    {
        if (sources.empty())
            return lux::cxx::unexpected(std::string("no cook sources"));
        PakCookDraft draft;
        collectSourceEntries(sources, draft);
        return publishDraft(std::move(draft), out_pak, mount_hint);
    }

    lux::cxx::expected<PakCookResult, std::string>
    cookSourcesAndFileEntriesToPak(
        const std::vector<PakCookSource>& sources,
        std::vector<PakCookFileEntry> entries,
        const std::filesystem::path& out_pak,
        std::string_view mount_hint)
    {
        if (sources.empty() && entries.empty())
            return lux::cxx::unexpected(std::string{"no Pak cook inputs"});

        PakCookDraft draft;
        collectSourceEntries(sources, draft);
        collectFileEntries(std::move(entries), draft);
        return publishDraft(std::move(draft), out_pak, mount_hint);
    }

    lux::cxx::expected<PakCookResult, std::string>
    cookDirectoryToPak(const std::filesystem::path& content_dir,
                       const std::filesystem::path& out_pak,
                       std::string_view             mount_hint)
    {
        return cookSourcesToPak({ PakCookSource{ content_dir, "" } },
                                out_pak, mount_hint);
    }

    lux::cxx::expected<PakCookResult, std::string>
    cookMemoryEntriesToPak(
        std::vector<PakCookMemoryEntry> inputs,
        const std::filesystem::path& out_pak,
        std::string_view mount_hint)
    {
        if (inputs.empty())
            return lux::cxx::unexpected(std::string{"no cooked images"});
        PakCookDraft draft;
        collectMemoryEntries(std::move(inputs), draft);
        return publishDraft(std::move(draft), out_pak, mount_hint);
    }

    lux::cxx::expected<PakCookResult, std::string>
    cookFileEntriesToPak(
        std::vector<PakCookFileEntry> inputs,
        const std::filesystem::path& out_pak,
        std::string_view mount_hint)
    {
        if (inputs.empty())
            return lux::cxx::unexpected(std::string{"no cooked images"});
        return cookSourcesAndFileEntriesToPak(
            {},
            std::move(inputs),
            out_pak,
            mount_hint);
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

        lux::asset::detail::PakHeader header;
        std::string err;
        if (!lux::asset::detail::readPakHeader(
                stream, file_size, header, &err))
            return lux::cxx::unexpected(std::move(err));

        std::vector<lux::asset::detail::PakEntry> entries;
        if (!lux::asset::detail::readAllPakEntries(
                stream, file_size, header, entries, &err))
            return lux::cxx::unexpected(std::move(err));

        PakInspectInfo info;
        info.mount_hint.assign(
            header.mount_hint,
            header.mount_hint + header.mount_hint_size);
        info.entries.reserve(entries.size());

        for (const auto& e : entries)
        {
            PakInspectEntry out;
            out.id           = e.id;
            out.type         = assetTypeOfMagic(e.asset_magic);
            out.offset       = e.offset;
            out.size         = e.size;
            out.compression  = e.compression;
            out.tombstone    = e.tombstone();
            out.content_digest = e.content_digest;
            out.vpath = e.vpath;
            info.entries.push_back(std::move(out));
        }
        return info;
    }
} // namespace lux::toolchain
