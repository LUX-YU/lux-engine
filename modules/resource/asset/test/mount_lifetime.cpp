#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <functional>
#include <thread>
#include <type_traits>
#include <utility>

using namespace lux;
using namespace lux::asset;

namespace
{
    const AssetId First{*uuids::uuid::from_string("fedcba98-7654-3210-fedc-ba9876543210")};
    const AssetId Second{*uuids::uuid::from_string("fedcba98-7654-3210-fedc-ba9876543211")};

    class Provider final : public IAssetProvider
    {
    public:
        explicit Provider(AssetId id = First, std::uint32_t revision = 1) noexcept : id_(id), revision_(revision) {}

        ~Provider() override
        {
            if (on_destroy)
            {
                on_destroy();
            }
        }

        std::optional<AssetId> resolve(std::string_view) const override
        {
            return id_;
        }

        bool contains(const AssetId& id) const override
        {
            return id == id_;
        }

        cxx::expected<AssetBlob, EAssetStorageError> open(const AssetId&, std::size_t limit) const override
        {
            if (entered)
            {
                entered->store(true);
                entered->notify_one();
                proceed->wait(false);
            }
            if (limit < sizeof(revision_))
            {
                return cxx::unexpected(EAssetStorageError::LIMIT_EXCEEDED);
            }
            return AssetBlob::fromShared(cxx::SharedBytes<>::copyOf(std::as_bytes(std::span(&revision_, 1))));
        }

        void enumerate(const std::function<void(const ProviderEntry&)>& visitor) const override
        {
            visitor({id_, revision_, "value", false});
        }

        std::optional<std::string> pathOf(const AssetId&) const override
        {
            return "value";
        }

        std::atomic<bool>* entered{};
        std::atomic<bool>* proceed{};
        std::function<void()> on_destroy;

    private:
        AssetId id_;
        std::uint32_t revision_;
    };

    void lifetime()
    {
        static_assert(!std::is_copy_constructible_v<MountLease>);
        static_assert(!std::is_copy_assignable_v<MountLease>);
        static_assert(std::is_nothrow_move_constructible_v<MountLease>);
        static_assert(std::is_nothrow_move_assignable_v<MountLease>);
        AssetVfs vfs;
        int destroyed = 0;
        auto provider = std::make_shared<Provider>();
        provider->on_destroy = [&]() noexcept { ++destroyed; };
        std::weak_ptr<IAssetProvider> weak = provider;
        {
            auto lease = vfs.mount({"/Game", std::move(provider)});
            assert(lease && lease->id() != kInvalidMountId);
            assert(vfs.mountCount() == 1);
            auto frozen = vfs.view().capture();
            MountLease moved{std::move(*lease)};
            assert(lease->id() == kInvalidMountId);
            moved = std::move(moved);
            assert(vfs.mountCount() == 1);
            moved = {};
            assert(vfs.mountCount() == 0 && vfs.resolve("/Game/value").isNull());
            assert(frozen.resolve("/Game/value") == First && frozen.open(First));
            assert(destroyed == 0 && !weak.expired());
        }
        assert(destroyed == 1 && weak.expired());

        MountLease survivor;
        AssetVfsView live;
        {
            AssetVfs temporary;
            auto mount = temporary.mount({"/Game", std::make_shared<Provider>()});
            assert(mount);
            survivor = std::move(*mount);
            live = temporary.view();
        }
        assert(live.resolve("/Game/value") == First);
        survivor = {};
        assert(live.resolve("/Game/value").isNull());

        auto a = vfs.mount({"/A", std::make_shared<Provider>()});
        auto b = vfs.mount({"/B", std::make_shared<Provider>(Second)});
        assert(a && b);
        *a = std::move(*b);
        assert(vfs.mountCount() == 1 && b->id() == kInvalidMountId);
        assert(vfs.resolve("/A/value").isNull() && vfs.resolve("/B/value") == Second);
    }

