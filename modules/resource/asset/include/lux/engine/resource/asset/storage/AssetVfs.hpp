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
#include <string>
#include <string_view>

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

    namespace detail
    {
        struct AssetVfsState;
    }

    class LUX_ASSET_PUBLIC AssetVfsView final
    {
    public:
        AssetVfsView() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] AssetId resolve(std::string_view vpath) const;
        [[nodiscard]] lux::cxx::expected<AssetBlob, EAssetStorageError> open(AssetId id) const;
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

        [[nodiscard]] MountId mount(MountDesc desc);
        void unmount(MountId id);

        [[nodiscard]] AssetVfsView view() const noexcept;
        [[nodiscard]] std::size_t mountCount() const noexcept;

    private:
        std::shared_ptr<detail::AssetVfsState> state_;
    };
} // namespace lux::asset
