#pragma once

#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/engine/process/world_loading/WorldStorageSource.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/ecs/ComponentSchema.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <lux/engine/world/WorldAssetCodec.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldPartitionData.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>
#include <stop_token>
#include <variant>

namespace lux::process
{
class TaskScope;
}
namespace lux::render
{
class RenderRuntime;
}
namespace lux::scene
{
class RenderAssetSource;
}

namespace lux::editor
{
class Project;
}

namespace lux::editor::scene
{
struct SceneEditorMetadata;

// Immutable author source. Runtime Registry and derived GPU state are separate owners.
struct NativeScene final
{
    std::shared_ptr<const lux::scene::SceneAsset> scene;
    std::shared_ptr<const lux::world::WorldAsset> world;
    std::shared_ptr<const lux::simulation::SimulationAsset> simulation;
    std::vector<lux::cxx::SharedBytes<>> volumes;
    std::vector<std::shared_ptr<const lux::world::WorldPartitionData>> partitions;
    lux::asset::PakDecodedImage package;

    [[nodiscard]] lux::cxx::expected<lux::process::world_loading::WorldStorageSource,
                                     lux::process::world_loading::WorldStorageRuntimeFailure>
    storageSource() const;
};

namespace detail
{
class SceneAssetSources final
{
  public:
    explicit SceneAssetSources(lux::render::RenderRuntime &runtime) noexcept : runtime_(runtime)
    {
    }
    [[nodiscard]] EditorResult<std::shared_ptr<lux::scene::RenderAssetSource>> acquire(Project &);

  private:
    struct Record final
    {
        std::uint64_t project{}, revision{};
        std::weak_ptr<lux::scene::RenderAssetSource> source;
    };
    lux::render::RenderRuntime &runtime_;
    std::vector<Record> records_;
};

[[nodiscard]] EditorResult<std::shared_ptr<const lux::scene::SceneDescription>> editorSceneDescription(
    const NativeScene &, const SceneEditorMetadata &, bool author_view = true);
[[nodiscard]] EditorResult<std::unique_ptr<lux::scene::SceneInstance>> instantiateNativeScene(
    const NativeScene &, const SceneEditorMetadata &, process::TaskScope &, lux::render::RenderRuntime &,
    std::shared_ptr<lux::scene::RenderAssetSource>, lux::simulation::ESimulationMode,
    std::chrono::nanoseconds fixed_step = std::chrono::milliseconds(16));
} // namespace detail

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
                 lux::process::world_loading::WorldStorageRuntimeFailure, lux::asset::AssetEncodeFailure,
                 lux::world::WorldStorageCodecFailure, lux::world::WorldDescriptionFailure,
                 lux::serialization::SerializationFailure>
        cause;
};

[[nodiscard]] lux::cxx::expected<NativeScene, NativeSceneFailure> decodeNativeScene(
    const lux::cxx::SharedBytes<> &, std::stop_token = {}, bool retain_partition_sources = true);

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
[[nodiscard]] lux::cxx::expected<std::vector<std::byte>, NativeSceneFailure> encodeNativeScene(const SceneCapture &,
                                                                                               std::size_t max_bytes,
                                                                                               std::stop_token = {});
} // namespace lux::editor::scene
