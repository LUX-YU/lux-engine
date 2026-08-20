#pragma once
// ============================================================================
//  PakCook — cook a loose content folder into a .luxpak + inspect a pak.
//
//  The single cook entry point: lux_asset_packer's `pack` mode and the future
//  editor "Cook" menu both call cookDirectoryToPak in-process (the CLI is
//  argv glue only). v1 cook = WHOLE FOLDER — exactly the editor's load
//  behavior today, so the editor and shipped asset SETS are identical until
//  a dependency walker lands.
//
//  Unlike LooseAssetProvider::rescan (which picks deterministic winners and
//  keeps going), cook REJECTS hard on any identity hazard: duplicate uuid,
//  duplicate vpath (same stem twice), case-insensitive vpath clash (frozen
//  ruling: byte-sensitive resolve + cook-time clash rejection), or a file
//  whose path can't be a canonical virtual path. All violations are
//  collected into one error message — a cook never half-succeeds silently.
// ============================================================================

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/toolchain/asset/cook/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/visibility.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lux::toolchain
{
    struct PakCookResult
    {
        std::size_t    asset_count{ 0 };
        std::uintmax_t payload_bytes{ 0 };
        std::uintmax_t authoring_bytes_stripped{ 0 };
    };

    /// One cook source: a directory walked recursively, its files addressed
    /// under @p vpath_prefix ("" = at the mount root; "Scenes" = every file's
    /// vpath gains that leading segment). A project cook is typically two
    /// sources: {Content, ""} + {Scenes, "Scenes"}.
    struct PakCookSource
    {
        std::filesystem::path dir;
        std::string           vpath_prefix;   ///< no leading/trailing '/'
    };

    /// One already-cooked immutable image for an in-process Pak cook. This is
    /// the byte-owned counterpart of PakCookSource: scene cookers and Editor
    /// Play can publish LXSC/LXES/generated assets without first materializing
    /// a second loose-file tree. The image must be a complete Runtime image for
    /// @p type; the Pak writer never decodes or rewrites it.
    struct PakCookMemoryEntry final
    {
        lux::asset::asset_id_t id{};
        std::uint32_t magic_number{0u};
        std::string vpath;
        lux::cxx::SharedBytes<> image;
    };

    /// One already-cooked immutable image staged in a file. The Pak writer
    /// reads it positionally while publishing and never copies the complete
    /// image set into memory. The caller owns the source file until this call
    /// returns.
    struct PakCookFileEntry final
    {
        lux::asset::asset_id_t id{};
        std::uint32_t magic_number{0u};
        std::string vpath;
        std::filesystem::path image_path;
    };

    /// Cook every .luxasset/.luxmodel plus legacy cooked World image found in
    /// the given sources into @p out_pak (uuid-sorted and deterministic).
    /// Auxiliary asset payloads are Authoring metadata and are removed from
    /// the cooked image; Runtime header/info/data regions remain byte-identical.
    /// Identity hazards are collected across sources and reject the cook
    /// wholesale. New LXSC/LXES images carry explicit identity and therefore
    /// enter through cookSourcesAndFileEntriesToPak instead of a filename
    /// scanner. @p mount_hint is advisory; Runtime chooses the mounted root.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ASSET_COOK_PUBLIC
    lux::cxx::expected<PakCookResult, std::string>
    cookSourcesToPak(const std::vector<PakCookSource>& sources,
                     const std::filesystem::path&      out_pak,
                     std::string_view                  mount_hint = "/Game");

    /// Atomically cook ordinary loose sources together with already-cooked
    /// file-backed images. Loose assets still pass through the normal Runtime-
    /// image stripping path; explicit images are opaque and are never parsed
    /// or rewritten by Pak cook. UUID, exact-vpath and ASCII case-fold conflicts
    /// are validated across both input sets before the destination is touched.
    /// The caller retains every staged file and must keep it alive until the
    /// call returns. At least one source or explicit entry is required.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ASSET_COOK_PUBLIC
    lux::cxx::expected<PakCookResult, std::string>
    cookSourcesAndFileEntriesToPak(
        const std::vector<PakCookSource>& sources,
        std::vector<PakCookFileEntry> entries,
        const std::filesystem::path& out_pak,
        std::string_view mount_hint = "/Game");

    /// Single-directory convenience wrapper over cookSourcesToPak.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ASSET_COOK_PUBLIC
    lux::cxx::expected<PakCookResult, std::string>
    cookDirectoryToPak(const std::filesystem::path& content_dir,
                       const std::filesystem::path& out_pak,
                       std::string_view             mount_hint = "/Game");

    /// Atomically publish a set of owning cooked images through the same
    /// paged immutable LUXPAK writer used by the directory cook. Duplicate
    /// identities/paths, unsupported types and empty images reject the entire
    /// operation before the destination is replaced.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ASSET_COOK_PUBLIC
    lux::cxx::expected<PakCookResult, std::string>
    cookMemoryEntriesToPak(
        std::vector<PakCookMemoryEntry> entries,
        const std::filesystem::path& out_pak,
        std::string_view mount_hint = "/Game");

    /// File-backed counterpart of cookMemoryEntriesToPak. Header/index state
    /// remains bounded by entry count while payload bytes are copied directly
    /// from the staged immutable files into the atomically published Pak.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ASSET_COOK_PUBLIC
    lux::cxx::expected<PakCookResult, std::string>
    cookFileEntriesToPak(
        std::vector<PakCookFileEntry> entries,
        const std::filesystem::path& out_pak,
        std::string_view mount_hint = "/Game");

    /// One row of inspectPak output (public-friendly mirror of the codec's
    /// entry + its PATH row, joined).
    struct PakInspectEntry
    {
        lux::asset::asset_id_t id{};
        std::uint32_t magic_number{0u};
        std::string   vpath;            ///< Mount-relative; empty if none.
        std::uint64_t offset{ 0 };
        std::uint64_t size{ 0 };
        std::uint8_t  compression{ 0 };
        bool          tombstone{ false };
        lux::cxx::algorithm::Sha256Digest content_digest;
    };

    struct PakInspectInfo
    {
        std::string                  mount_hint;
        std::vector<PakInspectEntry> entries;   ///< uuid-ascending.
    };

    /// Read + validate a pak's index for display (lux_asset_packer
    /// `pak-inspect`). Never touches payload bytes.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_ASSET_COOK_PUBLIC
    lux::cxx::expected<PakInspectInfo, std::string>
    inspectPak(const std::filesystem::path& pak_path);

} // namespace lux::toolchain
