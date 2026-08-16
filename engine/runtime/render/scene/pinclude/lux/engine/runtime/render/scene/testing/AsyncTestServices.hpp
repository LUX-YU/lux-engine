#pragma once
/**
 * @file AsyncTestServices.hpp
 * @brief Build-tree-only RAII composition root for render/residency tests.
 *
 * Tests used to instantiate the retired asset-coupled executor directly and therefore exercised a
 * product path that no host used. This rig assembles the same AssetLoadService,
 * AsyncRenderUploadService and AsyncRuntime graph as Editor/GameHost/Android.
 */

#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/detail/MainThreadMailbox.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <lux/engine/runtime/render/scene/AsyncRenderUploadService.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>
#include <lux/engine/runtime/scene/SceneRuntime.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <atomic>

namespace lux::runtime::testing
{
    namespace detail
    {
        class TestCloseEpoch final
        {
        public:
            explicit TestCloseEpoch(lux::exec::AsyncRuntime& runtime)
                : runtime_(runtime)
                , epoch_(std::make_shared<std::atomic<std::uint64_t>>(0u))
                , binding_(std::make_shared<
                    const lux::exec::MainThreadMailbox::WakeBinding>(
                        lux::exec::MainThreadMailbox::WakeBinding{
                            epoch_,
                            [](void* opaque) noexcept
                            {
                                auto& value = *static_cast<
                                    std::atomic<std::uint64_t>*>(opaque);
                                value.fetch_add(
                                    1u, std::memory_order_release);
                                value.notify_one();
                            }}))
            {
                runtime_.mainThreadMailbox().bindExternalWake(binding_);
            }

            ~TestCloseEpoch()
            {
                runtime_.mainThreadMailbox().unbindExternalWake(binding_);
            }

            void notify() noexcept
            {
                epoch_->fetch_add(1u, std::memory_order_release);
                epoch_->notify_one();
            }

            template <class Predicate>
            void drive(Predicate&& done) noexcept
            {
                while (!done())
                {
                    const auto observed = epoch_->load(
                        std::memory_order_acquire);
                    const auto pumped =
                        runtime_.drainMainThreadCompletions(256u);
                    if (!done())
                    {
                        if (pumped != 0u)
                            continue;
                        epoch_->wait(observed, std::memory_order_acquire);
                    }
                }
            }

        private:
            lux::exec::AsyncRuntime& runtime_;
            std::shared_ptr<std::atomic<std::uint64_t>> epoch_;
            std::shared_ptr<const lux::exec::MainThreadMailbox::WakeBinding> binding_;
        };

        inline void closeRuntime(lux::exec::AsyncRuntime& runtime) noexcept
        {
            TestCloseEpoch progress{runtime};
            std::atomic<bool> closed{false};
            lux::exec::detail::subscribeRuntimeClose(
                runtime,
                [&closed, &progress](lux::exec::AsyncCloseReport) noexcept
                {
                    closed.store(true, std::memory_order_release);
                    progress.notify();
                });
            progress.drive(
                [&closed]() noexcept
                {
                    return closed.load(std::memory_order_acquire);
                });
            (void)runtime.join();
        }

        inline void closeUpload(
            AsyncRenderUploadService& upload,
            lux::exec::AsyncRuntime& runtime) noexcept
        {
            TestCloseEpoch progress{runtime};
            std::atomic<bool> closed{false};
            lux::runtime::detail::subscribeRenderUploadClose(
                upload,
                [&closed, &progress](AsyncRenderUploadCloseReport) noexcept
                {
                    closed.store(true, std::memory_order_release);
                    progress.notify();
                });
            progress.drive(
                [&closed]() noexcept
                {
                    return closed.load(std::memory_order_acquire);
                });
        }

