#pragma once
#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/asset/visibility.h>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/world/WorldAssetCodec.hpp>
#include <lux/engine/world/WorldPartitionData.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/serialization/SerializationError.hpp>
#include <stop_token>
#include <variant>

namespace lux::scene
{
    struct ScenePackage final
    {
        std::shared_ptr<const lux::scene::SceneAsset> scene;
        std::shared_ptr<const lux::world::WorldAsset> world;
        std::shared_ptr<const lux::simulation::SimulationAsset> simulation;
        std::vector<lux::cxx::SharedBytes<>> volumes;
        std::vector<std::shared_ptr<const lux::world::WorldPartitionData>> partitions;
        lux::asset::PakDecodedImage package;
    };
    enum class EScenePackageError : std::uint8_t
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
        INDEX_REBUILD_REQUIRED,
        INVALID_ARGUMENT
    };

    struct ScenePackageFailure final
    {
        EScenePackageError code{};
        lux::asset::AssetId asset;
        std::size_t ordinal{};
        std::variant<
            std::monostate,
            std::string,
            lux::asset::AssetDecodeFailure,
            lux::asset::AssetEncodeFailure,
            lux::world::WorldStorageCodecFailure,
            lux::world::WorldDescriptionFailure,
            lux::serialization::SerializationFailure,
            SceneDescriptionFailure>
            cause;
        std::string stage;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC lux::cxx::expected<ScenePackage, ScenePackageFailure>
    decodeScenePackage(const lux::cxx::SharedBytes<>&, std::stop_token = {}, bool retain_partition_sources = true);

    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC lux::cxx::expected<std::vector<std::byte>, ScenePackageFailure>
    encodeScenePackage(const ScenePackage&, std::size_t max_bytes, std::stop_token = {});

    // Clone the package envelope; author object and partition identities remain stable.
    // Unknown root references and indexed layouts are rejected before any IO.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC lux::cxx::expected<ScenePackage, ScenePackageFailure> copyScenePackage(
        const ScenePackage&,
        asset::AssetId,
        std::stop_token = {}
    );

    // Pure single-partition construction; systems and their explicit bindings are preserved.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC lux::cxx::expected<ScenePackage, ScenePackageFailure>
    createScenePackage(asset::AssetId, std::string_view, std::span<const world::WorldDataSchemaId>, std::shared_ptr<const simulation::SimulationDescription>, const SceneDescription&) noexcept;

    // Rebuild existing content without changing its root or bundle identities.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC lux::cxx::expected<ScenePackage, ScenePackageFailure>
    assembleScenePackage(
        const ScenePackage&,
        std::span<const std::shared_ptr<const world::WorldPartitionData>>,
        std::stop_token = {}
    ) noexcept;

    // Assemble a new identity domain. Callers must reject extensions with unknown identity references.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC lux::cxx::expected<ScenePackage, ScenePackageFailure>
    assembleScenePackage(
        asset::AssetId,
        std::string_view,
        std::span<const world::WorldDataSchemaId>,
        world::WorldPartitionerDescriptor,
        std::span<const std::shared_ptr<const world::WorldPartitionData>>,
        std::shared_ptr<const simulation::SimulationDescription>,
        const SceneDescription&,
        std::stop_token = {}
    ) noexcept;
}
