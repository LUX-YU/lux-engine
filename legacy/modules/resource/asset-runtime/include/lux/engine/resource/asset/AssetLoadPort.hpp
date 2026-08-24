#pragma once
/** @file AssetLoadPort.hpp @brief ECS-free typed asset-loading operation port. */

#include <lux/engine/resource/asset/visibility.h>
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/core/async/OperationPort.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace lux::asset { class AssetManager; }

namespace lux::asset_runtime
{
    struct AssetLoadResult final
    {
        lux::asset::asset_id_t id{};
        std::uint32_t revision{0};
    };

    struct EnsureAssetLoaded final
    {
        using Value = void;
        using Error = lux::asset::EAssetError;

        lux::asset::asset_id_t id{};
        std::uint32_t revision{0};
        bool already_ready{false};
        std::shared_ptr<const lux::asset::AssetVfs> vfs;
    };

    struct LoadAsset final
    {
        using Value = AssetLoadResult;
        using Error = lux::asset::EAssetError;

        lux::asset::asset_id_t id{};
        std::uint32_t revision{0};
        bool already_ready{false};
        std::shared_ptr<const lux::asset::AssetVfs> vfs;
    };

    struct AssetLoadBatchResult final
    {
        std::vector<AssetLoadResult> assets;
    };

    struct LoadAssetBatch final
    {
        using Value = AssetLoadBatchResult;
        using Error = lux::asset::EAssetError;

        std::shared_ptr<const std::vector<LoadAsset>> assets;
    };

    struct InvalidateAssetLoad final
    {
        using Value = void;
        using Error = lux::asset::EAssetError;

        lux::asset::asset_id_t id{};
        std::uint32_t revision{0};
    };

    class AssetLoadService;

    /// Narrow typed port used by ECS asset-resolution Systems. It contains no
    /// scheduler, queue, registry, builder or service lookup.
    class LUX_ASSET_PUBLIC AssetClient final
    {
    public:
        AssetClient() noexcept = default;

        [[nodiscard]] lux::async::SubmitResult request(
            const lux::asset::asset_id_t& id) const noexcept;
        [[nodiscard]] lux::async::SubmitResult invalidate(
            const lux::asset::asset_id_t& id) const noexcept;
        [[nodiscard]] LoadAsset loadOperation(
            const lux::asset::asset_id_t& id) const noexcept;
        [[nodiscard]] LoadAssetBatch loadBatchOperation(
            std::span<const lux::asset::asset_id_t> ids) const;

        [[nodiscard]] const lux::async::OperationPort<LoadAsset>&
        loadClient() const noexcept { return load_; }
        [[nodiscard]] const lux::async::OperationPort<LoadAssetBatch>&
        loadBatchClient() const noexcept { return load_batch_; }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return manager_ != nullptr && static_cast<bool>(ensure_);
        }

    private:
        friend class AssetLoadService;

        AssetClient(
            lux::asset::AssetManager& manager,
            lux::async::OperationPort<EnsureAssetLoaded> ensure,
            lux::async::OperationPort<LoadAsset> load,
            lux::async::OperationPort<LoadAssetBatch> load_batch,
            lux::async::OperationPort<InvalidateAssetLoad> invalidate)
            noexcept
            : manager_(&manager)
            , ensure_(std::move(ensure))
            , load_(std::move(load))
            , load_batch_(std::move(load_batch))
            , invalidate_(std::move(invalidate))
        {}

        lux::asset::AssetManager* manager_{nullptr};
        lux::async::OperationPort<EnsureAssetLoaded> ensure_;
        lux::async::OperationPort<LoadAsset> load_;
        lux::async::OperationPort<LoadAssetBatch> load_batch_;
        lux::async::OperationPort<InvalidateAssetLoad> invalidate_;
    };
}
