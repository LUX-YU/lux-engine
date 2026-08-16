#include <lux/engine/resource/asset/AssetVfs.hpp>
#include <lux/engine/runtime/assets/AssetLoadSenders.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace ex = stdexec;

namespace
{
    class ProbeProvider final : public lux::asset::IAssetProvider
    {
    public:
        ProbeProvider(
            lux::asset::asset_id_t id,
            int transient_attempts) noexcept
            : id_(id), transient_attempts_(transient_attempts)
        {}

        [[nodiscard]] bool contains(
            const lux::asset::asset_id_t& id) const override
        {
            return id == id_;
        }

        [[nodiscard]] lux::cxx::expected<
            lux::asset::AssetBlob,
            lux::asset::EAssetError>
        open(const lux::asset::asset_id_t& id) const override
        {
            if (id != id_)
            {
                return lux::cxx::unexpected(
                    lux::asset::EAssetError::ASSET_NOT_EXIST);
            }
            io_thread_ = std::this_thread::get_id();
            const int attempt = opens_.fetch_add(1, std::memory_order_acq_rel);
            if (attempt < transient_attempts_)
            {
                return lux::cxx::unexpected(
                    lux::asset::EAssetError::FILE_OPEN_FAIL);
            }

            auto bytes = std::shared_ptr<std::byte[]>(new std::byte[8]);
            std::memset(bytes.get(), 0xA5, 8);
            return lux::asset::AssetBlob::fromSharedArray(
                std::move(bytes), 8u);
        }

        [[nodiscard]] std::optional<lux::asset::asset_id_t> resolve(
            std::string_view) const override
        {
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::string> pathOf(
            const lux::asset::asset_id_t&) const override
        {
            return std::nullopt;
        }

        void enumerate(
            const std::function<void(const lux::asset::ProviderEntry&)>&)
            const override
        {}

        [[nodiscard]] int opens() const noexcept
        {
            return opens_.load(std::memory_order_acquire);
        }

        [[nodiscard]] std::thread::id ioThread() const noexcept
        {
            return io_thread_;
        }

    private:
        lux::asset::asset_id_t id_;
        int transient_attempts_{0};
        mutable std::atomic<int> opens_{0};
        mutable std::thread::id io_thread_{};
    };

    struct AwaitResult final
    {
        std::atomic<bool> done{false};
        std::atomic<bool> domain_failure{false};
        std::atomic<bool> runtime_failure{false};
        std::atomic<int> error{0};
    };

    struct BatchAwaitResult final
    {
        std::atomic<bool> done{false};
        std::atomic<bool> success{false};
        std::atomic<std::size_t> count{0u};
    };

    void awaitLoad(
        lux::asset_runtime::AssetClient client,
        lux::asset::asset_id_t id,
        AwaitResult& result) noexcept
    {
        auto terminal = ex::sync_wait(
            lux::asset_runtime::loadAsset(client, id));
        if (terminal)
        {
            auto& outcome = std::get<0>(*terminal);
            if (!outcome && !outcome.error().isRuntime())
            {
                result.domain_failure.store(true, std::memory_order_relaxed);
                result.error.store(
                    static_cast<int>(outcome.error().domainError()),
                    std::memory_order_relaxed);
            }
            else if (!outcome)
            {
                result.runtime_failure.store(true, std::memory_order_relaxed);
                result.error.store(
                    static_cast<int>(outcome.error().runtimeError()),
                    std::memory_order_relaxed);
            }
        }
        result.done.store(true, std::memory_order_release);
    }

    void awaitBatch(
        lux::asset_runtime::AssetClient client,
        lux::asset_runtime::LoadAssetBatch operation,
        BatchAwaitResult& result) noexcept
    {
        auto terminal = ex::sync_wait(lux::exec::execute(
            client.loadBatchClient(),
            std::move(operation)));
        if (terminal)
        {
            auto& outcome = std::get<0>(*terminal);
            if (outcome)
            {
                result.count.store(
                    outcome->assets.size(),
                    std::memory_order_relaxed);
                result.success.store(true, std::memory_order_relaxed);
            }
        }
        result.done.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool pumpUntil(
        lux::exec::AsyncRuntime& runtime,
        const AwaitResult& result,
        std::chrono::seconds timeout) noexcept
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!result.done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
        {
            (void)runtime.drainMainThreadCompletions();
            std::this_thread::yield();
        }
        return result.done.load(std::memory_order_acquire);
    }
}

int main()
{
    int failures = 0;
    auto check = [&failures](bool condition, const char* message)
    {
        std::printf(condition ? "[ ok ] %s\n" : "[FAIL] %s\n", message);
        if (!condition)
            ++failures;
    };

    lux::asset::AssetManager manager{
        lux::asset::runtimeAssetCodecCatalog()};
    lux::exec::AsyncRuntimeBuilder builder;
    auto service_result = lux::asset_runtime::AssetLoadService::addTo(
        builder,
        manager);
    check(service_result.has_value(), "asset operations register before freeze");
    auto plan = std::move(builder).compile();
    check(plan.has_value(), "asset operation registry freezes");
    if (!service_result || !plan)
        return 1;

    lux::exec::AsyncRuntime runtime(
        std::move(*plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1,
            .background_cpu_concurrency = 1});
    auto service = std::move(*service_result);

    const auto client = service.client();
    const std::array<lux::asset::asset_id_t, 3> duplicate_ids{
        manager.generateUUID(),
        {},
        {}};
    auto ids = duplicate_ids;
    ids[1] = ids[0];
    auto deduplicated = client.loadBatchOperation(ids);
    check(
        deduplicated.assets && deduplicated.assets->size() == 1u,
        "dynamic dependency batch removes nil/duplicate ids in stable order");

    BatchAwaitResult empty_batch_result;
    std::thread empty_batch_waiter(
        awaitBatch,
        client,
        client.loadBatchOperation(
            std::span<const lux::asset::asset_id_t>{}),
        std::ref(empty_batch_result));
    const auto empty_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!empty_batch_result.done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < empty_deadline)
        std::this_thread::yield();
    empty_batch_waiter.join();
    check(
        empty_batch_result.success.load(std::memory_order_acquire) &&
            empty_batch_result.count.load(std::memory_order_relaxed) == 0u,
        "empty dynamic dependency join completes exactly once");