    void acceptedRead()
    {
        AssetVfs vfs;
        std::atomic<bool> entered{}, proceed{};
        auto provider = std::make_shared<Provider>();
        provider->entered = &entered;
        provider->proceed = &proceed;
        std::weak_ptr<IAssetProvider> weak = provider;
        auto lease = vfs.mount({"/Game", std::move(provider)});
        assert(lease);
        std::thread reader(
            [view = vfs.view()]
            {
                const auto blob = view.open(First);
                assert(blob && blob->bytes.size() == sizeof(std::uint32_t));
            }
        );
        entered.wait(false);
        *lease = {};
        assert(vfs.mountCount() == 0 && !weak.expired());
        assert(vfs.open(First).error() == EAssetStorageError::NOT_FOUND);
        proceed.store(true);
        proceed.notify_one();
        reader.join();
        assert(weak.expired());
    }

    void batch()
    {
        AssetVfs vfs;
        std::array<MountDesc, 2> descriptors{
            {{"/A", std::make_shared<Provider>(First, 1)}, {"/B", std::make_shared<Provider>(Second, 1)}}
        };
        auto leases = vfs.replaceMounts({}, descriptors);
        assert(leases && leases->size() == 2);
        auto frozen = vfs.view().capture();
        const auto original = leases->front().id();
        descriptors[1].root = "illegal";
        auto rejected = vfs.replaceMounts(*leases, descriptors);
        assert(!rejected && rejected.error() == EMountUpdateError::INVALID_DESCRIPTOR);
        assert(leases->front().id() == original && vfs.mountCount() == 2);
        AssetVfs foreign;
        assert(foreign.replaceMounts(*leases, {}).error() == EMountUpdateError::UNKNOWN_MOUNT);
        assert(leases->front().id() == original && vfs.mountCount() == 2);
        descriptors[1].root = "/B";
        std::atomic<bool> done{};
        std::atomic<unsigned> reads{};
        std::thread reader(
            [&]
            {
                do
                {
                    std::array<std::uint32_t, 2> revisions{};
                    std::size_t count = 0;
                    vfs.enumerate(
                        [&](const ProviderEntry& entry)
                        {
                            assert(count < revisions.size());
                            revisions[count++] = entry.magic_number;
                        }
                    );
                    assert(count == 2 && revisions[0] == revisions[1]);
                    reads.fetch_add(1);
                    reads.notify_one();
                } while (!done.load());
            }
        );
        reads.wait(0);
        for (std::uint32_t revision = 2; revision != 502; ++revision)
        {
            descriptors[0].provider = std::make_shared<Provider>(First, revision);
            descriptors[1].provider = std::make_shared<Provider>(Second, revision);
            auto replacement = vfs.replaceMounts(*leases, descriptors);
            assert(replacement && vfs.mountCount() == 2);
            assert(leases->front().id() == kInvalidMountId && leases->back().id() == kInvalidMountId);
            *leases = std::move(*replacement);
        }
        done.store(true);
        reader.join();
        assert(reads.load() != 0);
        frozen.enumerate([](const ProviderEntry& entry) { assert(entry.magic_number == 1); });
        auto empty = vfs.replaceMounts(*leases, {});
        assert(empty && empty->empty() && vfs.mountCount() == 0);
        assert(vfs.replaceMounts(*leases, {}).error() == EMountUpdateError::UNKNOWN_MOUNT);
    }

    void cleanupReentry()
    {
        AssetVfs vfs;
        auto provider = std::make_shared<Provider>();
        int revoked = 0;
        provider->on_destroy = [&]
        {
            assert(vfs.mountCount() == 0);
            auto nested = vfs.mount({"/Nested", std::make_shared<Provider>()});
            assert(nested && vfs.mountCount() == 1);
            ++revoked;
        };
        auto lease = vfs.mount({"/Game", std::move(provider)});
        assert(lease);
        *lease = {};
        assert(revoked == 1 && vfs.mountCount() == 0);

        provider = std::make_shared<Provider>();
        provider->on_destroy = [&]
        {
            assert(vfs.mountCount() == 0);
            auto nested = vfs.mount({"/Nested", std::make_shared<Provider>()});
            assert(nested);
            ++revoked;
        };
        lease = vfs.mount({"/Game", std::move(provider)});
        assert(lease);
        auto empty = vfs.replaceMounts(std::span(&*lease, 1), {});
        assert(empty && revoked == 2 && lease->id() == kInvalidMountId);
    }
} // namespace

int main()
{
    lifetime();
    acceptedRead();
    batch();
    cleanupReentry();
    std::puts(
        "PASS mount lease lifetime, source death, moves, captured/active reads, atomic replacement and cleanup reentry"
    );
}
