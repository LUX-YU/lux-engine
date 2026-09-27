#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <semaphore>

namespace lux::asset
{
    class DecodeProbe final : public Asset
    {
    public:
        explicit DecodeProbe(AssetId id) : Asset(AssetInfo{.id = id}, {}), thread(std::this_thread::get_id()) {}
        std::thread::id thread;
        static inline std::atomic<unsigned> decodes{};
    };

    template <> struct TAssetSerDeser<DecodeProbe> final
    {
        static cxx::expected<std::shared_ptr<const DecodeProbe>, AssetDecodeFailure> decode(
            AssetId id,
            cxx::SharedBytes<> bytes,
            AssetDecodeLimits
        ) noexcept
        {
            ++DecodeProbe::decodes;
            if (bytes.data()[0] == std::byte{0xFF})
                return cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_PAYLOAD, 7});
            return std::make_shared<const DecodeProbe>(id);
        }
    };
}

namespace
{
    using namespace lux;
    using namespace process::asset_loading;
    asset::AssetId identity(std::uint8_t value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return asset::AssetId{bytes};
    }
    asset::AssetBlob image(std::byte value)
    {
        auto bytes = std::make_shared<const std::vector<std::byte>>(16, value);
        return {cxx::SharedBytes<>::fromOwner(bytes, *bytes)};
    }
    struct Fallback final : AssetReadPort::Endpoint
    {
        asset::AssetBlob bytes{image(std::byte{7})};
        unsigned requests{};
        async::SubmitResult submit(
            ReadAssetImage,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            async::SubmitOptions
        ) noexcept override
        {
            ++requests;
            complete(state, Outcome{bytes});
            return {};
        }
    };
    struct Reply final
    {
        std::atomic<bool> ready{};
        std::thread::id thread;
        asset::AssetBlob bytes;
        static void complete(void* state, AssetReadPort::Outcome&& result) noexcept
        {
            auto& self = *static_cast<Reply*>(state);
            assert(result);
            self.thread = std::this_thread::get_id();
            self.bytes = std::move(*result);
            self.ready.store(true, std::memory_order_release);
        }
        void wait()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!ready.load(std::memory_order_acquire))
            {
                assert(std::chrono::steady_clock::now() < deadline);
                std::this_thread::yield();
            }
        }
    };
}
int main()
{
    auto runtime = process::ExecutionRuntime::create({1, 64, 64, {64}});
    assert(runtime);
    process::TaskScope tasks{*runtime};
    auto fallback = std::make_shared<Fallback>();
    auto first = makeAssetReadOverlay({{identity(1), image(std::byte{1})}}, AssetReadPort{fallback});
    assert(first);
    auto second = makeAssetReadOverlay({{identity(1), image(std::byte{2})}}, AssetReadPort{fallback});
    assert(second);
    Reply a, b, other;
    assert(first->submit({identity(1)}, &a, &Reply::complete, {}));
    assert(second->submit({identity(1)}, &b, &Reply::complete, {}));
    assert(first->submit({identity(2)}, &other, &Reply::complete, {}));
    *first = {};
    *second = {};
    a.wait();
    b.wait();
    other.wait();
    assert(a.thread == std::this_thread::get_id() && b.thread == std::this_thread::get_id());
    assert(a.bytes.bytes.data()[0] == std::byte{1} && b.bytes.bytes.data()[0] == std::byte{2});
    assert(other.bytes.bytes.data()[0] == std::byte{7} && fallback->requests == 1);
    const asset::AssetDecodeLimits limits{1024, 1024, 0};
    auto decoded =
        stdexec::sync_wait(loadAsset<asset::DecodeProbe>(AssetReadPort{fallback}, runtime->cpu(), identity(2), limits));
    assert(decoded && std::get<0>(*decoded)->thread != std::this_thread::get_id());
    AssetLoadFailure failure;
    auto reject = [&](auto sender) {
        return stdexec::sync_wait(stdexec::upon_error(std::move(sender), [&](AssetLoadFailure error) noexcept {
            failure = error;
            return std::shared_ptr<const asset::DecodeProbe>{};
        }));
    };
    const auto before_failure = asset::DecodeProbe::decodes.load();
    auto no_scheduler = reject(loadAsset<asset::DecodeProbe>(AssetReadPort{fallback}, {}, identity(2), limits));
    assert(no_scheduler && !std::get<0>(*no_scheduler));
    assert(
        failure.code == EAssetLoadError::EXECUTION_FAILURE &&
        failure.execution_error == process::EExecutionError::STOPPING
    );
    assert(asset::DecodeProbe::decodes == before_failure);
    fallback->bytes = image(std::byte{0xFF});
    auto invalid = reject(loadAsset<asset::DecodeProbe>(AssetReadPort{fallback}, runtime->cpu(), identity(2), limits));
    assert(invalid && !std::get<0>(*invalid));
    assert(failure.code == EAssetLoadError::DECODE_FAILURE && failure.decode.offset == 7);

    // A synchronous endpoint has completed on Main, but decoding is still queued.
    // Cancellation before that CPU slot runs must not enter the codec.
    std::binary_semaphore entered{0}, proceed{0};
    auto blocker = stdexec::then(stdexec::schedule(runtime->cpu()), [&]() noexcept {
        entered.release();
        proceed.acquire();
    });
    assert(tasks.submit({"Block CPU", "test"}, [sender = std::move(blocker)](process::TaskReporter) mutable noexcept {
        return stdexec::upon_error(std::move(sender), [](process::EExecutionError) noexcept { std::terminate(); });
    }));
    assert(entered.try_acquire_for(std::chrono::seconds(5)));
    std::stop_source cancel;
    std::atomic<bool> stopped{};
    const auto before_cancel = asset::DecodeProbe::decodes.load();
    auto values = stdexec::then(
        loadAsset<asset::DecodeProbe>(AssetReadPort{fallback}, runtime->cpu(), identity(2), limits, cancel.get_token()),
        [](std::shared_ptr<const asset::DecodeProbe>) noexcept { std::terminate(); }
    );
    auto errors = stdexec::upon_error(std::move(values), [](AssetLoadFailure) noexcept { std::terminate(); });
    assert(tasks.submit(
        {"Decode cancelled", "test"},
        [&, sender = std::move(errors)](process::TaskReporter) mutable noexcept {
            return stdexec::upon_stopped(std::move(sender), [&]() noexcept { stopped = true; });
        }
    ));
    cancel.request_stop();
    proceed.release();
    assert(tasks.join());
    assert(stopped && asset::DecodeProbe::decodes == before_cancel);
    runtime->requestStop();
    assert(runtime->join());
    std::cout << "PASS immutable asset overlays: byte transport, CPU decode, independent versions, fallback, retained "
                 "bytes\n";
}