    const auto main_thread = std::this_thread::get_id();
    const auto bad_id = manager.generateUUID();
    auto provider = std::make_shared<ProbeProvider>(bad_id, 0);
    auto vfs = std::make_shared<lux::asset::AssetVfs>();
    (void)vfs->mount({
        .root = "/Game",
        .provider = provider,
        .priority = 0});
    manager.setVfs(vfs);

    AwaitResult bad_result;
    std::thread bad_waiter(
        awaitLoad,
        service.client(),
        bad_id,
        std::ref(bad_result));
    const bool bad_done = pumpUntil(runtime, bad_result, std::chrono::seconds(5));
    check(
        bad_done,
        "BlockingIO -> TBB -> main -> coordinator pipeline terminates");
    if (bad_done)
        bad_waiter.join();
    else
    {
        (void)lux::exec::testing::closeRuntime(runtime);
        bad_waiter.join();
    }
    check(
        bad_result.domain_failure.load(std::memory_order_acquire),
        "malformed asset is a structured domain failure");
    check(
        provider->opens() == 1 && provider->ioThread() != main_thread,
        "provider open runs once on BlockingIO compatibility execution");

    const auto retry_id = manager.generateUUID();
    auto retry_provider = std::make_shared<ProbeProvider>(retry_id, 1);
    auto retry_vfs = std::make_shared<lux::asset::AssetVfs>();
    (void)retry_vfs->mount({
        .root = "/Game",
        .provider = retry_provider,
        .priority = 0});
    manager.setVfs(retry_vfs);

    AwaitResult retry_result;
    std::thread retry_waiter(
        awaitLoad,
        service.client(),
        retry_id,
        std::ref(retry_result));
    const bool retry_done = pumpUntil(
        runtime,
        retry_result,
        std::chrono::seconds(5));
    check(retry_done, "coordinator timer resumes transient asset load");
    if (retry_done)
        retry_waiter.join();
    else
    {
        (void)lux::exec::testing::closeRuntime(runtime);
        retry_waiter.join();
    }
    check(
        retry_provider->opens() == 2,
        "transient failure retries without a frame-driven clock");

    service.close();
    (void)lux::exec::testing::closeRuntime(runtime);

    // A request parked in coordinator backoff is not owned by a worker scope.
    // Runtime close must flush its timer and settle its receiver exactly once.
    lux::asset::AssetManager closing_manager{
        lux::asset::runtimeAssetCodecCatalog()};
    lux::exec::AsyncRuntimeBuilder closing_builder;
    auto closing_service_result =
        lux::asset_runtime::AssetLoadService::addTo(
            closing_builder,
            closing_manager);
    auto closing_plan = std::move(closing_builder).compile();
    check(
        closing_service_result.has_value() && closing_plan.has_value(),
        "close-window asset fixture assembles");
    if (closing_service_result && closing_plan)
    {
        lux::exec::AsyncRuntime closing_runtime(
            std::move(*closing_plan),
            lux::exec::AsyncRuntimeConfig{
                .blocking_io_threads = 1,
                .background_cpu_concurrency = 1});
        auto closing_service = std::move(*closing_service_result);
        const auto closing_id = closing_manager.generateUUID();
        auto closing_provider =
            std::make_shared<ProbeProvider>(closing_id, 100);
        auto closing_vfs = std::make_shared<lux::asset::AssetVfs>();
        (void)closing_vfs->mount({
            .root = "/Game",
            .provider = closing_provider,
            .priority = 0});
        closing_manager.setVfs(closing_vfs);

        AwaitResult closing_result;
        std::thread closing_waiter(
            awaitLoad,
            closing_service.client(),
            closing_id,
            std::ref(closing_result));
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (closing_provider->opens() == 0 &&
               std::chrono::steady_clock::now() < deadline)
        {
            (void)closing_runtime.drainMainThreadCompletions();
            std::this_thread::yield();
        }
        check(
            closing_provider->opens() == 1,
            "request enters runtime-owned backoff before close");
        (void)lux::exec::testing::closeRuntime(closing_runtime);
        closing_waiter.join();
        check(
            closing_result.done.load(std::memory_order_acquire) &&
                closing_result.runtime_failure.load(
                    std::memory_order_acquire) &&
                closing_result.error.load(std::memory_order_relaxed) ==
                    static_cast<int>(
                        lux::exec::EAsyncSubmitError::STOPPING),
            "close settles a parked retry exactly once with STOPPING");
        closing_service.close();
    }
    return failures == 0 ? 0 : 1;
}
