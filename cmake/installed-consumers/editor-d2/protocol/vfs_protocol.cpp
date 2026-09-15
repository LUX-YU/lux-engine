#include <atomic>
#include <cassert>
#include <cstdio>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <semaphore>
#include <thread>

using namespace lux::asset;
AssetId identity()
{
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = 42;
    return AssetId{bytes};
}
struct ReadGate final
{
    std::binary_semaphore entered{0}, released{0};
    std::atomic<bool> first{true};
};
class Provider final : public IAssetProvider
{
  public:
    Provider(std::string value, std::shared_ptr<ReadGate> gate = {})
        : value_(std::make_shared<const std::string>(std::move(value))), gate_(std::move(gate))
    {
    }
    std::optional<AssetId> resolve(std::string_view path) const override
    {
        return path == "Entry" ? std::optional{identity()} : std::nullopt;
    }
    bool contains(const AssetId &id) const override
    {
        return id == identity();
    }
    lux::cxx::expected<AssetBlob, EAssetStorageError> open(const AssetId &id) const override
    {
        assert(id == identity());
        if (gate_ && gate_->first.exchange(false))
        {
            gate_->entered.release();
            gate_->released.acquire();
        }
        return AssetBlob{lux::cxx::SharedBytes<>::fromOwner(value_, std::as_bytes(std::span(*value_)))};
    }
    void enumerate(const std::function<void(const ProviderEntry &)> &callback) const override
    {
        callback({identity(), 7, "Entry", false});
    }
    std::optional<std::string> pathOf(const AssetId &id) const override
    {
        return contains(id) ? std::optional<std::string>{"Entry"} : std::nullopt;
    }

  private:
    std::shared_ptr<const std::string> value_;
    std::shared_ptr<ReadGate> gate_;
};
std::string content(const AssetBlob &blob)
{
    return {reinterpret_cast<const char *>(blob.bytes.view().data()), blob.bytes.size()};
}
int main()
{
    AssetVfs vfs;
    auto gate = std::make_shared<ReadGate>();
    auto first = std::make_shared<Provider>("old", gate);
    std::weak_ptr<Provider> weak = first;
    const auto id = vfs.mount({"/Project", first, 0});
    assert(id);
    first.reset();
    const auto view = vfs.view();
    AssetBlob old;
    std::jthread reader(
        [&]
        {
            auto read = view.open(identity());
            assert(read);
            old = std::move(*read);
        });
    gate->entered.acquire();

    auto second = std::make_shared<Provider>("new");
    MountDesc invalid{"/Project/invalid", second, 0};
    auto rejected = vfs.replaceMounts({&id, 1}, {&invalid, 1});
    assert(!rejected && rejected.error() == EMountUpdateError::INVALID_DESCRIPTOR);
    assert(vfs.mountCount() == 1 && content(*view.open(identity())) == "old");
    MountDesc valid{"/Project", second, 0};
    auto replaced = vfs.replaceMounts({&id, 1}, {&valid, 1});
    assert(replaced && replaced->size() == 1 && replaced->front() > id);
    assert(vfs.mountCount() == 1 && content(*view.open(identity())) == "new");
    assert(!weak.expired());
    gate->released.release();
    reader.join();
    assert(weak.expired() && content(old) == "old");

    const MountId duplicate[]{replaced->front(), replaced->front()};
    auto duplicate_result = vfs.replaceMounts(duplicate, {});
    assert(!duplicate_result && duplicate_result.error() == EMountUpdateError::DUPLICATE_MOUNT);
    auto stale = vfs.replaceMounts({&id, 1}, {});
    assert(!stale && stale.error() == EMountUpdateError::UNKNOWN_MOUNT);
    assert(content(*view.open(identity())) == "new" && vfs.mountCount() == 1);
    assert(vfs.replaceMounts(*replaced, {}) && vfs.mountCount() == 0);
    assert(!view.open(identity()));
    std::puts("PASS VFS atomic replacement: invalid/duplicate/stale retain table; in-flight old provider remains "
              "alive; new reads use new provider; old bytes outlive retirement");
}
