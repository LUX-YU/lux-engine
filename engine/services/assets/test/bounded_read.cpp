#include "AssetTest.hpp"
#include <fstream>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>

int main(int argc, char** argv)
{
    using namespace asset_test;
    assert(argc == 2);
    const auto root = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(root);
    const auto file = root / "bounded.luxpak";
    const auto payload = image().bytes;
    assert(asset::writePakFile(file, {{Id, 1, "mesh", {}, payload}}));
    auto provider = asset::PakAssetProvider::loadFromFile(file);
    assert(provider);
    asset::AssetVfs vfs;
    auto mount = vfs.mount({"/Game", *provider});
    assert(mount);
    auto created = execution::ExecutionRuntime::create({1, 64, {32}, execution::BlockingSchedulerConfig{1, 32}});
    assert(created);
    auto execution = std::move(*created);
    auto limited = AssetRepository::create(vfs.view(), {payload.size() - 1, 2048, 8}, {}, codecs());
    assert(limited);
    auto rejected = wait<AssetDataRef<std::thread::id>>(*execution, read<asset::DecodeProbe>(**limited, Id));
    assert(!rejected);
    assert(rejected.error().domainFailure()->type == kAssetReadFailure);
    assert(
        rejected.error().domainFailure()->args[0] ==
        static_cast<std::uint64_t>(asset::EAssetStorageError::LIMIT_EXCEEDED)
    );
    assert(asset::DecodeProbe::decodes == 0);
    (*limited).reset();
    assert(!*limited);

    auto repository = AssetRepository::create(vfs.view(), {payload.size(), 2048, 8}, {}, codecs());
    assert(repository);
    auto loaded = wait<AssetDataRef<std::thread::id>>(*execution, read<asset::DecodeProbe>(**repository, Id));
    assert(loaded);
    *loaded = {};
    (*repository).reset();
    assert(!*repository);
    const auto inspection = asset::inspectPak(file);
    assert(inspection);
    {
        std::fstream stream(file, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(static_cast<std::streamoff>(inspection->entries.front().offset));
        stream.put('\x7f');
    }
    assert((*provider)->open(Id, 0).error() == asset::EAssetStorageError::LIMIT_EXCEEDED);
    assert((*provider)->open(Id, payload.size()).error() == asset::EAssetStorageError::CORRUPT_IMAGE);
}
