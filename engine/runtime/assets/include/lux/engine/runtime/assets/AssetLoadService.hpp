#pragma once
/**
 * @file AssetLoadService.hpp
 * @brief Process-domain asset loading orchestrated by AsyncRuntime.
 */

#include <lux/engine/resource/asset/AssetLoadPort.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>

#include <memory>

namespace lux::asset_runtime
{
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
            lux::async::OperationPort<EnsureAssetLoaded> ensure,
            lux::async::OperationPort<LoadAsset> load,
            lux::async::OperationPort<LoadAssetBatch> load_batch,
            lux::async::OperationPort<InvalidateAssetLoad> invalidate)
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
        lux::async::OperationPort<EnsureAssetLoaded> ensure_;
        lux::async::OperationPort<LoadAsset> load_;
        lux::async::OperationPort<LoadAssetBatch> load_batch_;
        lux::async::OperationPort<InvalidateAssetLoad> invalidate_;
        bool closed_{false};
    };
}
