#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

#include <array>
#include <atomic>
#include <cassert>
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

    class Provider final : public lux::asset::IAssetProvider
    {
    public:
        Provider(lux::asset::AssetId id, std::string path) : id_(id), path_(std::move(path))
        {
        }

        [[nodiscard]] std::optional<lux::asset::AssetId> resolve(std::string_view path) const override
        {
            return path == path_ ? std::optional{id_} : std::nullopt;
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
            const std::array bytes{std::byte{0x4CU}};
            return lux::asset::AssetBlob::fromShared(lux::cxx::SharedBytes<>::copyOf(bytes));
        }

        void enumerate(const std::function<void(const lux::asset::ProviderEntry&)>& fn) const override
        {
            fn(lux::asset::ProviderEntry{id_, 1U, path_, false});
        }

        [[nodiscard]] std::optional<std::string> pathOf(const lux::asset::AssetId& id) const override
        {
            return id == id_ ? std::optional{path_} : std::nullopt;
        }

    private:
        lux::asset::AssetId id_;
        std::string path_;
    };

    class BlockingProvider final : public lux::asset::IAssetProvider
    {
    public:
        explicit BlockingProvider(lux::asset::AssetId id) : id_(id)
        {
        }

        [[nodiscard]] std::optional<lux::asset::AssetId> resolve(std::string_view path) const override
        {
            return path == "blocked" ? std::optional{id_} : std::nullopt;
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
            entered.store(true, std::memory_order_release);
            entered.notify_all();
            release.wait(false, std::memory_order_acquire);
            const std::array bytes{std::byte{0x42U}};
            return lux::asset::AssetBlob::fromShared(lux::cxx::SharedBytes<>::copyOf(bytes));
        }

        void enumerate(const std::function<void(const lux::asset::ProviderEntry&)>& fn) const override
        {
            fn(lux::asset::ProviderEntry{id_, 1U, "blocked", false});
        }

        [[nodiscard]] std::optional<std::string> pathOf(const lux::asset::AssetId& id) const override
        {
            return id == id_ ? std::optional<std::string>{"blocked"} : std::nullopt;
        }

        mutable std::atomic_bool entered{};
        mutable std::atomic_bool release{};

    private:
        lux::asset::AssetId id_;
    };
}

int main()
{
    using namespace lux::asset;

    AssetVfs vfs;
    const auto base_id = makeId(1U);
    const auto patch_id = makeId(2U);
    auto base = std::make_shared<Provider>(base_id, "item");
    assert(vfs.mount({"/Game", base, 0}) != kInvalidMountId);
    const auto view = vfs.view();
    assert(view && view.resolve("/Game/item") == base_id);

    std::atomic_bool running{true};
    std::array<std::jthread, 4U> readers;
    for (auto& reader : readers)
    {
        reader = std::jthread([&] {
            while (running.load(std::memory_order_acquire))
            {
                const auto resolved = view.resolve("/Game/item");
                assert(resolved == base_id || resolved == patch_id);
                const auto opened = view.open(resolved);
                if (opened)
                    assert(opened->bytes.size() == 1U);
                static_cast<void>(view.pathOf(resolved));
                std::size_t count{};
                view.enumerate([&](const ProviderEntry&) { ++count; });
                assert(count == 1U);
            }
        });
    }

    for (std::size_t iteration{}; iteration < 200U; ++iteration)
    {
        auto patch = std::make_shared<Provider>(patch_id, "item");
        const auto mount = vfs.mount({"/Game", std::move(patch), 10});
        assert(mount != kInvalidMountId);
        vfs.unmount(mount);
    }
    running.store(false, std::memory_order_release);
    for (auto& reader : readers)
        reader.join();

    const auto blocked_id = makeId(3U);
    auto blocking = std::make_shared<BlockingProvider>(blocked_id);
    const std::weak_ptr<BlockingProvider> lifetime = blocking;
    const auto blocked_mount = vfs.mount({"/Game", blocking, 20});
    assert(blocked_mount != kInvalidMountId);
    bool read_succeeded{};
    std::jthread in_flight([&] { read_succeeded = static_cast<bool>(view.open(blocked_id)); });
    blocking->entered.wait(false, std::memory_order_acquire);
    vfs.unmount(blocked_mount);
    blocking.reset();
    assert(!lifetime.expired());
    if (auto retained = lifetime.lock())
    {
        retained->release.store(true, std::memory_order_release);
        retained->release.notify_all();
    }
    in_flight.join();
    assert(read_succeeded);
    assert(lifetime.expired());
    return 0;
}
