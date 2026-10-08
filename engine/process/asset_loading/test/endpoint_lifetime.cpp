#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <functional>
#include <thread>
#include <type_traits>

using namespace lux;
using namespace lux::process::asset_loading;

namespace
{
    const asset::AssetId Asset{*uuids::uuid::from_string("fedcba98-7654-3210-fedc-ba9876543210")};

    class BlockedProvider final : public asset::IAssetProvider
    {
    public:
        ~BlockedProvider() override
        {
            if (on_destroy)
            {
                on_destroy();
            }
        }

        std::optional<asset::AssetId> resolve(std::string_view) const override
        {
            return Asset;
        }

        bool contains(const asset::AssetId& id) const override
        {
            return id == Asset;
        }

        cxx::expected<asset::AssetBlob, asset::EAssetStorageError> open(const asset::AssetId&, std::size_t)
            const override
        {
            entered.store(true);
            entered.notify_one();
            proceed.wait(false);
            return asset::AssetBlob::fromShared(cxx::SharedBytes<>::copyOf(std::as_bytes(std::span("value"))));
        }

        void enumerate(const std::function<void(const asset::ProviderEntry&)>& visitor) const override
        {
            visitor({Asset, 1, "value", false});
        }

        std::optional<std::string> pathOf(const asset::AssetId&) const override
        {
            return "value";
        }

        std::function<void()> on_destroy;
        mutable std::atomic<bool> entered{};
        mutable std::atomic<bool> proceed{};
    };

    struct Reply final
    {
        std::size_t calls{};
        std::thread::id owner;
        bool succeeded{};
        std::optional<async::ESubmitError> failure;
        std::function<void()> on_complete;

        static void complete(void* target, AssetReadPort::Outcome&& result) noexcept
        {
            auto& self = *static_cast<Reply*>(target);
            assert(self.owner == std::this_thread::get_id());
            ++self.calls;
            self.succeeded = result.has_value();
            if (!result && result.error().isRuntime())
            {
                self.failure = result.error().runtimeError();
            }
            if (self.on_complete)
            {
                self.on_complete();
            }
        }
    };
} // namespace

namespace
{
    template <class T>
    concept HasJoin = requires(T& value) { value.join(); };

    template <class T>
    concept HasStop = requires(T& value) { value.requestStop(); };

    void ownerDeath(bool destroy_in_completion)
    {
        auto runtime = process::ExecutionRuntime::create({1, 16, 16, {16}, process::BlockingSchedulerConfig{1, 16}});
        assert(runtime);
        process::TaskScope tasks{*runtime};
        auto blocking = runtime->blocking();
        assert(blocking);
        asset::AssetVfs vfs;
        auto provider = std::make_shared<BlockedProvider>();
        auto mount = vfs.mount({"/Game", provider});
        assert(mount);
        auto created = VfsAssetReadEndpoint::create(vfs.view().capture(), *blocking, tasks, {1});
        assert(created);
        auto endpoint = std::move(*created);
        auto port = endpoint->port();
        Reply reply{0, std::this_thread::get_id(), false};
        Reply rejected{0, std::this_thread::get_id(), false};
        assert(port.submit({Asset, 1024}, &reply, &Reply::complete, {}));
        provider->entered.wait(false);
        assert(port.submit({Asset, 1024}, &rejected, &Reply::complete, {}).error() == async::ESubmitError::QUEUE_FULL);
        if (destroy_in_completion)
        {
            reply.on_complete = [&]() noexcept
            {
                endpoint.reset();
                assert(
                    port.submit({Asset, 1024}, &rejected, &Reply::complete, {}).error() == async::ESubmitError::STOPPING
                );
            };
        }
        else
        {
            endpoint.reset();
            assert(!provider->proceed.load() && reply.calls == 0);
            assert(
                port.submit({Asset, 1024}, &rejected, &Reply::complete, {}).error() == async::ESubmitError::STOPPING
            );
        }
        provider->proceed.store(true);
        provider->proceed.notify_one();
        assert(runtime->waitUntil([&]() noexcept { return reply.calls != 0; }));
        assert(reply.calls == 1 && reply.succeeded && rejected.calls == 0 && !endpoint);
        assert(tasks.join());
        runtime->requestStop();
        assert(runtime->join());
    }

    void cancelledQueuedRead()
    {
        auto runtime = process::ExecutionRuntime::create({1, 16, 16, {16}, process::BlockingSchedulerConfig{1, 16}});
        assert(runtime);
        process::TaskScope blocker{*runtime};
        auto scope = std::make_unique<process::TaskScope>(*runtime);
        auto blocking = runtime->blocking();
        assert(blocking);
        std::atomic<bool> entered{}, proceed{};
        assert(blocker.submit(
            {"Hold blocking worker", "test"},
            [&, scheduler = *blocking](process::TaskReporter) noexcept
            {
                return stdexec::then(
                    stdexec::schedule(scheduler),
                    [&]() noexcept
                    {
                        entered.store(true);
                        entered.notify_one();
                        proceed.wait(false);
                    }
                );
            }
        ));
        entered.wait(false);
        asset::AssetVfs vfs;
        auto provider = std::make_shared<BlockedProvider>();
        provider->proceed.store(true);
        auto mount = vfs.mount({"/Game", provider});
        assert(mount);
        auto endpoint = VfsAssetReadEndpoint::create(vfs.view().capture(), *blocking, *scope, {1});
        assert(endpoint);
        auto port = (*endpoint)->port();
        Reply reply{0, std::this_thread::get_id(), false};
        assert(port.submit({Asset, 1024}, &reply, &Reply::complete, {}));
        endpoint->reset();
        scope.reset();
        assert(reply.calls == 0 && !provider->entered.load() && !proceed.load());
        Reply rejected{0, std::this_thread::get_id(), false};
        assert(port.submit({Asset, 1024}, &rejected, &Reply::complete, {}).error() == async::ESubmitError::STOPPING);
        proceed.store(true);
        proceed.notify_one();
        assert(runtime->waitUntil([&]() noexcept { return reply.calls == 1; }));
        assert(reply.failure == async::ESubmitError::STOPPING && !reply.succeeded);
        assert(!provider->entered.load() && rejected.calls == 0);
        assert(blocker.join());
        runtime->requestStop();
        assert(runtime->join());
    }

