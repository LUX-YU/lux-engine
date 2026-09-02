#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace
{
    [[nodiscard]] lux::asset::AssetId makeId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = tail;
        return lux::asset::AssetId{bytes};
    }

    class SlowProvider final : public lux::asset::IAssetProvider
    {
    public:
        explicit SlowProvider(lux::asset::AssetId id) : id_(id)
        {
        }

        [[nodiscard]] std::optional<lux::asset::AssetId> resolve(std::string_view path) const override
        {
            return path == "slow" ? std::optional{id_} : std::nullopt;
        }

        [[nodiscard]] bool contains(const lux::asset::AssetId& id) const override
        {
            return id == id_;
        }

        [[nodiscard]] lux::cxx::expected<lux::asset::AssetBlob, lux::asset::EAssetStorageError>
        open(const lux::asset::AssetId& id) const override
        {
            if (id != id_)
                return lux::cxx::unexpected(lux::asset::EAssetStorageError::NOT_FOUND);
            open_thread = std::this_thread::get_id();
            entered.store(true, std::memory_order_release);
            entered.notify_all();
            release.wait(false, std::memory_order_acquire);
            const std::array bytes{std::byte{0x55U}};
            return lux::asset::AssetBlob::fromShared(lux::cxx::SharedBytes<>::copyOf(bytes));
        }

        void enumerate(const std::function<void(const lux::asset::ProviderEntry&)>& fn) const override
        {
            fn(lux::asset::ProviderEntry{id_, 1U, "slow", false});
        }

        [[nodiscard]] std::optional<std::string> pathOf(const lux::asset::AssetId& id) const override
        {
            return id == id_ ? std::optional<std::string>{"slow"} : std::nullopt;
        }

        mutable std::atomic_bool entered{};
        mutable std::atomic_bool release{};
        mutable std::thread::id open_thread;

    private:
        lux::asset::AssetId id_;
    };

    struct Completion final
    {
        std::atomic_size_t count{};
        bool value{};
        std::optional<lux::async::ESubmitError> runtime_error;
    };

    void complete(
        void* opaque,
        lux::process::asset_loading::AssetReadPort::Outcome&& outcome
    ) noexcept
    {
        auto& result = *static_cast<Completion*>(opaque);
        if (outcome)
            result.value = outcome->bytes.size() == 1U;
        else if (outcome.error().isRuntime())
            result.runtime_error = outcome.error().runtimeError();
        result.count.fetch_add(1U, std::memory_order_release);
        result.count.notify_all();
    }
}

int main()
{
    using namespace lux::process;
    using namespace lux::process::asset_loading;

    auto without_blocking = ExecutionRuntime::create({1U, 2U, 2U, {2U}});
    assert(without_blocking && !without_blocking->blocking());
    without_blocking->requestStop();
    assert(without_blocking->join());

    auto created = ExecutionRuntime::create({
        1U,
        2U,
        2U,
        {2U},
        BlockingSchedulerConfig{1U, 1U}
    });
    assert(created);
    auto runtime = std::move(*created);
    auto blocking = runtime.blocking();
    assert(blocking);

    lux::asset::AssetVfs vfs;
    const auto id = makeId(1U);
    auto provider = std::make_shared<SlowProvider>(id);
    assert(vfs.mount({"/Game", provider, 0}) != lux::asset::kInvalidMountId);
    auto endpoint = VfsAssetReadEndpoint::create(vfs.view(), *blocking, {2U});
    assert(endpoint);
    auto port = (*endpoint)->port();

    Completion first;
    const auto owner = std::this_thread::get_id();
    const auto began = std::chrono::steady_clock::now();
    assert(port.submit(ReadAssetImage{id}, &first, &complete));
    const auto submit_elapsed = std::chrono::steady_clock::now() - began;
    assert(submit_elapsed < std::chrono::milliseconds(100));
    provider->entered.wait(false, std::memory_order_acquire);
    assert(provider->open_thread != owner);

    Completion queued;
    assert(port.submit(ReadAssetImage{id}, &queued, &complete));
    Completion rejected;
    const auto overflow = port.submit(ReadAssetImage{id}, &rejected, &complete);
    assert(!overflow && overflow.error() == lux::async::ESubmitError::QUEUE_FULL);
    assert(rejected.count.load(std::memory_order_acquire) == 0U);

    std::atomic<EVfsAssetReadEndpointError> wrong_thread{};
    std::jthread foreign([&] {
        const auto joined = (*endpoint)->join();
        assert(!joined);
        wrong_thread.store(joined.error(), std::memory_order_release);
    });
    foreign.join();
    assert(wrong_thread.load(std::memory_order_acquire) == EVfsAssetReadEndpointError::WRONG_THREAD);

    (*endpoint)->requestStop();
    provider->release.store(true, std::memory_order_release);
    provider->release.notify_all();
    assert((*endpoint)->join());
    assert(first.count.load(std::memory_order_acquire) == 1U && first.value);
    assert(queued.count.load(std::memory_order_acquire) == 1U);
    assert(queued.runtime_error == lux::async::ESubmitError::STOPPING);

    Completion stale;
    const auto stopped = port.submit(ReadAssetImage{id}, &stale, &complete);
    assert(!stopped && stopped.error() == lux::async::ESubmitError::STOPPING);
    runtime.requestStop();
    assert(runtime.join());
    return 0;
}
