#include "AssetTest.hpp"
#if defined(LUX_ASSET_FILEWATCH)
#include <lux/engine/filewatch/FileWatcher.hpp>
#endif

int main()
{
    using namespace asset_test;
    auto created = execution::ExecutionRuntime::create({1, 64, {32}, execution::BlockingSchedulerConfig{1, 32}});
    assert(created);
    auto execution = std::move(*created);
    asset::AssetVfs vfs;
    auto provider = std::make_shared<Provider>();
    Provider alternate;
    const asset::AssetId material_id{*uuids::uuid::from_string("fedcba98-7654-3210-fedc-ba9876543211")};
    provider->images.emplace(Id, image());
    provider->images.emplace(material_id, image());
    auto mount = vfs.mount({"/Game", provider});
    assert(mount);
    auto repository = AssetRepository::create(vfs.view(), {1024, 2048, 8}, {}, codecs());
    assert(repository);
    AssetDataRef<std::thread::id> first, second;
    unsigned completions = 0;
    auto receive = [&](AssetDataRef<std::thread::id>& target)
    {
        return [&](auto result) noexcept
        {
            assert(result && execution->isMainThread());
            target = std::move(*result);
            ++completions;
        };
    };
    assert(
        execution->spawn(stdexec::then(execution::asResult(read<asset::DecodeProbe>(**repository, Id)), receive(first)))
    );
    assert(execution->spawn(
        stdexec::then(execution::asResult(read<asset::DecodeProbe>(**repository, "/Game/mesh")), receive(second))
    ));
    assert(execution->waitUntil([&]() noexcept { return completions == 2; }));
    assert(&*first == &*second && provider->reads == 1 && asset::DecodeProbe::decodes == 1);
    assert(*first != std::this_thread::get_id());
    assert(provider->read_thread != std::this_thread::get_id());
    assert((*repository)->usage(Id)->data_readers == 2);
    {
        auto& assets = **repository;
        auto texture = assets.acquire<asset::DecodeProbe>(Id);
        auto material = assets.acquire<asset::DecodeProbe>(material_id);
        assert(texture && material);
        auto texture_connection = assets.observe<&Provider::contentChanged>(*texture, *provider);
        auto material_connection = assets.observe<&Provider::contentChanged>(*material, *provider);
        const auto references = assets.usage(Id)->references;
        {
            auto copy = *texture;
            assert(assets.usage(Id)->references == references + 1);
        }
        assert(assets.usage(Id)->references == references);
        assert(assets.revision(*texture) == 0 && assets.revision(first) == 0 && assets.isCurrent(first));
        const auto original_data = *first;
        const auto replacement = std::this_thread::get_id();
        assert(wait<void>(*execution, assets.write(*texture, replacement)));
        assert(provider->notifications == std::vector{Id});
        assert(assets.revision(*texture) == 1 && !assets.isCurrent(first) && *first == original_data);
        auto current = wait<AssetDataRef<std::thread::id>>(*execution, assets.read(*texture));
        assert(current && assets.isCurrent(*current) && assets.revision(*current) == 1);
        provider->fail_write = true;
        auto rejected = wait<void>(*execution, assets.write(*texture, replacement));
        assert(!rejected && assets.revision(*texture) == 1 && assets.isCurrent(*current));
        assert(provider->notifications.size() == 1);
        provider->fail_write = false;
        provider->unknown_write = true;
        auto unknown = wait<void>(*execution, assets.write(*texture, replacement));
        assert(!unknown && unknown.error().domainFailure()->type == kAssetWriteFailure);
        assert(unknown.error().domainFailure()->args[0] ==
               static_cast<std::uint64_t>(asset::EAssetStorageError::PUBLICATION_UNKNOWN));
        assert(assets.revision(*texture) == 2 && !assets.isCurrent(*current));
        assert(provider->notifications.size() == 2);
        provider->unknown_write = false;
        assert(assets.registerUse<ProbeCodec>(*provider));
        assert(assets.registerUse<ProbeCodec>(alternate));
        auto texture_use = wait<AssetDataRef<std::uint32_t>>(*execution, assets.use<ProbeCodec>(*texture, *provider));
        assert(texture_use && assets.isCurrent(*texture_use));
        provider->preparation = [&](AssetRepository&, std::vector<AssetDataRef<void>>& dependencies)
            noexcept -> exec::task<AssetResult<void>>
        {
            dependencies.emplace_back(*texture_use);
            co_return AssetResult<void>{};
        };
        auto material_use = wait<AssetDataRef<std::uint32_t>>(*execution, assets.use<ProbeCodec>(*material, *provider));
        assert(material_use && assets.isCurrent(*material_use));
        assert(provider->uploads == 2);
        const auto material_revision = assets.revision(*material);
        provider->notifications.clear();
        assert(wait<void>(*execution, assets.write(*texture, replacement)));
        assert(provider->notifications == std::vector({Id, material_id}));
        assert(!assets.isCurrent(*texture_use) && !assets.isCurrent(*material_use));
        assert(assets.revision(*material) == material_revision);
        assert(provider->releases.empty()); // Existing use references still own the obsolete outputs.
        provider->preparation = {};
        auto next_texture = wait<AssetDataRef<std::uint32_t>>(*execution, assets.use<ProbeCodec>(*texture, *provider));
        assert(next_texture && **next_texture != **texture_use);
        provider->preparation = [&](AssetRepository&, std::vector<AssetDataRef<void>>& dependencies)
            noexcept -> exec::task<AssetResult<void>>
        {
            dependencies.emplace_back(*next_texture);
            co_return AssetResult<void>{};
        };
        auto next_material = wait<AssetDataRef<std::uint32_t>>(
            *execution, assets.use<ProbeCodec>(*material, *provider)
        );
        assert(next_material && **next_material != **material_use && assets.isCurrent(*next_material));

        // Existing-result dependencies cannot introduce an asset-level cycle, even across Providers.
        alternate.preparation = [&](AssetRepository&, std::vector<AssetDataRef<void>>& dependencies)
            noexcept -> exec::task<AssetResult<void>>
        {
            dependencies.emplace_back(*next_material);
            co_return AssetResult<void>{};
        };
        auto cycle = wait<AssetDataRef<std::uint32_t>>(*execution, assets.use<ProbeCodec>(*texture, alternate));
        assert(!cycle && alternate.uploads == 1);
        alternate.preparation = {};

        // The dependency changes before its reverse edge can be published. Qualification must still fail.
        assert(wait<void>(*execution, assets.write(*material, replacement)));
        provider->preparation = [&](AssetRepository& repository, std::vector<AssetDataRef<void>>& dependencies)
            noexcept -> exec::task<AssetResult<void>>
        {
            dependencies.emplace_back(*next_texture);
            co_return co_await repository.write(*texture, replacement);
        };
        auto stale = wait<AssetDataRef<std::uint32_t>>(*execution, assets.use<ProbeCodec>(*material, *provider));
        assert(!stale && stale.error().domainFailure()->type == kAssetChanged);
        provider->preparation = {};
#if defined(LUX_ASSET_FILEWATCH)
        provider->notifications.clear();
        const auto previous_revision = assets.revision(*texture);
        provider->images[Id] = image(std::byte{0xFF});
        provider->refreshed = {Id};
        const std::array events{platform::FileEvent{"mesh", {}, platform::EFileEvent::FILE_MODIFIED}};
        assert(wait<void>(*execution, assets.refresh(events)));
        assert(assets.revision(*texture) == previous_revision + 1);
        assert(!provider->notifications.empty() && provider->notifications.front() == Id);
        auto invalid_content = wait<AssetDataRef<std::thread::id>>(*execution, assets.read(*texture));
        assert(!invalid_content && invalid_content.error().domainFailure()->type == kAssetDecodeFailure);
        assert(*first == original_data); // Notification is invalidation, not a claim that new bytes decode.
        provider->images[Id] = image();
        provider->refreshed = {Id};
        assert(wait<void>(*execution, assets.refresh(events)));
        assert(wait<AssetDataRef<std::thread::id>>(*execution, assets.read(*texture)));
#endif
        texture_connection.release();
        material_connection.release();
        provider->notifications.clear();
        assert(wait<void>(*execution, assets.write(*texture, replacement)));
        assert(provider->notifications.empty());
    }
    first = {};
    std::thread release([reference = std::move(second)]() mutable { reference = {}; });
    release.join();
    (*repository).reset();
    assert(!*repository);
    assert(provider->releases.size() == provider->uploads);
    assert(alternate.releases.size() == alternate.uploads);
}
