#include <lux/engine/editor/sessions/scene/detail/SceneResources.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace
{
    class HeldProvider final : public lux::asset::IAssetProvider
    {
    public:
        std::shared_ptr<lux::asset::IAssetProvider> source;
        mutable std::atomic<bool> entered{};
        std::atomic<bool> released{};
        std::optional<lux::asset::AssetId> resolve(std::string_view path) const override
        {
            return source->resolve(path);
        }
        bool contains(const lux::asset::AssetId &id) const override
        {
            return source->contains(id);
        }
        lux::cxx::expected<lux::asset::AssetBlob, lux::asset::EAssetStorageError> open(
            const lux::asset::AssetId &id) const override
        {
            entered.store(true, std::memory_order_release);
            while (!released.load(std::memory_order_acquire))
                released.wait(false, std::memory_order_acquire);
            return source->open(id);
        }
        void enumerate(const std::function<void(const lux::asset::ProviderEntry &)> &visitor) const override
        {
            source->enumerate(visitor);
        }
        std::optional<std::string> pathOf(const lux::asset::AssetId &id) const override
        {
            return source->pathOf(id);
        }
    };
    void wait(const std::atomic<bool> &value)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        while (!value.load(std::memory_order_acquire))
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    }
} // namespace
int main(int argc, char **argv)
{
    assert(argc == 2);
    using namespace lux::editor::sessions::detail;
    using Result = AssetResult<lux::asset::MeshAsset>;
    for (const bool stop : {false, true})
    {
        auto execution =
            lux::process::ExecutionRuntime::create({1, 16, 16, {16}, lux::process::BlockingSchedulerConfig{1, 16}});
        assert(execution);
        auto blocking = execution->blocking();
        assert(blocking);
        auto pak = lux::asset::PakAssetProvider::loadFromFile(argv[1]);
        assert(pak);
        auto provider = std::make_shared<HeldProvider>();
        provider->source = *pak;
        lux::asset::AssetVfs vfs;
        assert(vfs.mount({"/Seed", provider, 0}) != lux::asset::kInvalidMountId);
        auto endpoint = lux::process::asset_loading::VfsAssetReadEndpoint::create(vfs.view(), *blocking, {4});
        assert(endpoint);
        ResourceTasks tasks;
        auto result = std::make_shared<Result>();
        const std::weak_ptr<Result> lifetime = result;
        auto probe = std::make_shared<AssetCompletionProbe>();
        result->completion_probe = probe;
        std::array<std::uint8_t, 16> id{};
        id[0] = 0x53;
        id[1] = 0x56;
        id[2] = 1;
        id.back() = 10;
        assert(tasks.read<lux::asset::MeshAsset>((*endpoint)->port(), lux::asset::AssetId{id}, result));
        wait(provider->entered);
        assert(result->state.load() == Result::EState::PENDING && !result->value);
        if (stop)
            tasks.scope.requestStop();
        assert(result->state.load() == Result::EState::PENDING && !result->value);
        provider->released.store(true, std::memory_order_release);
        provider->released.notify_all();
        wait(probe->entered);
        const auto expected = stop ? Result::EState::CANCELLED : Result::EState::VALUE;
        assert(result->state.load(std::memory_order_acquire) == expected);
        assert(bool(result->value) == !stop);
        assert(tasks.close());
        for (unsigned step = 0; step != 32; ++step)
            assert(!tasks.done.load(std::memory_order_acquire) && !tasks.scope.closed());
        result.reset();
        assert(!lifetime.expired());
        std::printf("resource completion=%s state=%u close_pending_steps=32 callback_held=1 result_retained=1\n",
                    stop ? "set_stopped" : "set_value", static_cast<unsigned>(expected));
        std::fflush(stdout);
        probe->released.store(true, std::memory_order_release);
        probe->released.notify_all();
        wait(tasks.done);
        assert(tasks.scope.closed());
        (*endpoint)->requestStop();
        assert((*endpoint)->join());
        endpoint->reset();
        execution->requestStop();
        assert(execution->join());
        assert(lifetime.expired());
        std::printf("resource completion=%s callback_returned=1 scope_closed=1 result_destroyed=1 endpoint_joined=1 "
                    "execution_joined=1\n",
                    stop ? "set_stopped" : "set_value");
    }
    std::puts("resource completion protocol PASS actual pak/endpoint/decode/TaskScope; distinct value/stopped; owners "
              "joined");
}
