#pragma once
// ============================================================================
//  AssetVfs — mount/override semantics for opaque provider records.
//
//  Deliberately narrowed to TWO verbs (resolve: path->id, open: id->bytes)
//  plus registry-style enumeration — NOT a general virtual file IO layer.
//  The same AssetVfs code runs in the editor (an Authoring loose provider over
//  content folder) and in shipped builds (PakAssetProvider over .luxpak),
//  which is what makes editor/runtime path behavior byte-identical.
//
//  Identity model: the UUID is the only hard reference; a virtual path is a
//  soft, human-facing address that RESOLVES to a uuid. open() never takes a
//  path. Design: .internal/plan/virtual-path-pak-design.md §3.
// ============================================================================

#include <lux/engine/resource/asset/storage/AssetProvider.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lux::asset
{
    using MountId = std::uint32_t;
    inline constexpr MountId kInvalidMountId = 0;

    struct MountDesc
    {
        std::string root; ///< Canonical, e.g. "/Game".
        std::shared_ptr<IAssetProvider> provider;
        int priority{0}; ///< Higher wins.
    };

    /// The single upward-facing API. Mount order semantics (UE-style):
    /// scanned by (priority desc, recency desc) — equal priority means the
    /// NEWEST mount wins, so a patch pak mounted later shadows the base pak
    /// without priority bookkeeping.
    class LUX_ASSET_PUBLIC AssetVfs
    {
    public:
        /// Mount a provider under a root. The root must itself parse as one
        /// legal path segment prefixed with '/' ("/Game"); an illegal root
        /// is rejected with kInvalidMountId (diagnostic-friendly: catches
        /// "/Gmae"-class typos at mount time, the only place roots enter).
        MountId mount(MountDesc desc);

        void unmount(MountId id);

        /// Absolute canonical vpath -> uuid. Nil uuid when the path is
        /// illegal, the root is unmounted, or no provider knows it.
        [[nodiscard]] AssetId resolve(std::string_view vpath) const;

        /// uuid -> full .luxasset image from the WINNING mount: first
        /// provider (in mount order) whose contains() is true. A tombstone
        /// wins the claim and then fails the open — that IS shadow-delete.
        [[nodiscard]] lux::cxx::expected<AssetBlob, EAssetStorageError> open(const AssetId& id) const;

        /// Shadow-aware enumeration: each id/path appears once, from its
        /// winning mount, with the ABSOLUTE vpath; tombstoned ids appear
        /// not at all (but still consume their id claim).
        void enumerate(const std::function<void(const ProviderEntry&)>& fn) const;

        /// Diagnostic reverse lookup: absolute vpath of the winning mount's
        /// entry for this id, if any.
        [[nodiscard]] std::optional<std::string> pathOf(const AssetId& id) const;

        [[nodiscard]] std::size_t mountCount() const noexcept
        {
            return mounts_.size();
        }

    private:
        struct Mount
        {
            MountId id;
            std::string root; ///< "/Game"
            std::shared_ptr<IAssetProvider> provider;
            int priority;
            std::uint64_t seq; ///< Mount recency.
        };

        /// Sorted: priority desc, then seq desc (newest first).
        std::vector<Mount> mounts_;
        MountId next_id_{1};
        std::uint64_t next_seq_{1};
    };

} // namespace lux::asset
