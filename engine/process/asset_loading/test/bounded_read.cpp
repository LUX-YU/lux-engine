#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/process/asset_loading/AssetReadOverlay.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <cassert>
#include <fstream>

using namespace lux;
using namespace lux::process::asset_loading;

int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto root = std::filesystem::path(argv[1]) /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    const asset::AssetId id{*uuids::uuid::from_string("fedcba98-7654-3210-fedc-ba9876543210")};
    const auto image = cxx::SharedBytes<>::copyOf(std::as_bytes(std::span("bounded payload")));
    const auto path = root / "bounded.luxpak";
    assert(asset::writePakFile(path, {{id, 1, "payload", {}, image}}));
    auto provider = asset::PakAssetProvider::loadFromFile(path);
    assert(provider);
    assert((*provider)->open(id, image.size())->bytes.size() == image.size());
    assert((*provider)->open(id, image.size() - 1).error() == asset::EAssetStorageError::LIMIT_EXCEEDED);
    asset::AssetVfs vfs;
    assert(vfs.mount({"/Game", *provider}) != asset::kInvalidMountId);
    auto runtime = process::ExecutionRuntime::create({1, 64, 64, {32}, process::BlockingSchedulerConfig{1, 64}});
    assert(runtime);
    process::TaskScope tasks{*runtime};
    auto blocking = runtime->blocking();
    assert(blocking);
    auto endpoint = VfsAssetReadEndpoint::create(vfs.view().capture(), *blocking, tasks, {2});
    assert(endpoint);
    struct Reply final
    {
        std::optional<AssetReadPort::Outcome> result;
        static void complete(void* target, AssetReadPort::Outcome&& value) noexcept
        {
            static_cast<Reply*>(target)->result = std::move(value);
        }
    };
    auto request = [&](AssetReadPort port, std::size_t bound) {
        Reply reply;
        assert(port.submit({id, bound}, &reply, &Reply::complete, {}));
        assert(runtime->waitUntil([&]() noexcept { return reply.result.has_value(); }));
        return std::move(*reply.result);
    };
    auto port = (*endpoint)->port();
    assert(request(port, image.size()));
    const auto limited = request(port, image.size() - 1);
    assert(!limited && limited.error().domainError() == asset::EAssetStorageError::LIMIT_EXCEEDED);
    auto overlay = makeAssetReadOverlay({{id, {image}}}, port);
    assert(overlay);
    assert(request(*overlay, image.size()));
    const auto overlay_limited = request(*overlay, 0);
    assert(!overlay_limited && overlay_limited.error().domainError() == asset::EAssetStorageError::LIMIT_EXCEEDED);
    // Metadata remains legal; a payload read would detect this corruption. The size rejection
    // precedes payload access/allocation, while an admitted read still validates the original digest.
    const auto inspection = asset::inspectPak(path);
    assert(inspection && inspection->entries.size() == 1);
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(static_cast<std::streamoff>(inspection->entries[0].offset));
        file.put('\x7f');
    }
    assert((*provider)->open(id, 0).error() == asset::EAssetStorageError::LIMIT_EXCEEDED);
    assert((*provider)->open(id, image.size()).error() == asset::EAssetStorageError::CORRUPT_IMAGE);
    (*endpoint)->requestStop();
    assert(tasks.join());
    assert((*endpoint)->join());
    runtime->requestStop();
    assert(runtime->join());
}
