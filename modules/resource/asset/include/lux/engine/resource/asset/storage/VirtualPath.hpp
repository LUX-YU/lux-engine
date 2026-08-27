#pragma once
// ============================================================================
//  VirtualPath — THE single parser/validator for asset virtual paths.
//
//      /Game/Materials/M_Box      /Engine/Meshes/SM_Cube
//
//  Shared verbatim by the editor index, the cook pipeline and the runtime
//  VFS — divergent leniency between those three is an identity-bug factory,
//  so convenience normalization (backslash fixup, trimming, case-tolerant
//  completion) lives in editor UI code ONLY, never here.
//
//  Grammar (design: .internal/plan/virtual-path-pak-design.md §2):
//      asset-path := "/" root ("/" segment)* "/" asset-name
//      segment    := name-char+        ; no extension, no dots, no colons
//      name-char  := any byte except / \ . : " | ? * < > and control chars
//
//  MUST-NOT-CHANGE: these rules are written into cooked pak PATH sections.
//  Loosening or tightening them invalidates every shipped pak (same class
//  of frozen constant as kEngineAssetNamespace). Comparison is BYTE-LEVEL
//  case-sensitive (ruling §11.1); case-insensitive *collisions* are rejected
//  at cook/scan time instead of folding at resolve time.
// ============================================================================

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace lux::asset
{
    enum class EVirtualPathError
    {
        EMPTY,             ///< Empty input.
        TOO_LONG,          ///< Exceeds kMaxLength bytes.
        NOT_ABSOLUTE,      ///< Canonical form must start with '/'.
        ILLEGAL_CHARACTER, ///< Contains \ . : " | ? * < > or a control char.
        EMPTY_SEGMENT,     ///< "//", trailing '/', or a bare root with no name.
        MISSING_ASSET_NAME ///< Only a root segment ("/Game").
    };

    /// Parsed, validated, canonical virtual path. Immutable; the only way to
    /// obtain one is parse(), so holding a VirtualPath IS the validity proof.
    class LUX_ASSET_PUBLIC VirtualPath
    {
    public:
        /// Hard byte cap, part of the on-disk contract (pak PATH section
        /// validates against the same constant).
        static constexpr std::size_t kMaxLength = 1024;

        /// Parse a canonical ABSOLUTE path ("/Game/Materials/M_Box").
        /// No normalization: a string is either canonical or rejected.
        [[nodiscard]] static lux::cxx::expected<VirtualPath, EVirtualPathError> parse(std::string_view text);

        /// Validate a mount-RELATIVE canonical path ("Materials/M_Box") —
        /// what IAssetProvider::resolve receives and what providers index.
        /// Same charset/segment rules, no leading slash, root not included.
        /// Returns nullopt when valid, the error otherwise.
        [[nodiscard]] static std::optional<EVirtualPathError> validateRelative(std::string_view rel) noexcept;

        /// Byte legality test for a single name character ('/' is structure,
        /// not a name char). UTF-8 continuation bytes (>= 0x80) pass.
        [[nodiscard]] static bool isLegalChar(char c) noexcept;

        /// Mount-root legality: '/' + exactly one legal segment ("/Game").
        /// Shared by AssetVfs::mount and the pak codec's mount_hint check.
        [[nodiscard]] static bool isLegalRoot(std::string_view root) noexcept;

        /// Canonical absolute form, e.g. "/Game/Materials/M_Box".
        [[nodiscard]] const std::string& str() const noexcept
        {
            return path_;
        }

        /// Root segment without slashes, e.g. "Game".
        [[nodiscard]] std::string_view root() const noexcept
        {
            return std::string_view{path_}.substr(1, root_len_);
        }

        /// Mount-relative remainder, e.g. "Materials/M_Box".
        [[nodiscard]] std::string_view relPath() const noexcept
        {
            return std::string_view{path_}.substr(root_len_ + 2);
        }

        /// Final segment (the asset name), e.g. "M_Box".
        [[nodiscard]] std::string_view name() const noexcept
        {
            return std::string_view{path_}.substr(name_pos_);
        }

        /// Byte-sensitive equality — THE comparison semantics (ruling §11.1).
        [[nodiscard]] bool operator==(const VirtualPath& rhs) const noexcept
        {
            return path_ == rhs.path_;
        }

    private:
        VirtualPath() = default;

        std::string path_;       ///< Full canonical form.
        std::size_t root_len_{}; ///< Byte length of the root segment.
        std::size_t name_pos_{}; ///< Offset of the asset-name segment.
    };

    /// ASCII-only case folding for collision DIAGNOSTICS (cook/scan reject
    /// case-insensitive clashes; editors warn). Deliberately NOT a resolution
    /// key — resolve is byte-sensitive, and keeping folding out of lookup
    /// means no folding algorithm ever becomes a frozen on-disk contract.
    [[nodiscard]] LUX_ASSET_PUBLIC std::string foldCaseAscii(std::string_view s);

} // namespace lux::asset
