#pragma once
/**
 * @file PakCodec.hpp (private)
 * @brief Reader/writer for the .luxpak container format.
 *
 * Used by PakAssetProvider (read) and the cook pipeline (write). NOT part of
 * any public API — tools go through <lux/engine/asset/PakCook.hpp>. Mirrors
 * MaterialGraphCodec discipline: little-endian, magic+endian+version, fourcc
 * trailer, defensive caps, tolerant-versioning, sticky-failure decode — but
 * tuned for RANDOM ACCESS: the index is footer-located (UE FPakInfo style) so
 * opening a pak never scans payloads.
 *
 * File layout (design: .internal/plan/virtual-path-pak-design.md §4):
 *
 *   offset 0   u8[8]  "LUXPAK\0\0"
 *              [ entry payload region — each entry 16-aligned, uuid-ascending,
 *                VERBATIM .luxasset file images (exportAsLuxAsset bytes) ]
 *              [ index region — one ByteWriter blob, see below ]
 *   EOF-64     PakFooter (fixed 64 bytes, trivially copyable, memcpy'd)
 *
 *   PakFooter:
 *     u32 magic='LPAK' | u32 endian=0x01020304 | u32 version=1
 *     u32 flags (bit0 = index encrypted, v2)
 *     u64 index_offset | u64 index_size
 *     u64 index_hash | u32 hash_algo (1 = FNV-1a64; integrity, not security)
 *     u8  key_id[16] (encryption key id, all-zero in v1)
 *     u32 trailer='LPKE'
 *
 *   index blob:
 *     u32 'LPKI' | u32 endian | u32 version | str mount_hint ("/Game" —
 *     advisory only; the actual root is decided at mount() time)
 *     sections: { u32 fourcc; u64 size; bytes } — UNKNOWN TAGS ARE SKIPPED
 *     (forward compat); terminated by u32 'PLIE'.
 *
 *   'ENTB' (uuid-ascending, binary-searchable in place), 56 bytes/entry:
 *     uuid[16] | u64 offset | u64 size | u64 uncompressed_size
 *     | u32 asset_magic (the per-type .luxasset magic — type queries and
 *       SerDeser dispatch never touch the payload)
 *     | u8 compression (v1: 0=NONE; 1=DEFLATE/2=ZSTD reserved — an unknown
 *       value is a SINGLE-ENTRY failure at open(), never a whole-pak reject)
 *     | u8 flags (bit0=encrypted v2, bit1=tombstone) | u8 pad[2]
 *     | u64 content_hash (FNV-1a64 of the payload; 0 = absent)
 *
 *   'PATH' (vpath byte-ascending => unique):
 *     { str vpath (mount-relative canonical, no extension, <=1024); uuid[16] }
 *     load-validated: every uuid present in ENTB, every vpath canonical.
 *
 *   Reserved section tags (seams, unimplemented): 'META' searchable tags,
 *   'ALIA' path-redirect rows (cross-patch renames), 'SIGN' signatures.
 *
 * DETERMINISM: uuid-sorted entries, byte-sorted paths, zero padding, and NO
 * TIMESTAMPS anywhere in the format — identical input bytes produce a
 * byte-identical pak (patch diffing + reproducible CI).
 */

#include <lux/engine/asset/Asset.hpp>
#include <lux/engine/resource/visibility.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