        [[nodiscard]] inline SceneCloseReport closeScene(
            SceneRuntime& scene,
            lux::exec::AsyncRuntime& runtime) noexcept
        {
            TestCloseEpoch progress{runtime};
            std::atomic<bool> closed{false};
            SceneCloseReport report{};
            lux::runtime::detail::subscribeSceneClose(
                scene,
                [&closed, &progress, &report](SceneCloseReport value) noexcept
                {
                    report = value;
                    closed.store(true, std::memory_order_release);
                    progress.notify();
                });
            progress.drive(
                [&closed]() noexcept
                {
                    return closed.load(std::memory_order_acquire);
                });
            return report;
        }

        [[nodiscard]] inline ResidencyCloseReport closeResidency(
            ResidencyAssembly& assembly,
            lux::exec::AsyncRuntime& runtime) noexcept
        {
            TestCloseEpoch progress{runtime};
            std::atomic<bool> closed{false};
            ResidencyCloseReport report{};
            lux::runtime::detail::subscribeResidencyClose(
                assembly,
                [&closed, &progress, &report](ResidencyCloseReport value)
                    noexcept
                {
                    report = value;
                    closed.store(true, std::memory_order_release);
                    progress.notify();
                });
            progress.drive(
                [&closed]() noexcept
                {
                    return closed.load(std::memory_order_acquire);
                });
            return report;
        }
    }

    class AssetAsyncTestServices final
    {
    public:
        explicit AssetAsyncTestServices(
            lux::asset::AssetManager& assets,
            lux::exec::AsyncRuntimeConfig config =
                lux::exec::AsyncRuntimeConfig{
                    .blocking_io_threads = 1,
                    .background_cpu_concurrency = 2})
        {
            lux::exec::AsyncRuntimeBuilder builder;
            auto asset_load = lux::asset_runtime::AssetLoadService::addTo(
                builder,
                assets
            );
            if (!asset_load)
                return;
            auto plan = std::move(builder).compile();
            if (!plan)
                return;
            runtime_ = std::make_unique<lux::exec::AsyncRuntime>(
                std::move(*plan),
                config
            );
            asset_load_ = std::make_unique<
                lux::asset_runtime::AssetLoadService>(
                    std::move(*asset_load)
                );
            valid_ = true;
        }

        ~AssetAsyncTestServices() { close(); }

        AssetAsyncTestServices(const AssetAsyncTestServices&) = delete;
        AssetAsyncTestServices& operator=(const AssetAsyncTestServices&) = delete;
        AssetAsyncTestServices(AssetAsyncTestServices&&) = delete;
        AssetAsyncTestServices& operator=(AssetAsyncTestServices&&) = delete;

        [[nodiscard]] bool valid() const noexcept { return valid_; }
        [[nodiscard]] lux::asset_runtime::AssetClient client() const noexcept
        {
            return asset_load_ ? asset_load_->client()
                               : lux::asset_runtime::AssetClient{};
        }
        [[nodiscard]] lux::exec::AsyncRuntime& runtime() noexcept
        {
            return *runtime_;
        }
        std::size_t drainMainThreadCompletions(std::size_t budget = 256u)
        {
            return runtime_ ? runtime_->drainMainThreadCompletions(budget) : 0u;
        }

        void close() noexcept
        {
            if (closed_)
                return;
            closed_ = true;
            if (asset_load_)
                asset_load_->close();
            if (runtime_)
                detail::closeRuntime(*runtime_);
            valid_ = false;
        }

    private:
        std::unique_ptr<lux::exec::AsyncRuntime> runtime_;
        std::unique_ptr<lux::asset_runtime::AssetLoadService> asset_load_;
        bool valid_{false};
        bool closed_{false};
    };

