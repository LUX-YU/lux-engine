#pragma once

#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/world/WorldAssetCodec.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/process/world_loading/WorldStorageSource.hpp>
#include <lux/engine/world/WorldPartitionData.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/simulation/ecs/ComponentSchema.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
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
        lux::asset::PakDecodedImage package;
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
        CANCELLED,
        ENCODE,
        INDEX_REBUILD_REQUIRED
    };

    struct NativeSceneFailure final
    {
        ENativeSceneError code{};
        lux::asset::AssetId asset;
        std::size_t ordinal{};
        std::variant<std::monostate, std::string, lux::asset::AssetDecodeFailure,
                     lux::process::world_loading::WorldStorageRuntimeFailure,
                     lux::asset::AssetEncodeFailure, lux::world::WorldStorageCodecFailure,
                     lux::world::WorldDescriptionFailure, lux::serialization::SerializationFailure>
            cause;
    };

    [[nodiscard]] lux::cxx::expected<NativeScene, NativeSceneFailure> decodeNativeScene(const lux::cxx::SharedBytes<> &,
                                                                                        std::stop_token = {});

    struct CapturedComponent final
    {
        lux::world::WorldObjectId object;
        std::uint32_t schema;
        std::uint32_t version;
        lux::simulation::ecs::ComponentCapture value;
    };

    struct SceneCapture final
    {
        struct Object final
        {
            lux::world::WorldObjectId id;
            lux::partition::PartitionOrdinal partition;
        };
        std::shared_ptr<const NativeScene> source;
        lux::simulation::ecs::WorldEntityMap identities;
        std::vector<CapturedComponent> components;
        std::vector<Object> objects;
        bool structure_changed{};
        // Complete sorted schema directory for this capture; existing unknown schemas remain present.
        std::vector<lux::world::WorldDataSchemaId> schemas;
    };

    // CPU-only preparation over a frozen author capture. The original package remains unchanged.
    [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, NativeSceneFailure>
    encodeNativeScene(const SceneCapture&, std::size_t max_bytes, std::stop_token = {});
} // namespace lux::editor::scene
