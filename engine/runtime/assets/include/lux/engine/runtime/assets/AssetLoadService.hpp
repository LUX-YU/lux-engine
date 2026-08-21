#pragma once
/**
 * @file AssetLoadService.hpp
 * @brief Process-domain asset loading orchestrated by AsyncRuntime.
 */

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

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
        std::uint32_t          revision{0};
        bool                   already_ready{false};
        std::shared_ptr<const lux::asset::AssetVfs> vfs;
    };

    struct AssetLoadBatchResult final
    {
        std::vector<AssetLoadResult> assets;
    };

    /// Dynamic dependency join. The caller snapshots every LoadAsset on the
    /// main thread before submission; the coordinator then joins the existing
    /// per-id rows without reading AssetManager from a worker thread.
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

    class AssetClient final
    {
    public:
        AssetClient() noexcept = default;

        /// Main-thread convenience used by ECS resolver callbacks. It only
        /// submits an ensure intent; readiness remains authoritative in
        /// AssetManager and is observed at the next safe point.
        [[nodiscard]] lux::exec::AsyncSubmitResult request(
            const lux::asset::asset_id_t& id) const noexcept;

        /// Clear terminal/backoff memory after content replacement. The
        /// manager revision is captured on the main thread as the ABA guard.
        [[nodiscard]] lux::exec::AsyncSubmitResult invalidate(
            const lux::asset::asset_id_t& id) const noexcept;

        [[nodiscard]] LoadAsset
        loadOperation(const lux::asset::asset_id_t& id) const noexcept;

        /// Main-thread snapshot for a dynamically-sized dependency set.
        /// Duplicate and nil ids are removed while preserving first-seen
        /// order. The returned operation owns every payload it needs.
        [[nodiscard]] LoadAssetBatch loadBatchOperation(
            std::span<const lux::asset::asset_id_t> ids) const;

        [[nodiscard]] const lux::exec::AsyncOperationClient<LoadAsset>&
        loadClient() const noexcept
        {
            return load_;
        }

        [[nodiscard]] const lux::exec::AsyncOperationClient<LoadAssetBatch>&
        loadBatchClient() const noexcept
        {
            return load_batch_;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return manager_ != nullptr && static_cast<bool>(ensure_);
        }

    private:
        friend class AssetLoadService;

        AssetClient(
            lux::asset::AssetManager& manager,
            lux::exec::AsyncOperationClient<EnsureAssetLoaded> ensure,
            lux::exec::AsyncOperationClient<LoadAsset> load,
            lux::exec::AsyncOperationClient<LoadAssetBatch> load_batch,
            lux::exec::AsyncOperationClient<InvalidateAssetLoad> invalidate)
            noexcept
            : manager_(&manager)
            , ensure_(std::move(ensure))
            , load_(std::move(load))
            , load_batch_(std::move(load_batch))
            , invalidate_(std::move(invalidate))
        {}

        lux::asset::AssetManager* manager_{nullptr};
        lux::exec::AsyncOperationClient<EnsureAssetLoaded> ensure_;
        lux::exec::AsyncOperationClient<LoadAsset> load_;
        lux::exec::AsyncOperationClient<LoadAssetBatch> load_batch_;
        lux::exec::AsyncOperationClient<InvalidateAssetLoad> invalidate_;
    };

    class AssetLoadService final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            AssetLoadService,
            lux::exec::AsyncAssemblyFailure>
        addTo(
            lux::exec::AsyncRuntimeBuilder& builder,
            lux::asset::AssetManager& manager);

        AssetLoadService(const AssetLoadService&) = delete;
        AssetLoadService& operator=(const AssetLoadService&) = delete;
        AssetLoadService(AssetLoadService&& other) noexcept;
        AssetLoadService& operator=(AssetLoadService&& other) noexcept;
        ~AssetLoadService();

        [[nodiscard]] AssetClient client() const noexcept;

        /// Prevent new work and invalidate the service-facing client. Runtime
        /// scopes still own accepted operations and settle them exactly once.
        void close() noexcept;

    private:
        struct State;

        AssetLoadService(
            std::shared_ptr<State> state,
            lux::asset::AssetManager& manager,
            lux::exec::AsyncOperationClient<EnsureAssetLoaded> ensure,
            lux::exec::AsyncOperationClient<LoadAsset> load,
            lux::exec::AsyncOperationClient<LoadAssetBatch> load_batch,
            lux::exec::AsyncOperationClient<InvalidateAssetLoad> invalidate)
            noexcept
            : state_(std::move(state))
            , manager_(&manager)
            , ensure_(std::move(ensure))
            , load_(std::move(load))
            , load_batch_(std::move(load_batch))
            , invalidate_(std::move(invalidate))
        {}

        std::shared_ptr<State> state_;
        lux::asset::AssetManager* manager_{nullptr};
        lux::exec::AsyncOperationClient<EnsureAssetLoaded> ensure_;
        lux::exec::AsyncOperationClient<LoadAsset> load_;
        lux::exec::AsyncOperationClient<LoadAssetBatch> load_batch_;
        lux::exec::AsyncOperationClient<InvalidateAssetLoad> invalidate_;
        bool closed_{false};
    };
}