namespace lux::asset::detail
{
    // fourcc helpers ('LPAK' stored little-endian: 'L' is the lowest byte).
    inline constexpr std::uint32_t pakFourcc(char a, char b, char c, char d) noexcept
    {
        return  static_cast<std::uint32_t>(static_cast<unsigned char>(a))
             | (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8)
             | (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16)
             | (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
    }

    inline constexpr std::uint8_t  kPakFileMagic[8]
        = { 'L', 'U', 'X', 'P', 'A', 'K', '\0', '\0' };
    inline constexpr std::uint32_t kPakFooterMagic   = pakFourcc('L','P','A','K');
    inline constexpr std::uint32_t kPakFooterTrailer = pakFourcc('L','P','K','E');
    inline constexpr std::uint32_t kPakIndexMagic    = pakFourcc('L','P','K','I');
    inline constexpr std::uint32_t kPakIndexTrailer  = pakFourcc('P','L','I','E');
    inline constexpr std::uint32_t kPakSectionEntb   = pakFourcc('E','N','T','B');
    inline constexpr std::uint32_t kPakSectionPath   = pakFourcc('P','A','T','H');
    inline constexpr std::uint32_t kPakEndianTag     = 0x01020304u;
    inline constexpr std::uint32_t kPakVersion       = 1u;
    inline constexpr std::uint32_t kPakHashFnv1a64   = 1u;
    inline constexpr std::uint64_t kPakAlign         = 16u;
    inline constexpr std::size_t   kPakEntryBytes    = 56u;

    inline constexpr std::uint8_t  kPakCompressionNone   = 0u;
    inline constexpr std::uint8_t  kPakEntryFlagEncrypted = 0x01u; // v2
    inline constexpr std::uint8_t  kPakEntryFlagTombstone = 0x02u;

    // Defensive caps (decode failure, not allocation death).
    inline constexpr std::uint64_t kMaxPakEntries   = 1ull << 24;
    inline constexpr std::uint64_t kMaxPakIndexSize = 1ull << 30;

    /// Fixed 64-byte footer, memcpy'd from EOF-64.
    struct PakFooter
    {
        std::uint32_t magic;
        std::uint32_t endian_tag;
        std::uint32_t version;
        std::uint32_t flags;
        std::uint64_t index_offset;
        std::uint64_t index_size;
        std::uint64_t index_hash;
        std::uint32_t hash_algo;
        std::uint8_t  key_id[16];
        std::uint32_t trailer;
    };
    static_assert(sizeof(PakFooter) == 64);
    static_assert(std::is_trivially_copyable_v<PakFooter>);

    /// FNV-1a 64 (integrity, not security — hash_algo is the upgrade path).
    inline std::uint64_t fnv1a64(const void* p, std::size_t n,
                                 std::uint64_t h = 14695981039346656037ull) noexcept
    {
        const auto* s = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i)
        {
            h ^= s[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    /// Decoded ENTB row.
    struct PakEntry
    {
        asset_id_t    id{};
        std::uint64_t offset{ 0 };
        std::uint64_t size{ 0 };
        std::uint64_t uncompressed_size{ 0 };
        std::uint32_t asset_magic{ 0 };
        std::uint8_t  compression{ kPakCompressionNone };
        std::uint8_t  flags{ 0 };
        std::uint64_t content_hash{ 0 };

        [[nodiscard]] bool tombstone() const noexcept
        {
            return (flags & kPakEntryFlagTombstone) != 0;
        }
    };

    struct PakPathRow
    {
        std::string vpath;
        asset_id_t  id{};
    };

    /// Decoded, validated index. entries uuid-ascending (binary-searchable);
    /// paths byte-ascending.
    struct PakIndex
    {
        std::string             mount_hint;
        std::vector<PakEntry>   entries;
        std::vector<PakPathRow> paths;
    };

    /// Writer input. Payload bytes are STREAM-COPIED verbatim from
    /// source_file (a complete .luxasset image) — the codec never re-encodes.
    /// Empty vpath = no PATH row (the future tombstone/patch shape).
    struct PakWriteEntry
    {
        asset_id_t            id{};
        std::uint32_t         asset_magic{ 0 };
        std::string           vpath;
        std::filesystem::path source_file;
    };

    /// Write a .luxpak: sorts entries internally (uuid order; paths byte
    /// order), streams payloads 16-aligned, emits index + footer, writes to
    /// "<out>.tmp" then atomically renames. Validates unique ids, unique +
    /// canonical vpaths and a legal mount_hint. Exported (pinclude headers
    /// are not installed) so the module's own tests can link it.
    LUX_RESOURCE_PUBLIC bool
    writePakFile(const std::filesystem::path& out_pak,
                 std::vector<PakWriteEntry>   entries,
                 std::string_view             mount_hint,
                 std::string*                 error_out = nullptr);

    /// Read + fully validate footer and index from an open binary stream of
    /// @p file_size bytes. On failure returns false with a human-readable
    /// reason in *error_out; the stream position is unspecified afterwards.
    LUX_RESOURCE_PUBLIC bool
    readPakIndex(std::istream&  is,
                 std::uint64_t  file_size,
                 PakIndex&      out,
                 std::string*   error_out = nullptr);
}
