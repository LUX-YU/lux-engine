#pragma once

#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/world/WorldAssetCodec.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/process/world_loading/WorldStorageSource.hpp>
#include <lux/engine/world/WorldPartitionData.hpp>
#include <variant>
#include <stop_token>

namespace lux::editor::scene
{
    // Immutable author source. Runtime Registry and derived GPU state are separate owners.
    struct NativeScene final
    {
        std::shared_ptr<const lux::scene::SceneAsset> scene;
        std::shared_ptr<const lux::world::WorldAsset> world;
        std::shared_ptr<const lux::simulation::SimulationAsset> simulation;
        std::vector<lux::cxx::SharedBytes<>> volumes;
        std::vector<lux::world::WorldPartitionData> partitions;
    };

    enum class ENativeSceneError : std::uint8_t
    {
        PACKAGE,
        MISSING_ASSET,
        TYPE_MISMATCH,
        DECODE,
        MISSING_VOLUME,
        VOLUME_SIZE,
        LIMIT,
        STORAGE,
        CANCELLED
    };

    struct NativeSceneFailure final
    {
        ENativeSceneError code{};
        lux::asset::AssetId asset;
        std::size_t ordinal{};
        std::variant<std::monostate, std::string, lux::asset::AssetDecodeFailure,
                     lux::process::world_loading::WorldStorageRuntimeFailure>
            cause;
    };

    [[nodiscard]] lux::cxx::expected<NativeScene, NativeSceneFailure> decodeNativeScene(const lux::cxx::SharedBytes<> &,
                                                                                        std::stop_token = {});
} // namespace lux::editor::scene
