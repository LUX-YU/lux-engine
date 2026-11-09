#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/core/execution/ExecutionResult.hpp>
#include <lux/engine/EngineMetaFunctionRegistry.hpp>
#include <lux/engine/resource/asset/storage/directory/DirectoryAssetProvider.hpp>
#include <lux/engine/scene/SceneContent.hpp>
#include <lux/engine/scene/ScenePersistence.hpp>
#include <lux/engine/toolchain/scene/SceneStreamingBuild.hpp>

#include <cassert>
#include <algorithm>
#include <filesystem>
#include <latch>
#include <optional>
#include <semaphore>

namespace
{
    using namespace lux;

    template <class Sender> auto wait(execution::ExecutionRuntime& execution, Sender sender)
    {
        std::optional<typename execution::detail::ExecutionTypes<Sender>::Result> result;
        assert(execution.spawn(stdexec::then(
            execution::asResult(stdexec::starts_on(execution.mainScheduler(), std::move(sender))),
            [&](auto value) noexcept { result.emplace(std::move(value)); }
        )));
        assert(execution.waitUntil([&]() noexcept { return result.has_value(); }));
        return std::move(*result);
    }
}

int main(int argc, char** argv)
{
    using namespace lux;
    assert(argc == 2);
    constexpr std::size_t limit = 4U << 20U;
    // CTest supplies an exclusive directory; never delete the supplied parent or other test data.
    const auto root = std::filesystem::absolute(argv[1]) / "scene-block-parity";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto provider = asset::DirectoryAssetProvider::create(root);
    assert(provider);
    asset::AssetVfs vfs;
    auto mounted = vfs.mount({"/Scenes", *provider});
    assert(mounted);
    auto execution = execution::ExecutionRuntime::create({2, 32, {8}, execution::BlockingSchedulerConfig{1, 32}});
    assert(execution);
    std::vector<std::unique_ptr<asset::AssetCodec>> codecs;
    codecs.push_back(scene::createSceneCodec());
    auto repository = engine::assets::AssetRepository::create(vfs.view(), {limit, limit, 8}, {}, std::move(codecs));
    assert(repository);
    auto& assets = **repository;
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("adc42d2b-728d-4298-9476-917d6830ee62")};
    const world::WorldObjectId object{ids("object")};
    const std::vector<scene::SceneObjectData> objects{
        {object, {{ecs::componentSchemaId("test.opaque"), 1, {std::byte{3}, std::byte{7}}}}}
    };
    const std::vector<scene::SceneObjectSummary> summaries{{object, {}}};
    scene::SceneBlockDescription block;
    std::vector<std::byte> encoded;
    assert(scene::encodeSceneBlock(objects, summaries, encoded, block, limit));
    const auto decoded = scene::decodeSceneBlock(encoded, block, limit);
    assert(decoded && decoded->size() == 1 && decoded->front().id == object);
    assert(decoded->front().components.front().payload == objects.front().components.front().payload);
    std::vector<std::byte> roundtrip;
    scene::SceneBlockDescription roundtrip_block;
    assert(scene::encodeSceneBlock(*decoded, summaries, roundtrip, roundtrip_block, limit));
    assert(roundtrip == encoded && roundtrip_block.digest == block.digest);
    assert(!scene::decodeSceneBlock(encoded, block, encoded.size() - 1));
    for (std::size_t size = 0; size < encoded.size(); ++size)
    {
        assert(!scene::decodeSceneBlock(std::span(encoded).first(size), block, limit));
    }
    auto corrupt = encoded;
    corrupt.back() ^= std::byte{1};
    assert(!scene::decodeSceneBlock(corrupt, block, limit));
    auto invalid = block;
    invalid.summary_size = UINT64_MAX;
    assert(!scene::decodeSceneBlock(encoded, invalid, limit));

    scene::SceneDescription description;
    description.world.name = "Parity";
    const std::vector<scene::SceneBlockUpdate> updates{{0, objects, summaries}};
    engine::EngineMetaFunctionRegistry metadata;
    auto streaming = wait(**execution, engine::toolchain::buildSceneStreaming(
        assets, {}, description, updates, metadata, limit
    ));
    assert(streaming);
    const asset::AssetInfo info{.id = asset::AssetId{ids("scene")}, .type = scene::SceneAsset::asset_type};
    auto asset = wait(**execution, scene::createSceneAsset(
        assets, "/Scenes/parity", info, description, updates, *streaming
    ));
    assert(asset);
    auto data = wait(**execution, assets.read(*asset));
    assert(data && (*data)->blocks.size() == 1);
    const auto stored = (*data)->blocks.front();
    auto bytes = wait(**execution, assets.readRange(*data, stored.offset, stored.size));
    assert(bytes && std::ranges::equal(bytes->view(), encoded));
    auto from_file = scene::decodeSceneBlock(bytes->view(), stored, limit);
    assert(from_file && from_file->front().id == decoded->front().id);
    assert(from_file->front().components.front().payload == decoded->front().components.front().payload);
    for (const auto [offset, size] : {std::pair{UINT64_MAX, std::uint64_t{64}},
                                    std::pair{stored.offset + stored.size, std::uint64_t{1}}})
    {
        const auto rejected = wait(**execution, assets.readRange(*data, offset, size));
        assert(!rejected && rejected.error().domainFailure()->type == engine::assets::kAssetReadFailure);
    }

    // Delay the sole IO worker. Main receives exactly one completion after the accepted range read,
    // even when the caller drops its asset/data references and the VFS mount before completion.
    std::latch occupied{1};
    std::binary_semaphore release{0};
    assert((*execution)->spawn(stdexec::then(
        execution::asResult(stdexec::schedule(*(*execution)->blockingIoScheduler())),
        [&](auto ready) noexcept { assert(ready); occupied.count_down(); release.acquire(); }
    )));
    occupied.wait();
    std::optional<execution::ExecutionResult<cxx::SharedBytes<>, error::Error>> completed;
    unsigned completions = 0;
    assert((*execution)->spawn(stdexec::then(
        execution::asResult(assets.readRange(*data, stored.offset, stored.size)),
        [&](auto value) noexcept
        {
            assert((*execution)->isMainThread());
            completed.emplace(std::move(value));
            ++completions;
        }
    )));
    assert((*execution)->dispatch());
    assert(!completed);
    const auto content = root / (*data)->content_file;
    *data = {};
    *asset = {};
    *mounted = {};
    provider->reset();
    release.release();
    assert((*execution)->waitUntil([&]() noexcept { return completed.has_value(); }));
    assert(*completed && completions == 1 && std::ranges::equal((**completed).view(), encoded));
    completed.reset();

    // Reopen the current root, then truncate its member after opening the source: the stale range must fail.
    provider = asset::DirectoryAssetProvider::create(root);
    assert(provider);
    mounted = vfs.mount({"/Scenes", *provider});
    assert(mounted);
    auto reloaded_asset = assets.acquire<scene::SceneAsset>(info.id);
    assert(reloaded_asset);
    data = wait(**execution, assets.read(*reloaded_asset));
    assert(data);
    std::filesystem::resize_file(content, stored.offset + stored.size - 1);
    const auto short_read = wait(**execution, assets.readRange(*data, stored.offset, stored.size));
    assert(!short_read && short_read.error().domainFailure()->type == engine::assets::kAssetReadFailure);
}
