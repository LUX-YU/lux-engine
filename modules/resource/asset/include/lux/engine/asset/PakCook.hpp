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
//  Unlike LooseDirProvider::rescan (which picks deterministic winners and
//  keeps going), cook REJECTS hard on any identity hazard: duplicate uuid,
//  duplicate vpath (same stem twice), case-insensitive vpath clash (frozen
//  ruling: byte-sensitive resolve + cook-time clash rejection), or a file
//  whose path can't be a canonical virtual path. All violations are
//  collected into one error message — a cook never half-succeeds silently.
// ============================================================================

#include "Asset.hpp"

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/visibility.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace lux::asset
{
    struct PakCookResult
    {
        std::size_t    asset_count{ 0 };
        std::uintmax_t payload_bytes{ 0 };
    };

    /// Cook every .luxasset/.luxmodel under @p content_dir into @p out_pak
    /// (verbatim byte copies, uuid-sorted, deterministic). @p mount_hint is
    /// recorded advisory-only ("/Game"); the runtime decides the actual root
    /// at mount time.
    [[nodiscard]] LUX_RESOURCE_PUBLIC
    lux::cxx::expected<PakCookResult, std::string>
    cookDirectoryToPak(const std::filesystem::path& content_dir,
                       const std::filesystem::path& out_pak,
                       std::string_view             mount_hint = "/Game");

    /// One row of inspectPak output (public-friendly mirror of the codec's
    /// entry + its PATH row, joined).
    struct PakInspectEntry
    {
        asset_id_t    id{};
        EAssetType    type{ EAssetType::UNKNOWN };
        std::string   vpath;            ///< Mount-relative; empty if none.
        std::uint64_t offset{ 0 };
        std::uint64_t size{ 0 };
        std::uint8_t  compression{ 0 };
        bool          tombstone{ false };
        std::uint64_t content_hash{ 0 };
    };

    struct PakInspectInfo
    {
        std::string                  mount_hint;
        std::vector<PakInspectEntry> entries;   ///< uuid-ascending.
    };

    /// Read + validate a pak's index for display (lux_asset_packer
    /// `pak-inspect`). Never touches payload bytes.
    [[nodiscard]] LUX_RESOURCE_PUBLIC
    lux::cxx::expected<PakInspectInfo, std::string>
    inspectPak(const std::filesystem::path& pak_path);

} // namespace lux::asset
