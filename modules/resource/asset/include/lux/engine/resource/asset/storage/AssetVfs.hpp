#pragma once
// ============================================================================
// AssetVfs is the explicit, low-frequency mount control plane. AssetVfsView
// is the copyable read capability published to runtime and Editor consumers.
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
    DUPLICATE_MOUNT,
    CAPACITY
};

namespace detail
{
struct AssetVfsState;
}

class LUX_ASSET_PUBLIC AssetVfsView final
{
  public:
    AssetVfsView() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept;
    // Retains the current mount table. Later mount publications are invisible.
    // Providers still define content immutability (e.g. a versioned LUXPAK).
    [[nodiscard]] AssetVfsView capture() const;
    [[nodiscard]] AssetId resolve(std::string_view vpath) const;
    [[nodiscard]] lux::cxx::expected<AssetBlob, EAssetStorageError> open(AssetId id) const;
    void enumerate(const std::function<void(const ProviderEntry &)> &fn) const;
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

    AssetVfs(const AssetVfs &) = delete;
    AssetVfs &operator=(const AssetVfs &) = delete;
    AssetVfs(AssetVfs &&) = delete;
    AssetVfs &operator=(AssetVfs &&) = delete;

    [[nodiscard]] MountId mount(MountDesc desc);
    void unmount(MountId id);

    // One control-plane publication. Failure leaves the table and descriptors unchanged;
    // readers already inside a provider retain the previous table until their read completes.
    [[nodiscard]] lux::cxx::expected<std::vector<MountId>, EMountUpdateError> replaceMounts(
        std::span<const MountId> removed, std::span<const MountDesc> added);

    [[nodiscard]] AssetVfsView view() const noexcept;
    [[nodiscard]] std::size_t mountCount() const noexcept;

  private:
    std::shared_ptr<detail::AssetVfsState> state_;
};
} // namespace lux::asset