    void cleanupAndCapacity()
    {
        auto runtime = process::ExecutionRuntime::create({1, 16, 16, {16}, process::BlockingSchedulerConfig{1, 16}});
        assert(runtime);
        process::TaskScope tasks{*runtime};
        auto blocking = runtime->blocking();
        assert(blocking);
        asset::AssetVfs vfs;
        auto provider = std::make_shared<BlockedProvider>();
        provider->proceed.store(true);
        auto mount = vfs.mount({"/Game", provider});
        assert(mount);
        assert(
            VfsAssetReadEndpoint::create(vfs.view(), *blocking, tasks, {0}).error() ==
            EVfsAssetReadEndpointError::INVALID_ARGUMENT
        );
        auto endpoint = VfsAssetReadEndpoint::create(vfs.view().capture(), *blocking, tasks, {1});
        assert(endpoint);
        auto port = (*endpoint)->port();
        for (unsigned i = 0; i != 32; ++i)
        {
            Reply reply{0, std::this_thread::get_id(), false};
            assert(port.submit({Asset, 1024}, &reply, &Reply::complete, {}));
            assert(runtime->waitUntil([&]() noexcept { return reply.calls == 1; }));
            assert(reply.succeeded);
        }
        tasks.requestStop();
        Reply rejected{0, std::this_thread::get_id(), false};
        for (unsigned i = 0; i != 2; ++i)
        {
            // Failed TaskScope admission releases endpoint capacity, so the next rejection stays STOPPING.
            assert(
                port.submit({Asset, 1024}, &rejected, &Reply::complete, {}).error() == async::ESubmitError::STOPPING
            );
        }
        unsigned destroyed = 0;
        provider->on_destroy = [&]() noexcept
        {
            ++destroyed;
            assert(
                port.submit({Asset, 1024}, &rejected, &Reply::complete, {}).error() == async::ESubmitError::STOPPING
            );
        };
        provider.reset();
        *mount = {};
        assert(destroyed == 0);
        endpoint->reset();
        assert(destroyed == 1 && rejected.calls == 0);
        assert(tasks.join());
        runtime->requestStop();
        assert(runtime->join());
    }

    std::unique_ptr<VfsAssetReadEndpoint>* wake_owner{};
    const std::thread::id OwnerThread = std::this_thread::get_id();

    void wakeReentry()
    {
        auto runtime = process::ExecutionRuntime::create({1, 16, 16, {16}, process::BlockingSchedulerConfig{1, 16}});
        assert(runtime);
        process::TaskScope tasks{*runtime};
        asset::AssetVfs vfs;
        auto provider = std::make_shared<BlockedProvider>();
        provider->proceed.store(true);
        auto mount = vfs.mount({"/Game", provider});
        assert(mount);
        auto endpoint = VfsAssetReadEndpoint::create(vfs.view().capture(), *runtime->blocking(), tasks, {1});
        assert(endpoint);
        auto port = (*endpoint)->port();
        wake_owner = &*endpoint;
        runtime->setWake(+[]() noexcept
                         {
                             if (std::this_thread::get_id() == OwnerThread)
                             {
                                 wake_owner->reset();
                             }
                         });
        Reply reply{0, OwnerThread, false};
        assert(port.submit({Asset, 1024}, &reply, &Reply::complete, {}));
        assert(!*endpoint);
        runtime->setWake(nullptr);
        assert(runtime->waitUntil([&]() noexcept { return reply.calls == 1; }));
        assert(reply.succeeded);
        assert(tasks.join());
        runtime->requestStop();
        assert(runtime->join());
        wake_owner = nullptr;
    }
} // namespace

int main()
{
    static_assert(!HasJoin<VfsAssetReadEndpoint> && !HasStop<VfsAssetReadEndpoint>);
    static_assert(!std::is_copy_constructible_v<VfsAssetReadEndpoint>);
    static_assert(!std::is_move_constructible_v<VfsAssetReadEndpoint>);
    static_assert(std::
                      is_same_v<VfsAssetReadEndpoint::CreateResult::value_type, std::unique_ptr<VfsAssetReadEndpoint>>);
    ownerDeath(false);
    ownerDeath(true);
    cancelledQueuedRead();
    cleanupAndCapacity();
    wakeReentry();
    std::puts("PASS actual VFS endpoint owner death, stale ports, queued cancellation, completion/wake/cleanup reentry "
              "and bounded capacity");
}
