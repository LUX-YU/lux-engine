#pragma once

#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lux::asset
{
    /// One opaque LUXPAK v2 payload. Exactly one of source_file and
    /// source_bytes must identify a non-empty image.
    struct PakWriteEntry final
    {
        AssetId id{};
        std::uint32_t asset_magic{0u};
        std::string vpath;
        std::filesystem::path source_file;
        lux::cxx::SharedBytes<> source_bytes;
        bool tombstone{}; // Memory encoding preserves deletion records without a payload.
    };

    [[nodiscard]] LUX_ASSET_PUBLIC bool writePakFile(
        const std::filesystem::path& out_pak,
        std::vector<PakWriteEntry> entries,
        std::string_view mount_hint = "/Game",
        std::string* error_out = nullptr
    );

    // Pure CPU encoding over owning byte captures; no filesystem access.
    [[nodiscard]] LUX_ASSET_PUBLIC lux::cxx::expected<std::vector<std::byte>, std::string>
    encodePak(const std::vector<PakWriteEntry>& entries, std::size_t byte_limit,
              std::string_view mount_hint = "/Game");

    struct PakInspectEntry final
    {
        AssetId id{};
        std::uint32_t magic_number{0u};
        std::string vpath;
        std::uint64_t offset{0u};
        std::uint64_t size{0u};
        std::uint8_t compression{0u};
        bool tombstone{false};
        lux::cxx::algorithm::Sha256Digest content_digest;
    };

    struct PakInspectInfo final
    {
        std::string mount_hint;
        std::vector<PakInspectEntry> entries;
    };

    struct PakDecodedEntry final
    {
        PakInspectEntry metadata;
        lux::cxx::SharedBytes<> bytes; // Owning slice of the input image; empty for tombstones.
    };
    struct PakDecodedImage final
    {
        std::string mount_hint;
        std::vector<PakDecodedEntry> entries; // Sorted by AssetId, as in the on-disk entry index.
    };

    // Pure, bounded decoding after a Process read. Validates both index trees and payload digests;
    // payload slices retain the exact input image without re-reading a mutable filesystem path.
    [[nodiscard]] LUX_ASSET_PUBLIC lux::cxx::expected<PakDecodedImage, std::string>
    decodePak(const lux::cxx::SharedBytes<>& image, std::size_t entry_limit);

    [[nodiscard]] LUX_ASSET_PUBLIC lux::cxx::expected<PakInspectInfo, std::string>
    inspectPak(const std::filesystem::path& pak_path);
} // namespace lux::asset
