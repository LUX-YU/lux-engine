#pragma once
// ============================================================================
// AssetVfs publishes mounts; MountLease owns their registration. AssetVfsView
// is a copyable read capability for consumers that do not own the mount table.
// Each read retains one immutable mount-table snapshot for the entire provider
// call, so mount publication cannot invalidate readers or provider lifetimes.
// ============================================================================

#include <lux/engine/resource/asset/storage/AssetProvider.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::asset
{
    using MountId = std::uint32_t;
    inline constexpr MountId kInvalidMountId = 0;

    struct MountDesc final
    {
        std::string root;
        std::shared_ptr<IAssetProvider> provider;
        int priority{};
    };

    enum class EMountUpdateError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        UNKNOWN_MOUNT,
        CAPACITY
    };

    namespace detail
    {
        struct AssetVfsState;
    }

    // Owns publication, not the AssetVfs object. A lease may outlive that object;
    // its last release still revokes the mount in live views of the same table.
    // Captured views and reads already inside a provider keep their own snapshot.
    // Access to the same lease requires caller synchronization.
    class LUX_ASSET_PUBLIC MountLease final
    {
    public:
        MountLease() noexcept = default;
        ~MountLease() noexcept;
        MountLease(MountLease&& other) noexcept;
        MountLease& operator=(MountLease&& other) noexcept;
        MountLease(const MountLease&) = delete;
        MountLease& operator=(const MountLease&) = delete;

        [[nodiscard]] MountId id() const noexcept
        {
            return id_;
        }

    private:
        friend class AssetVfs;

        MountLease(std::shared_ptr<detail::AssetVfsState> state, MountId id) noexcept;

        std::shared_ptr<detail::AssetVfsState> state_;
        MountId id_{};
    };

    using MountResult = lux::cxx::expected<MountLease, EMountUpdateError>;
    using MountBatchResult = lux::cxx::expected<std::vector<MountLease>, EMountUpdateError>;

    class LUX_ASSET_PUBLIC AssetVfsView final
    {
    public:
        AssetVfsView() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept;
        // Retains the current mount table. Later mount publications are invisible.
        // Providers still define content immutability (e.g. a versioned LUXPAK).
        [[nodiscard]] AssetVfsView capture() const;
        [[nodiscard]] AssetId resolve(std::string_view vpath) const;
        [[nodiscard]] lux::cxx::expected<AssetBlob, EAssetStorageError> open(
            AssetId id,
            std::size_t max_bytes = SIZE_MAX
        ) const;
        void enumerate(const std::function<void(const ProviderEntry&)>& fn) const;
        [[nodiscard]] std::optional<std::string> pathOf(AssetId id) const;

    private:
        friend class AssetVfs;

        explicit AssetVfsView(std::shared_ptr<detail::AssetVfsState> state) noexcept;

        std::shared_ptr<detail::AssetVfsState> state_;
    };

    class LUX_ASSET_PUBLIC AssetVfs final
    {
    public:
        AssetVfs();
        ~AssetVfs();

        AssetVfs(const AssetVfs&) = delete;
        AssetVfs& operator=(const AssetVfs&) = delete;
        AssetVfs(AssetVfs&&) = delete;
        AssetVfs& operator=(AssetVfs&&) = delete;

        [[nodiscard]] MountResult mount(MountDesc desc) noexcept;

        // One atomic table publication, including when removing all mounts. Success consumes
        // removed leases; failure preserves them and the table. Each added mount has one owner.
        [[nodiscard]] MountBatchResult replaceMounts(
            std::span<MountLease> removed,
            std::span<const MountDesc> added
        ) noexcept;

        [[nodiscard]] AssetId resolve(std::string_view vpath) const;
        [[nodiscard]] lux::cxx::expected<AssetBlob, EAssetStorageError> open(
            AssetId id,
            std::size_t max_bytes = SIZE_MAX
        ) const;
        void enumerate(const std::function<void(const ProviderEntry&)>& fn) const;
        [[nodiscard]] std::optional<std::string> pathOf(AssetId id) const;

        [[nodiscard]] AssetVfsView view() const noexcept;
        [[nodiscard]] std::size_t mountCount() const noexcept;

    private:
        std::shared_ptr<detail::AssetVfsState> state_;
    };
} // namespace lux::asset
