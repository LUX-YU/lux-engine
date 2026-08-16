#pragma once
// ============================================================================
//  AssetVfs / IAssetProvider — the asset-only virtual filesystem seam.
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

#include "Asset.hpp"

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/visibility.h>
#include <lux/cxx/memory/SharedBytes.hpp>

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
    /// Ownership-erased bytes of a complete .luxasset file image (header +
    /// info + data + payload tail). The shared_ptr erasure is the v2 seam:
    /// a future mmap'd pak view plugs in with zero interface change. Callers
    /// MUST NOT assume the blob is small (a 2GB texture arrives whole).
    struct AssetBlob
    {
        lux::cxx::SharedBytes<> bytes;

        [[nodiscard]] static AssetBlob fromSharedArray(
            std::shared_ptr<const std::byte[]> owner,
            std::size_t size) noexcept
        {
            if (!owner || size == 0u)
                return {};
            const auto* data = owner.get();
            std::shared_ptr<const void> erased{owner, data};
            auto shared = lux::cxx::SharedBytes<>::fromOwner(
                std::move(erased),
                std::span<const std::byte>{data, size});
            return shared.empty() ? AssetBlob{} : AssetBlob{std::move(shared)};
        }

        [[nodiscard]] static AssetBlob fromShared(
            lux::cxx::SharedBytes<> value) noexcept
        {
            return AssetBlob{std::move(value)};
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return !bytes.empty();
        }
    };

    /// One enumerable asset of a provider. `vpath` is MOUNT-RELATIVE
    /// canonical form ("Materials/M_Box"); AssetVfs::enumerate re-emits it
    /// absolute. Tombstones (patch-deleted assets) ARE emitted here with
    /// `tombstone=true` and an empty vpath so the VFS can shadow lower
    /// mounts — they never reach user-facing enumeration.
    struct ProviderEntry
    {
        asset_id_t  id{};
        EAssetType  type{ EAssetType::UNKNOWN };
        std::string vpath;
        bool        tombstone{ false };
    };

    /// One mount source (loose content dir, pak file, in-memory builtins).
    /// Contract:
    ///  * resolve/contains/open/pathOf are pure lookups — no lazy IO besides
    ///    open() reading bytes; never touch host-filesystem case semantics.
    ///  * resolve takes the mount-relative canonical path, byte-sensitive.
    ///  * Tombstone entries: contains()==true, open()==ASSET_NOT_EXIST-class
    ///    error, excluded from resolve()/pathOf(), emitted by enumerate()
    ///    with tombstone=true. This is the shadow-delete contract — v1
    ///    readers MUST honor it even though v1 writers never produce one.
    class LUX_RESOURCE_PUBLIC IAssetProvider
    {
    public:
        virtual ~IAssetProvider() = default;

        [[nodiscard]] virtual std::optional<asset_id_t>
        resolve(std::string_view rel_vpath) const = 0;

        [[nodiscard]] virtual bool contains(const asset_id_t& id) const = 0;

        [[nodiscard]] virtual lux::cxx::expected<AssetBlob, EAssetError>
        open(const asset_id_t& id) const = 0;

        virtual void enumerate(
            const std::function<void(const ProviderEntry&)>& fn) const = 0;

        [[nodiscard]] virtual std::optional<std::string>
        pathOf(const asset_id_t& id) const = 0;
    };

    // ── Boot-scene resolution ─────────────────────────────────────────────
    enum class EBootSceneError : std::uint8_t
    {
        VPathNotFound,    ///< The explicit vpath resolves to nothing.
        EntryTypeMismatch,///< Explicit vpath is not an ENTITY_SCENE.
        NoSceneEntry,     ///< The provider carries no ENTITY_SCENE entry.
        MultipleScenes,   ///< >1 scene entries and no vpath to disambiguate.
    };

    struct BootSceneResolve
    {
        asset_id_t  id{};
        std::string vpath;          ///< The resolved entry's vpath (origin tag).
        std::size_t scene_count{};  ///< ENTITY_SCENE entries seen.
    };

    /// THE boot-scene rule, one copy for both players (desktop lux_player +
    /// the Android game shell): an explicit @p vpath wins; otherwise the
    /// provider's single ENTITY_SCENE entry boots — zero or several is an error
    /// demanding an explicit pick. Error TEXT stays with the host
    /// (diagnostics belong to the host; this returns the facts).
    [[nodiscard]] LUX_RESOURCE_PUBLIC
    lux::cxx::expected<BootSceneResolve, EBootSceneError>
    resolveBootScene(const IAssetProvider& provider, std::string_view vpath);

    using MountId = std::uint32_t;
    inline constexpr MountId kInvalidMountId = 0;

    struct MountDesc
    {
        std::string                     root;       ///< Canonical, e.g. "/Game".
        std::shared_ptr<IAssetProvider> provider;
        int                             priority{ 0 }; ///< Higher wins.
    };

    /// The single upward-facing API. Mount order semantics (UE-style):
    /// scanned by (priority desc, recency desc) — equal priority means the
    /// NEWEST mount wins, so a patch pak mounted later shadows the base pak
    /// without priority bookkeeping.
    class LUX_RESOURCE_PUBLIC AssetVfs
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
        [[nodiscard]] asset_id_t resolve(std::string_view vpath) const;

        /// uuid -> full .luxasset image from the WINNING mount: first
        /// provider (in mount order) whose contains() is true. A tombstone
        /// wins the claim and then fails the open — that IS shadow-delete.
        [[nodiscard]] lux::cxx::expected<AssetBlob, EAssetError>
        open(const asset_id_t& id) const;

        /// Shadow-aware enumeration: each id/path appears once, from its
        /// winning mount, with the ABSOLUTE vpath; tombstoned ids appear
        /// not at all (but still consume their id claim).
        void enumerate(const std::function<void(const ProviderEntry&)>& fn) const;

        /// Diagnostic reverse lookup: absolute vpath of the winning mount's
        /// entry for this id, if any.
        [[nodiscard]] std::optional<std::string> pathOf(const asset_id_t& id) const;

        [[nodiscard]] std::size_t mountCount() const noexcept { return mounts_.size(); }

    private:
        struct Mount
        {
            MountId                         id;
            std::string                     root;   ///< "/Game"
            std::shared_ptr<IAssetProvider> provider;
            int                             priority;
            std::uint64_t                   seq;    ///< Mount recency.
        };

        /// Sorted: priority desc, then seq desc (newest first).
        std::vector<Mount> mounts_;
        MountId            next_id_{ 1 };
        std::uint64_t      next_seq_{ 1 };
    };

} // namespace lux::asset