    class AsyncTestServices final
    {
    public:
        AsyncTestServices(
            lux::asset::AssetManager& assets,
            lux::render::RenderUploadSession& upload,
            std::shared_ptr<lux::render::RenderChannelSync> sync,
            lux::exec::AsyncRuntimeConfig config =
                lux::exec::AsyncRuntimeConfig{
                    .blocking_io_threads = 1,
                    .background_cpu_concurrency = 2})
        {
            lux::exec::AsyncRuntimeBuilder builder;
            auto asset_load = lux::asset_runtime::AssetLoadService::addTo(
                builder,
                assets
            );
            if (!asset_load)
                return;

            auto upload_service = AsyncRenderUploadService::addTo(builder);
            if (!upload_service)
                return;

            auto plan = std::move(builder).compile();
            if (!plan)
                return;

            runtime_ = std::make_unique<lux::exec::AsyncRuntime>(
                std::move(*plan),
                config
            );
            asset_load_ = std::make_unique<
                lux::asset_runtime::AssetLoadService>(
                    std::move(*asset_load)
                );
            upload_ = std::make_unique<AsyncRenderUploadService>(
                std::move(*upload_service)
            );
            if (!upload_->bind(*runtime_, upload, sync))
            {
                asset_load_->close();
                detail::closeRuntime(*runtime_);
                asset_load_.reset();
                upload_.reset();
                runtime_.reset();
                return;
            }
            valid_ = true;
        }

        ~AsyncTestServices()
        {
            close();
        }

        AsyncTestServices(const AsyncTestServices&) = delete;
        AsyncTestServices& operator=(const AsyncTestServices&) = delete;
        AsyncTestServices(AsyncTestServices&&) = delete;
        AsyncTestServices& operator=(AsyncTestServices&&) = delete;

        [[nodiscard]] bool valid() const noexcept { return valid_; }

        [[nodiscard]] lux::asset_runtime::AssetClient assetClient() const noexcept
        {
            return asset_load_ ? asset_load_->client()
                               : lux::asset_runtime::AssetClient{};
        }

        [[nodiscard]] lux::render::RenderUploadClient uploadClient() const noexcept
        {
            return upload_ ? upload_->client()
                           : lux::render::RenderUploadClient{};
        }

        [[nodiscard]] lux::exec::AsyncRuntime& runtime() noexcept
        {
            return *runtime_;
        }

        std::size_t drainMainThreadCompletions(std::size_t budget = 256u)
        {
            return runtime_ ? runtime_->drainMainThreadCompletions(budget) : 0u;
        }

        /// Deterministically advances a fake render consumer until every
        /// operation accepted before the call has crossed the coordinator,
        /// upload reply and main-adoption boundaries. Production never waits
        /// this way; tests use it instead of depending on wall-clock timing.
        template <class Progress>
        [[nodiscard]] bool settle(
            Progress&& progress,
            std::size_t max_turns = 10000u)
        {
            if (!runtime_ || !upload_)
                return false;

            std::size_t stable_turns = 0u;
            for (std::size_t turn = 0u; turn < max_turns; ++turn)
            {
                progress();
                (void)runtime_->drainMainThreadCompletions(256u);
                const auto runtime_stats = runtime_->stats();
                const auto upload_report = upload_->report();
                const bool idle =
                    runtime_stats.queued_packets == 0u &&
                    runtime_stats.accepted == runtime_stats.dispatched &&
                    runtime_stats.active_operations == 0u &&
                    !runtime_stats.main_completion_pending &&
                    upload_report.clean;
                if (idle)
                {
                    if (++stable_turns == 2u)
                        return true;
                }
                else
                    stable_turns = 0u;
            }
            return false;
        }

        void close() noexcept
        {
            if (closed_)
                return;
            closed_ = true;
            if (!runtime_)
                return;

            if (asset_load_)
                asset_load_->close();
            if (upload_)
            {
                detail::closeUpload(*upload_, *runtime_);
                if (upload_->report().clean)
                    upload_->unbind();
            }
            detail::closeRuntime(*runtime_);
            valid_ = false;
        }

    private:
        std::unique_ptr<lux::exec::AsyncRuntime> runtime_;
        std::unique_ptr<lux::asset_runtime::AssetLoadService> asset_load_;
        std::unique_ptr<AsyncRenderUploadService> upload_;
        bool valid_{false};
        bool closed_{false};
    };
}
