#pragma once

// LUXPAK v2 is a generated, immutable container. Its 256-byte header points
// directly at an Entry and a Path B+tree root. Every index page is exactly
// 4 KiB and every parent row carries the SHA-256 of its child. Opening a Pak
// therefore reads only the header and two roots; payload and metadata lookup
// use independent positional reads and a bounded provider-side page cache.

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lux::asset::detail
{
    inline constexpr std::uint32_t pakFourcc(char a, char b, char c, char d) noexcept
    {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8u) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16u) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24u);
    }

    inline constexpr std::uint8_t kPakFileMagic[8] = {'L', 'U', 'X', 'P', 'A', 'K', '\0', '\0'};
    inline constexpr std::uint32_t kPakEndianTag = 0x01020304u;
    inline constexpr std::uint32_t kPakVersion = 2u;
    inline constexpr std::uint32_t kPakPageMagic = pakFourcc('L', 'P', 'G', '2');
    inline constexpr std::uint32_t kPakPageSize = 4096u;
    inline constexpr std::uint64_t kPakPayloadAlign = 16u;
    inline constexpr std::uint8_t kPakCompressionNone = 0u;
    inline constexpr std::uint8_t kPakEntryFlagEncrypted = 0x01u;
    inline constexpr std::uint8_t kPakEntryFlagTombstone = 0x02u;
    inline constexpr std::uint64_t kMaxPakEntries = 1ull << 24u;
    inline constexpr std::size_t kPakMountHintBytes = 96u;

    enum class EPakPageKind : std::uint8_t
    {
        ENTRY_LEAF,
        ENTRY_INTERNAL,
        PATH_LEAF,
        PATH_INTERNAL
    };

    struct PakHeader final
    {
        std::uint8_t magic[8]{};
        std::uint32_t endian_tag{0u};
        std::uint32_t version{0u};
        std::uint32_t page_size{0u};
        std::uint32_t flags{0u};
        std::uint64_t entry_root_offset{0u};
        std::uint64_t path_root_offset{0u};
        std::uint64_t entry_count{0u};
        std::uint64_t path_count{0u};
        std::uint64_t index_page_count{0u};
        std::uint64_t payload_end{0u};
        std::uint32_t mount_hint_size{0u};
        char mount_hint[kPakMountHintBytes]{};
        lux::cxx::algorithm::Sha256Digest entry_root_digest;
        lux::cxx::algorithm::Sha256Digest path_root_digest;
        std::uint8_t reserved[20]{};
    };
    static_assert(sizeof(PakHeader) == 256u);
    static_assert(std::is_trivially_copyable_v<PakHeader>);

    struct PakPageHeader final
    {
        std::uint32_t magic{0u};
        std::uint16_t version{0u};
        EPakPageKind kind{EPakPageKind::ENTRY_LEAF};
        std::uint8_t level{0u};
        std::uint16_t count{0u};
        std::uint16_t reserved{0u};
        std::uint32_t used_bytes{0u};
        std::uint64_t next_leaf_offset{0u};
    };
    static_assert(sizeof(PakPageHeader) == 24u);
    static_assert(std::is_trivially_copyable_v<PakPageHeader>);

    using PakPage = std::array<std::byte, kPakPageSize>;

    struct PakEntry final
    {
        AssetId id{};
        std::uint64_t offset{0u};
        std::uint64_t size{0u};
        std::uint64_t uncompressed_size{0u};
        std::uint32_t asset_magic{0u};
        std::uint8_t compression{kPakCompressionNone};
        std::uint8_t flags{0u};
        lux::cxx::algorithm::Sha256Digest content_digest;
        std::string vpath;

        [[nodiscard]] bool tombstone() const noexcept
        {
            return (flags & kPakEntryFlagTombstone) != 0u;
        }
    };

    struct PakEntryChild final
    {
        AssetId maximum_key{};
        std::uint64_t offset{0u};
        lux::cxx::algorithm::Sha256Digest digest;
    };

    struct PakPathRow final
    {
        std::string vpath;
        AssetId id{};
    };

    struct PakPathChild final
    {
        std::string maximum_key;
        std::uint64_t offset{0u};
        lux::cxx::algorithm::Sha256Digest digest;
    };

    bool writePakFileImpl(
        const std::filesystem::path& out_pak,
        std::vector<PakWriteEntry> entries,
        std::string_view mount_hint,
        std::string* error_out = nullptr
    );

    LUX_ASSET_PUBLIC bool
    readPakHeader(std::istream& stream, std::uint64_t file_size, PakHeader& output, std::string* error_out = nullptr);

    LUX_ASSET_PUBLIC bool readPakPage(
        std::istream& stream,
        std::uint64_t file_size,
        std::uint64_t offset,
        PakPage& output,
        std::string* error_out = nullptr
    );

    [[nodiscard]] LUX_ASSET_PUBLIC bool
    verifyPakPageDigest(const PakPage& page, const lux::cxx::algorithm::Sha256Digest& expected) noexcept;

    [[nodiscard]] LUX_ASSET_PUBLIC PakPageHeader pakPageHeader(const PakPage& page) noexcept;

    LUX_ASSET_PUBLIC bool
    decodeEntryLeaf(const PakPage& page, std::vector<PakEntry>& output, std::string* error_out = nullptr);

    LUX_ASSET_PUBLIC bool
    decodeEntryInternal(const PakPage& page, std::vector<PakEntryChild>& output, std::string* error_out = nullptr);

    LUX_ASSET_PUBLIC bool
    decodePathLeaf(const PakPage& page, std::vector<PakPathRow>& output, std::string* error_out = nullptr);

    LUX_ASSET_PUBLIC bool
    decodePathInternal(const PakPage& page, std::vector<PakPathChild>& output, std::string* error_out = nullptr);

    /// Explicit inspection path. Unlike normal Provider startup this walks
    /// the complete Entry tree, but still verifies every parent/child digest.
    LUX_ASSET_PUBLIC bool readAllPakEntries(
        std::istream& stream,
        std::uint64_t file_size,
        const PakHeader& header,
        std::vector<PakEntry>& output,
        std::string* error_out = nullptr
    );
} // namespace lux::asset::detail
