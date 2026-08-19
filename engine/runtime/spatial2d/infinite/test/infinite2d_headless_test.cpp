#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/pixel/components/PixelChunk2DComponent.hpp>
#include <lux/engine/ecs/pixel/components/PixelField2DComponent.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldSystem.hpp>
#include <lux/engine/ecs/pixel/systems/PixelChunkPersistence.hpp>
#include <lux/engine/ecs/tilemap/components/TileChunk2DComponent.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapComponent.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapSystem.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/resource/asset/AssetVfs.hpp>
#include <lux/engine/scene/ScenePackageCodec.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/resource/tilemap/TilemapChunk.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionGeneratorCatalog.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/spatial2d/infinite/Infinite2DPixelContent.hpp>
#include <lux/engine/runtime/spatial2d/infinite/Infinite2DPixelPrepareService.hpp>
#include <lux/engine/runtime/spatial2d/infinite/Infinite2DPixelSystem.hpp>
#include <lux/engine/ecs/spatial2d/components/SpatialInterest2DComponent.hpp>
#include <lux/engine/runtime/spatial2d/infinite/SpatialInterest2DSystem.hpp>
#include <lux/engine/runtime/spatial2d/tilemap/TilemapChunkSystem.hpp>
#include <lux/engine/runtime/spatial2d/tilemap/TilemapPrepareService.hpp>
#include <lux/engine/runtime/spatial_partition/EntitySectionRecordStore.hpp>
#include <lux/engine/runtime/spatial_partition/SpatialPartitionSystem.hpp>

#include "Infinite2DTestHarness.hpp"

#include <lux/cxx/algorithm/hash.hpp>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    lux::runtime::spatial2d::testing::Infinite2DTestExtension*
        g_test_extension{nullptr};
    lux::runtime::spatial2d::TilemapChunkSystem*
        g_tilemap_chunk_system{nullptr};

    void tickSchedule(lux::ecs::Schedule& schedule, float dt) noexcept
    {
        if (g_test_extension)
            g_test_extension->beforeTick(schedule.world());
        schedule.tick(dt);
        if (g_test_extension)
            g_test_extension->afterTick(schedule.world());
    }

    constexpr std::string_view kPixelFieldSchema = "lux.pixel.field2d";
    constexpr std::string_view kPixelChunkSchema = "lux.pixel.chunk2d";
    constexpr std::string_view kTileChunkSchema = "lux.tilemap.chunk2d";
    constexpr std::string_view kDemandChannel = "lux.spatial2d.resident";
    constexpr lux::ecs::MaterialId kForegroundMaterial = 1u;
    constexpr lux::ecs::MaterialId kLandmarkMaterial = 2u;
    constexpr lux::ecs::MaterialId kSandMaterial = 3u;
    constexpr lux::ecs::MaterialId kWaterMaterial = 4u;
    constexpr lux::ecs::MaterialId kPlayerMaterial = 5u;

    lux::runtime::entity_scene::EntitySceneCatalog emptyCatalog()
    {
        lux::scene::ScenePackage package;
        package.id = lux::scene::ScenePackageId{
            uuids::uuid::from_string(
                "85000000-0000-4000-8000-000000000001").value()};
        auto result = lux::runtime::entity_scene::EntitySceneCatalog::create(
            std::move(package));
        assert(result);
        return std::move(*result);
    }

    uuids::uuid ordinalUuid(std::uint64_t ordinal)
    {
        std::array<std::uint8_t, 16u> bytes{};
        bytes[6] = 0x40u;
        bytes[8] = 0x80u;
        for (std::size_t index = 0u; index < 8u; ++index)
        {
            bytes[15u - index] = static_cast<std::uint8_t>(
                ordinal >> (index * 8u));
        }
        return uuids::uuid{bytes};
    }

    template<class Component>
    bool has(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return registry.all_of<Component>(entity);
    }

    template<class Component>
    void* get(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return registry.try_get<Component>(entity);
    }

    template<class Component>
    void* emplace(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return &registry.emplace_or_replace<Component>(entity);
    }

    template<class Component>
    void remove(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        static_cast<void>(registry.remove<Component>(entity));
    }

    template<class Component>
    void notify(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        if (registry.all_of<Component>(entity))
            registry.patch<Component>(entity);
    }

    template<class Component>
    void reserve(
        lux::meta::EntityRegistryBase& registry,
        std::size_t additional)
    {
        auto& storage = registry.storage<Component>();
        storage.reserve(storage.size() + additional);
    }

    template<class Component>
    void* transfer(
        lux::meta::EntityRegistryBase& source,
        entt::entity source_entity,
        lux::meta::EntityRegistryBase& destination,
        entt::entity destination_entity) noexcept
    {
        auto* value = source.try_get<Component>(source_entity);
        if (!value)
            return nullptr;
        auto* result = &destination.emplace<Component>(
            destination_entity, std::move(*value));
        static_cast<void>(source.remove<Component>(source_entity));
        return result;
    }

    struct Reflections final
    {
        lux::meta::RefClass coordinate;
        lux::meta::RefClass field;
        lux::meta::RefClass chunk;
        lux::meta::RefClass tile_chunk;

        Reflections()
        {
            coordinate.name = "GridCoord2i64";
            coordinate.full_name = "lux::spatial::GridCoord2i64";
            coordinate.hash = lux::meta::ref_type_of_v<
                lux::spatial::GridCoord2i64>.hash;
            coordinate.type = lux::meta::ref_type_of_v<
                lux::spatial::GridCoord2i64>;
            coordinate.fields = {
                lux::meta::RefField{
                    "x",
                    lux::meta::ref_type_of_v<std::int64_t>,
                    lux::meta::EVisibility::Public,
                    &coordinate,
                    static_cast<std::uint32_t>(offsetof(
                        lux::spatial::GridCoord2i64, x))},
                lux::meta::RefField{
                    "y",
                    lux::meta::ref_type_of_v<std::int64_t>,
                    lux::meta::EVisibility::Public,
                    &coordinate,
                    static_cast<std::uint32_t>(offsetof(
                        lux::spatial::GridCoord2i64, y))}};

            field.name = "PixelField2DComponent";
            field.full_name = "lux::ecs::PixelField2DComponent";
            field.hash = lux::meta::ref_type_of_v<
                lux::ecs::PixelField2DComponent>.hash;
            field.type = lux::meta::ref_type_of_v<
                lux::ecs::PixelField2DComponent>;
            field.fields = {
                lux::meta::RefField{
                    "definition",
                    lux::meta::ref_type_of_v<lux::asset::asset_id_t>,
                    lux::meta::EVisibility::Public,
                    &field,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::PixelField2DComponent, definition))},
                lux::meta::RefField{
                    "material",
                    lux::meta::ref_type_of_v<lux::asset::asset_id_t>,
                    lux::meta::EVisibility::Public,
                    &field,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::PixelField2DComponent, material))},
                lux::meta::RefField{
                    "cell_size",
                    lux::meta::ref_type_of_v<double>,
                    lux::meta::EVisibility::Public,
                    &field,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::PixelField2DComponent, cell_size))},
                lux::meta::RefField{
                    "draw_priority",
                    lux::meta::ref_type_of_v<std::int32_t>,
                    lux::meta::EVisibility::Public,
                    &field,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::PixelField2DComponent, draw_priority))},
                lux::meta::RefField{
                    "visible",
                    lux::meta::ref_type_of_v<bool>,
                    lux::meta::EVisibility::Public,
                    &field,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::PixelField2DComponent, visible))},
                lux::meta::RefField{
                    "simulation_enabled",
                    lux::meta::ref_type_of_v<bool>,
                    lux::meta::EVisibility::Public,
                    &field,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::PixelField2DComponent,
                        simulation_enabled))}};

            chunk.name = "PixelChunk2DComponent";
            chunk.full_name = "lux::ecs::PixelChunk2DComponent";
            chunk.hash = lux::meta::ref_type_of_v<
                lux::ecs::PixelChunk2DComponent>.hash;
            chunk.type = lux::meta::ref_type_of_v<
                lux::ecs::PixelChunk2DComponent>;
            auto coordinate_type = lux::meta::ref_type_of_v<
                lux::spatial::GridCoord2i64>;
            coordinate_type.ptr = &coordinate;
            chunk.fields = {
                lux::meta::RefField{
                    "coordinate",
                    coordinate_type,
                    lux::meta::EVisibility::Public,
                    &chunk,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::PixelChunk2DComponent, coordinate))},
                lux::meta::RefField{
                    "field",
                    lux::meta::ref_type_of_v<
                        lux::ecs::PersistentEntityRef>,
                    lux::meta::EVisibility::Public,
                    &chunk,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::PixelChunk2DComponent, field))},
                lux::meta::RefField{
                    "content",
                    lux::meta::ref_type_of_v<
                        lux::ecs::scene_format::ContentBlobRef>,
                    lux::meta::EVisibility::Public,
                    &chunk,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::PixelChunk2DComponent, content))}};

            tile_chunk.name = "TileChunk2DComponent";
            tile_chunk.full_name = "lux::ecs::TileChunk2DComponent";
            tile_chunk.hash = lux::meta::ref_type_of_v<
                lux::ecs::TileChunk2DComponent>.hash;
            tile_chunk.type = lux::meta::ref_type_of_v<
                lux::ecs::TileChunk2DComponent>;
            tile_chunk.fields = {
                lux::meta::RefField{
                    "coordinate",
                    coordinate_type,
                    lux::meta::EVisibility::Public,
                    &tile_chunk,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::TileChunk2DComponent, coordinate))},
                lux::meta::RefField{
                    "tilemap",
                    lux::meta::ref_type_of_v<
                        lux::ecs::PersistentEntityRef>,
                    lux::meta::EVisibility::Public,
                    &tile_chunk,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::TileChunk2DComponent, tilemap))},
                lux::meta::RefField{
                    "content",
                    lux::meta::ref_type_of_v<
                        lux::ecs::scene_format::ContentBlobRef>,
                    lux::meta::EVisibility::Public,
                    &tile_chunk,
                    static_cast<std::uint32_t>(offsetof(
                        lux::ecs::TileChunk2DComponent, content))}};
        }
    };

    template<class Component>
    void registerComponent(
        lux::ecs::ComponentTypeCatalog& catalog,
        std::string_view schema,
        const lux::meta::RefClass& reflection)
    {
        const auto type = lux::ecs::typeToken<Component>();
        const auto registered = catalog.registerSchema(
            lux::ecs::ComponentSchemaDescriptor{
                {type.hash, std::string{type.name}},
                {lux::cxx::algorithm::fnv1a(schema), std::string{schema}},
                1u,
                &reflection,
                {&has<Component>,
                 &get<Component>,
                 &emplace<Component>,
                 &remove<Component>,
                 nullptr,
                 &notify<Component>,
                 &reserve<Component>,
                 &transfer<Component>,
                 true},
                "lux.pixel",
                {},
                lux::ecs::EComponentSerializationPolicy::COOKED});
        assert(registered);
    }

    struct FieldFacts final
    {
        lux::asset::asset_id_t definition{ordinalUuid(41u)};
        lux::asset::asset_id_t material{ordinalUuid(42u)};
        double cell_size{0.25};
        std::int32_t draw_priority{17};
        bool visible{true};
        bool simulation_enabled{true};
    };

    std::vector<std::byte> fieldPayload(const FieldFacts& facts)
    {
        std::vector<std::byte> result;
        lux::serialize::ArchiveWriter writer{result};
        const auto writeHeader = [&](
            std::uint32_t name,
            lux::serialize::EArchiveType type,
            std::uint32_t size)
        {
            writer.writePod(name);
            writer.writePod(static_cast<std::uint8_t>(type));
            writer.writePod(size);
        };
        writeHeader(2u, lux::serialize::EArchiveType::AssetRef, 16u);
        writer.writeUuid(facts.definition);
        writeHeader(4u, lux::serialize::EArchiveType::AssetRef, 16u);
        writer.writeUuid(facts.material);
        writeHeader(1u, lux::serialize::EArchiveType::Double, sizeof(double));
        writer.writePod(facts.cell_size);
        writeHeader(
            3u,
            lux::serialize::EArchiveType::Int32,
            sizeof(std::int32_t));
        writer.writePod(facts.draw_priority);
        writeHeader(6u, lux::serialize::EArchiveType::Bool, 1u);
        writer.writePod<std::uint8_t>(facts.visible ? 1u : 0u);
        writeHeader(5u, lux::serialize::EArchiveType::Bool, 1u);
        writer.writePod<std::uint8_t>(
            facts.simulation_enabled ? 1u : 0u);
        writer.writePod(lux::serialize::kEndOfObject);
        return result;
    }

    struct FieldSection final
    {
        lux::scene::SectionRecord record;
        lux::asset::asset_id_t asset;
        std::vector<std::byte> bytes;
        std::string path;
    };

    FieldSection makeFieldSection(
        const lux::ecs::PersistentEntityId& field_id,
        const FieldFacts& facts)
    {
        using namespace lux::ecs::scene_format;
        EntitySectionImage image;
        image.section = EntitySectionId{ordinalUuid(100u)};
        image.component_names = {
            "",
            "cell_size",
            "definition",
            "draw_priority",
            "material",
            "simulation_enabled",
            "visible"};
        image.schemas.push_back({
            lux::ecs::componentSchemaId(kPixelFieldSchema),
            1u,
            EEntityComponentStorage::DATA});
        image.archetypes.push_back({{0u}});
        image.entities.push_back({
            0u,
            field_id});
        auto payload = fieldPayload(facts);
        image.columns.push_back({
            0u,
            0u,
            {0u, static_cast<std::uint32_t>(payload.size())},
            std::move(payload)});
        auto encoded =
            lux::ecs::scene_format::encodeEntitySectionImage(image);
        assert(encoded);

        FieldSection result;
        result.asset = ordinalUuid(101u);
        result.bytes = std::move(*encoded);
        result.path = "Sections/PixelField_lxes";
        result.record.id = image.section;
        result.record.source = lux::scene::StoredSectionSource{
            "/Game/" + result.path};
        result.record.content_digest =
            lux::ecs::scene_format::entitySectionContentDigest(result.bytes);
        result.record.encoded_bytes = result.bytes.size();
        result.record.decoded_bytes = result.bytes.size();
        result.record.entity_count = 1u;
        result.record.required_components.push_back({
            lux::ecs::componentSchemaId(kPixelFieldSchema),
            1u});
        return result;
    }

    class FieldProvider final : public lux::asset::IAssetProvider
    {
    public:
        explicit FieldProvider(std::vector<FieldSection> sections)
            : sections_(std::move(sections))
        {}

        [[nodiscard]] std::optional<lux::asset::asset_id_t> resolve(
            std::string_view path) const override
        {
            const auto found = std::ranges::find(
                sections_, path, &FieldSection::path);
            return found != sections_.end()
                ? std::optional<lux::asset::asset_id_t>{found->asset}
                : std::nullopt;
        }

        [[nodiscard]] bool contains(
            const lux::asset::asset_id_t& id) const override
        {
            return std::ranges::find(
                       sections_, id, &FieldSection::asset) !=
                sections_.end();
        }

        [[nodiscard]] lux::cxx::expected<
            lux::asset::AssetBlob,
            lux::asset::EAssetError>
        open(const lux::asset::asset_id_t& id) const override
        {
            const auto found = std::ranges::find(
                sections_, id, &FieldSection::asset);
            if (found == sections_.end())
            {
                return lux::cxx::unexpected(
                    lux::asset::EAssetError::ASSET_NOT_EXIST);
            }
            return lux::asset::AssetBlob::fromShared(
                lux::cxx::SharedBytes<>::copyOf(found->bytes));
        }

        void enumerate(
            const std::function<void(const lux::asset::ProviderEntry&)>& fn)
            const override
        {
            for (const auto& section : sections_)
            {
                fn({
                    section.asset,
                    lux::asset::EAssetType::UNKNOWN,
                    section.path,
                    false});
            }
        }

        [[nodiscard]] std::optional<std::string> pathOf(
            const lux::asset::asset_id_t& id) const override
        {
            const auto found = std::ranges::find(
                sections_, id, &FieldSection::asset);
            return found != sections_.end()
                ? std::optional<std::string>{found->path}
                : std::nullopt;
        }

    private:
        std::vector<FieldSection> sections_;
    };

    constexpr std::uint32_t kTileProofParameterMagic = 0x50544d4cu;

    struct TilemapProofState final
    {
        lux::ecs::PersistentEntityId tilemap;
        lux::scene::SectionGeneratorId generator{
            "lux.tilemap.infinite2d.chunk.test"};
        lux::ecs::ComponentSchemaId schema{
            lux::ecs::componentSchemaId(kTileChunkSchema)};
        lux::ecs::scene_format::ContentTypeId content_type{
            std::string{lux::tilemap::kTilemapChunkContentTypeName}};
    };

    lux::ecs::scene_format::EntitySectionId tileProofSectionId(
        lux::spatial::GridCoord2i64 coordinate)
    {
        std::uint64_t hash = 1469598103934665603ull;
        const auto append = [&hash](std::uint64_t value) noexcept
        {
            for (std::uint32_t byte = 0u; byte != 8u; ++byte)
            {
                hash ^= static_cast<std::uint8_t>(value >> (byte * 8u));
                hash *= 1099511628211ull;
            }
        };
        append(static_cast<std::uint64_t>(coordinate.x));
        append(static_cast<std::uint64_t>(coordinate.y));
        return lux::ecs::scene_format::EntitySectionId{ordinalUuid(hash)};
    }

    std::vector<std::byte> tileProofParameters(
        lux::spatial::GridCoord2i64 coordinate)
    {
        std::vector<std::byte> result;
        lux::serialize::ArchiveWriter writer{result};
        writer.writePod(kTileProofParameterMagic);
        writer.writePod(coordinate.x);
        writer.writePod(coordinate.y);
        return result;
    }

    bool decodeTileProofParameters(
        std::span<const std::byte> bytes,
        lux::spatial::GridCoord2i64& coordinate) noexcept
    {
        lux::serialize::ArchiveReader reader{bytes.data(), bytes.size()};
        const auto magic = reader.readPod<std::uint32_t>();
        coordinate.x = reader.readPod<std::int64_t>();
        coordinate.y = reader.readPod<std::int64_t>();
        return reader.ok() && reader.eof() &&
            magic == kTileProofParameterMagic;
    }

    std::vector<std::byte> tileProofCoordinatePayload(
        lux::spatial::GridCoord2i64 coordinate)
    {
        std::vector<std::byte> nested;
        lux::serialize::ArchiveWriter nested_writer{nested};
        nested_writer.writePod<std::uint32_t>(4u);
        nested_writer.writePod<std::uint8_t>(static_cast<std::uint8_t>(
            lux::serialize::EArchiveType::Int64));
        nested_writer.writePod<std::uint32_t>(sizeof(coordinate.x));
        nested_writer.writePod(coordinate.x);
        nested_writer.writePod<std::uint32_t>(5u);
        nested_writer.writePod<std::uint8_t>(static_cast<std::uint8_t>(
            lux::serialize::EArchiveType::Int64));
        nested_writer.writePod<std::uint32_t>(sizeof(coordinate.y));
        nested_writer.writePod(coordinate.y);
        nested_writer.writePod(lux::serialize::kEndOfObject);

        std::vector<std::byte> result;
        lux::serialize::ArchiveWriter writer{result};
        writer.writePod<std::uint32_t>(2u);
        writer.writePod<std::uint8_t>(static_cast<std::uint8_t>(
            lux::serialize::EArchiveType::Struct));
        writer.writePod<std::uint32_t>(
            static_cast<std::uint32_t>(nested.size()));
        writer.writeBytes(nested.data(), nested.size());
        writer.writePod(lux::serialize::kEndOfObject);
        return result;
    }

    lux::ecs::scene_format::EntitySectionImage makeTileProofImage(
        const TilemapProofState& state,
        lux::ecs::scene_format::EntitySectionId section,
        lux::spatial::GridCoord2i64 coordinate)
    {
        using namespace lux::ecs::scene_format;
        EntitySectionImage image;
        image.section = section;
        image.component_names = {
            "", "content", "coordinate", "tilemap", "x", "y"};
        image.schemas.push_back({
            state.schema, 1u, EEntityComponentStorage::DATA});
        image.archetypes.push_back({{0u}});
        image.entities.push_back({0u, std::nullopt});
        auto payload = tileProofCoordinatePayload(coordinate);
        image.columns.push_back({
            0u,
            0u,
            {0u, static_cast<std::uint32_t>(payload.size())},
            std::move(payload)});
        image.persistent_reference_relocations.push_back({
            0u,
            0u,
            3u,
            state.tilemap});

        EntitySectionAttachment attachment;
        attachment.reference.type = state.content_type;
        attachment.reference.schema_version =
            lux::tilemap::kTilemapChunkSchemaVersion;
        lux::tilemap::TilemapChunkBlobV1 content;
        content.tiles.assign(lux::tilemap::kTilemapChunkTileCount, 7u);
        auto encoded_content = lux::tilemap::encodeTilemapChunkBlob(content);
        assert(encoded_content);
        attachment.payload = std::move(*encoded_content);
        attachment.reference.id = makeContentBlobId(
            attachment.reference.type,
            attachment.reference.schema_version,
            attachment.payload);
        image.attachments.push_back(std::move(attachment));
        image.blob_relocations.push_back({0u, 0u, 1u, 0u});
        return image;
    }

    lux::runtime::entity_scene::EntitySectionGeneratorDescriptor
    tileProofGenerator(std::shared_ptr<const TilemapProofState> state)
    {
        using namespace lux::runtime::entity_scene;
        EntitySectionGeneratorDescriptor result;
        result.id = state->generator;
        result.state = std::shared_ptr<const void>{std::move(state)};
        result.generate = [](
            const void* opaque,
            GeneratedEntitySectionRequest request) noexcept
            -> lux::cxx::expected<
                lux::ecs::scene_format::EntitySectionImage,
                EntitySectionGeneratorFailure>
        {
            const auto& state = *static_cast<const TilemapProofState*>(opaque);
            const auto* source = std::get_if<
                lux::scene::GeneratedSectionSource>(
                    &request.record.source);
            lux::spatial::GridCoord2i64 coordinate;
            if (!source || source->generator != state.generator ||
                !decodeTileProofParameters(source->parameters, coordinate) ||
                request.record.id != tileProofSectionId(coordinate))
            {
                return lux::cxx::unexpected(EntitySectionGeneratorFailure{
                    EEntitySectionGeneratorError::GENERATION_FAILED,
                    state.generator,
                    {},
                    "invalid independent Tilemap proof source"});
            }
            return makeTileProofImage(
                state,
                request.record.id,
                coordinate);
        };
        return result;
    }

    lux::scene::SectionRecord tileProofRecord(
        const TilemapProofState& state,
        lux::spatial::GridCoord2i64 coordinate)
    {
        lux::scene::SectionRecord result;
        result.id = tileProofSectionId(coordinate);
        result.source = lux::scene::GeneratedSectionSource{
            state.generator, 0u, tileProofParameters(coordinate)};
        auto image = makeTileProofImage(
            state,
            result.id,
            coordinate);
        auto encoded =
            lux::ecs::scene_format::encodeEntitySectionImage(image);
        assert(encoded);
        result.content_digest =
            lux::ecs::scene_format::entitySectionContentDigest(*encoded);
        result.encoded_bytes = encoded->size();
        result.decoded_bytes = encoded->size();
        result.entity_count = 1u;
        result.required_components.push_back({
            lux::ecs::componentSchemaId(state.schema.name),
            1u});
        return result;
    }

    FieldSection storedTileProofSection(
        const TilemapProofState& state,
        lux::spatial::GridCoord2i64 coordinate)
    {
        FieldSection result;
        result.asset = ordinalUuid(201u);
        result.path = "Sections/StoredTilemapProof_lxes";
        const lux::ecs::scene_format::EntitySectionId section{
            ordinalUuid(200u)};
        auto image = makeTileProofImage(state, section, coordinate);
        auto encoded =
            lux::ecs::scene_format::encodeEntitySectionImage(image);
        assert(encoded);
        result.bytes = std::move(*encoded);
        result.record.id = section;
        result.record.source = lux::scene::StoredSectionSource{
            "/Game/" + result.path};
        result.record.content_digest =
            lux::ecs::scene_format::entitySectionContentDigest(result.bytes);
        result.record.encoded_bytes = result.bytes.size();
        result.record.decoded_bytes = result.bytes.size();
        result.record.entity_count = 1u;
        result.record.required_components.push_back({
            lux::ecs::componentSchemaId(state.schema.name),
            1u});
        return result;
    }

    bool validateTileProof(
        lux::meta::EntityRegistry& registry,
        lux::ecs::PersistentEntityIndex& persistent_entities,
        lux::runtime::entity_scene::ContentBlobClient blobs,
        lux::spatial::GridCoord2i64 expected,
        const lux::ecs::PersistentEntityId& expected_owner)
    {
        auto view = registry.view<const lux::ecs::TileChunk2DComponent>();
        if (registry.storage<lux::ecs::TileChunk2DComponent>().size() != 1u)
            return false;
        const auto entity = *view.begin();
        const auto& chunk =
            registry.get<lux::ecs::TileChunk2DComponent>(entity);
        const auto owner = persistent_entities.find(chunk.tilemap.id);
        if (chunk.coordinate != expected || !chunk.tilemap.valid() ||
            chunk.tilemap.id != expected_owner || owner == entt::null ||
            !registry.all_of<lux::ecs::TilemapComponent>(owner))
        {
            return false;
        }
        auto content = blobs.resolve(chunk.content);
        if (!content)
            return false;
        const auto decoded = lux::tilemap::decodeTilemapChunkBlob(
            content->bytes().view());
        if (!decoded ||
            decoded->tiles.size() != lux::tilemap::kTilemapChunkTileCount ||
            !std::ranges::all_of(
                decoded->tiles,
                [](std::uint16_t tile) noexcept { return tile == 7u; }))
        {
            return false;
        }
        return true;
    }

    class TilemapInterestAdapter final
        : public lux::runtime::spatial2d::TilemapChunkActivity2D
    {
    public:
        explicit TilemapInterestAdapter(
            const lux::runtime::spatial2d::SpatialInterest2DSystem&
                interest) noexcept
            : interest_(&interest)
        {}

        [[nodiscard]] bool isActive(
            lux::ecs::TileChunkCoord coordinate) const noexcept override
        {
            return interest_->isActive(coordinate);
        }

    private:
        const lux::runtime::spatial2d::SpatialInterest2DSystem* interest_;
    };

    std::size_t chunkCount(lux::meta::EntityRegistry& registry)
    {
        return registry.storage<lux::ecs::PixelChunk2DComponent>().size();
    }

    bool hasExactChunkWindow(
        lux::meta::EntityRegistry& registry,
        lux::spatial::GridCoord2i64 center) noexcept
    {
        std::array<bool,
            lux::runtime::spatial2d::kSpatial2DResidentSectionCount> seen{};
        std::size_t count = 0u;
        for (const auto entity :
             registry.view<const lux::ecs::PixelChunk2DComponent>())
        {
            const auto coordinate = registry.get<
                lux::ecs::PixelChunk2DComponent>(entity).coordinate;
            const auto x = coordinate.x - center.x;
            const auto y = coordinate.y - center.y;
            if (x < -2 || x > 2 || y < -2 || y > 2)
                return false;
            const auto index = static_cast<std::size_t>(y + 2) * 5u +
                static_cast<std::size_t>(x + 2);
            if (seen[index])
                return false;
            seen[index] = true;
            ++count;
        }
        return count == seen.size();
    }

    template<class Predicate>
    void drive(
        lux::exec::AsyncRuntime& runtime,
        lux::ecs::Schedule& schedule,
        const lux::runtime::entity_scene::EntitySectionLoaderSystem& loader,
        const lux::runtime::spatial_partition::SpatialPartitionSystem&
            partition,
        const lux::runtime::spatial2d::SpatialInterest2DSystem& interest,
        const lux::ecs::PixelFieldSystem& fields,
        const lux::runtime::spatial2d::Infinite2DPixelSystem& pixels,
        lux::meta::EntityRegistry& registry,
        Predicate&& done)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        progress.driveWithStep(
            [&schedule]() noexcept { tickSchedule(schedule, 0.0f); },
            std::forward<Predicate>(done),
            [&]() noexcept
            {
                const auto load = loader.snapshot();
                const auto partition_state = partition.snapshot();
                const auto interest_state = interest.snapshot();
                const auto field = fields.snapshot();
                const auto pixel = pixels.snapshot();
                const auto tilemap = g_tilemap_chunk_system
                    ? g_tilemap_chunk_system->snapshot()
                    : lux::runtime::spatial2d::
                          TilemapChunkSystemSnapshot{};
                std::array<std::size_t, 4u> tilemap_domain_states{};
                registry.view<const lux::runtime::spatial2d::
                    TilemapChunkDomainStateComponent>().each(
                    [&tilemap_domain_states](const auto& state) noexcept
                    {
                        ++tilemap_domain_states[
                            static_cast<std::size_t>(state.state)];
                    });
                const bool tilemap_snapshot_stale =
                    tilemap.waiting_chunks != tilemap_domain_states[
                        static_cast<std::size_t>(lux::runtime::spatial2d::
                            ETilemapChunkDomainState::WAITING_TILEMAP)] ||
                    tilemap.staging_chunks != tilemap_domain_states[
                        static_cast<std::size_t>(lux::runtime::spatial2d::
                            ETilemapChunkDomainState::STAGING)] ||
                    tilemap.ready_chunks != tilemap_domain_states[
                        static_cast<std::size_t>(lux::runtime::spatial2d::
                            ETilemapChunkDomainState::READY)] ||
                    tilemap.failed_chunks != tilemap_domain_states[
                        static_cast<std::size_t>(lux::runtime::spatial2d::
                            ETilemapChunkDomainState::FAILED)];
                const auto chunks = chunkCount(registry);
                assert(load.failed_sections == 0u);
                assert(partition_state.failed_sections == 0u);
                assert(!interest_state.last_failure);
                assert(field.command_rejections == 0u);
                return load.waiting_admission_sections != 0u ||
                    load.staging_sections != 0u ||
                    load.armed_sections != 0u ||
                    (interest_state.closing && !interest_state.closed) ||
                    partition_state.loader_tickets !=
                        partition_state.demand.resident_sections ||
                    field.intents_enqueued != field.commands_applied ||
                    pixel.waiting_chunks != 0u ||
                    pixel.admission_chunks != 0u ||
                    pixel.publish_pending_chunks != 0u ||
                    pixel.retiring_chunks != 0u ||
                    tilemap.waiting_chunks != 0u ||
                    tilemap.retiring_chunks != 0u ||
                    tilemap.staging_chunks >
                        tilemap.background_chunks ||
                    tilemap.commands_enqueued !=
                        tilemap.commands_applied ||
                    tilemap_snapshot_stale ||
                    chunks != pixel.waiting_chunks +
                        pixel.staging_chunks + pixel.ready_chunks +
                        pixel.failed_chunks;
            });
    }

    template<class Sender>
    void closeOwner(
        lux::exec::AsyncRuntime& runtime,
        Sender sender)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        std::atomic<bool> closed{false};
        auto completion = std::move(sender)
            | stdexec::then(
                  [&closed, &progress]() noexcept
                  {
                      closed.store(true, std::memory_order_release);
                      progress.notify();
                  });
        ::experimental::execution::start_detached(std::move(completion));
        progress.drive(
            [&closed]() noexcept
            {
                return closed.load(std::memory_order_acquire);
            });
    }

    void validateWindow(
        lux::meta::EntityRegistry& registry,
        lux::runtime::entity_scene::ContentBlobClient blobs,
        lux::spatial::GridCoord2i64 center,
        const lux::ecs::PersistentEntityId& field_id)
    {
        std::vector<lux::spatial::GridCoord2i64> coordinates;
        registry.view<const lux::ecs::PixelChunk2DComponent>().each(
            [&](const lux::ecs::PixelChunk2DComponent& chunk)
            {
                coordinates.push_back(chunk.coordinate);
                assert(chunk.field.valid());
                assert(chunk.field.id == field_id);
                assert(chunk.content.valid());
                const auto content = blobs.resolve(chunk.content);
                assert(content);
                const auto decoded =
                    lux::runtime::spatial2d::decodeInfinite2DPixelChunk(
                        *content);
                assert(decoded);
                assert(decoded->coordinate == chunk.coordinate);
                assert(decoded->materials.size() ==
                    lux::ecs::PixelFieldRuntime::kChunkCellCount);
                std::array<std::size_t, 6u> material_counts{};
                for (const auto material : decoded->materials)
                {
                    assert(material < material_counts.size());
                    ++material_counts[material];
                }
                assert(material_counts[kForegroundMaterial] != 0u);
                assert(material_counts[kLandmarkMaterial] != 0u);
                assert(material_counts[kSandMaterial] != 0u);
                assert(material_counts[kWaterMaterial] != 0u);
                assert(material_counts[kPlayerMaterial] != 0u);
            });
        assert(coordinates.size() ==
            lux::runtime::spatial2d::kSpatial2DResidentSectionCount);
        std::sort(coordinates.begin(), coordinates.end());
        std::vector<lux::spatial::GridCoord2i64> expected;
        for (std::int64_t y = -2; y <= 2; ++y)
            for (std::int64_t x = -2; x <= 2; ++x)
                expected.push_back({center.x + x, center.y + y});
        std::sort(expected.begin(), expected.end());
        assert(coordinates == expected);
    }
}

int lux::runtime::spatial2d::testing::runInfinite2DScenario(
    Infinite2DTestExtension* extension)
{
    namespace entity_runtime = lux::runtime::entity_scene;
    namespace partition = lux::runtime::spatial_partition;
    namespace spatial2d = lux::runtime::spatial2d;

    Reflections reflections;
    lux::ecs::ComponentTypeCatalog components;
    registerComponent<lux::ecs::PixelField2DComponent>(
        components, kPixelFieldSchema, reflections.field);
    registerComponent<lux::ecs::PixelChunk2DComponent>(
        components, kPixelChunkSchema, reflections.chunk);
    registerComponent<lux::ecs::TileChunk2DComponent>(
        components, kTileChunkSchema, reflections.tile_chunk);

    const lux::ecs::PersistentEntityId field_id{
        ordinalUuid(10u)};
    constexpr lux::spatial::GridCoord2i64 kTileProofCoordinate{
        -1'234'567, 7'654'321};
    const FieldFacts field_facts;
    auto field_section = makeFieldSection(field_id, field_facts);
    const auto field_record = field_section.record;
    auto tile_proof_state = std::make_shared<const TilemapProofState>(
        TilemapProofState{field_id});
    auto stored_tile_section = storedTileProofSection(
        *tile_proof_state, kTileProofCoordinate);
    const auto stored_tile_record = stored_tile_section.record;
    auto provider = std::make_shared<FieldProvider>(
        std::vector<FieldSection>{
            std::move(field_section), std::move(stored_tile_section)});
    auto vfs = std::make_shared<lux::asset::AssetVfs>();
    assert(vfs->mount({"/Game", provider, 0}) !=
        lux::asset::kInvalidMountId);

    auto generated = spatial2d::Infinite2DPixelSectionSource::create({
        .field = field_id,
        .generator = lux::scene::SectionGeneratorId{
            "lux.pixel.infinite2d.chunk"},
        .chunk_schema = lux::ecs::componentSchemaId(kPixelChunkSchema),
        .content_type = lux::ecs::scene_format::ContentTypeId{
            "lux.pixel.chunk"},
        .demand_channel = lux::scene::DemandChannelId{
            std::string{kDemandChannel}},
        .seed = 0x4c55583244494e46ull,
        .foreground_material = kForegroundMaterial,
        .landmark_material = kLandmarkMaterial,
        .sand_material = kSandMaterial,
        .water_material = kWaterMaterial,
        .player_material = kPlayerMaterial});
    assert(generated);
    auto generator_catalog =
        entity_runtime::EntitySectionGeneratorCatalog::create(
            {generated->generatorDescriptor(),
             tileProofGenerator(tile_proof_state)});
    assert(generator_catalog);

    lux::exec::AsyncRuntimeBuilder runtime_builder;
    auto section_service = entity_runtime::EntitySectionService::addTo(
        runtime_builder, *generator_catalog);
    assert(section_service);
    auto pixel_prepare_service =
        spatial2d::Infinite2DPixelPrepareService::addTo(runtime_builder);
    assert(pixel_prepare_service);
    auto tilemap_prepare_service =
        spatial2d::TilemapPrepareService::addTo(runtime_builder);
    assert(tilemap_prepare_service);
    auto runtime_plan = std::move(runtime_builder).compile();
    assert(runtime_plan);
    lux::exec::AsyncRuntime runtime{
        std::move(*runtime_plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1u,
            .background_cpu_concurrency = 2u}};

    // Admission is fail-closed before the provider runs. This is a
    // deterministic byte-budget rejection, independent of worker timing.
    lux::exec::AsyncScope backpressure_scope{runtime};
    {
        std::optional<lux::exec::AsyncOutcome<
            spatial2d::PrepareInfinite2DPixelChunk>> backpressure_outcome;
        std::atomic<bool> backpressure_done{false};
        lux::exec::testing::CloseEpoch backpressure_progress{runtime};
        auto backpressure = lux::exec::execute(
                pixel_prepare_service->client().operation(),
                spatial2d::PrepareInfinite2DPixelChunk{},
                lux::exec::AsyncSubmitOptions{
                    .accounted_bytes =
                        spatial2d::kInfinite2DPixelPrepareByteBudget + 1u})
            | stdexec::continues_on(lux::exec::mainThreadScheduler(runtime))
            | stdexec::then(
                  [&](auto outcome) noexcept
                  {
                      backpressure_outcome.emplace(std::move(outcome));
                      backpressure_done.store(
                          true, std::memory_order_release);
                      backpressure_progress.notify();
                  });
        assert(lux::exec::spawn(
            backpressure_scope, std::move(backpressure)));
        backpressure_progress.drive([&]() noexcept
        {
            return backpressure_done.load(std::memory_order_acquire);
        });
        assert(backpressure_outcome && !*backpressure_outcome);
        assert(backpressure_outcome->error().isRuntime());
        assert(backpressure_outcome->error().runtimeError() ==
            lux::exec::EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED);
    }
    closeOwner(runtime, backpressure_scope.closeAsync());

    // Tilemap owns a separate typed queue and byte budget. Rejecting this
    // request cannot consume a Pixel admission slot or publish partial ECS
    // state.
    lux::exec::AsyncScope tilemap_backpressure_scope{runtime};
    {
        std::optional<lux::exec::AsyncOutcome<
            spatial2d::PrepareTilemapChunk>> outcome;
        std::atomic<bool> done{false};
        lux::exec::testing::CloseEpoch progress{runtime};
        auto rejected = lux::exec::execute(
                tilemap_prepare_service->client().operation(),
                spatial2d::PrepareTilemapChunk{},
                lux::exec::AsyncSubmitOptions{
                    .accounted_bytes =
                        spatial2d::kTilemapPrepareByteBudget + 1u})
            | stdexec::continues_on(
                  lux::exec::mainThreadScheduler(runtime))
            | stdexec::then(
                  [&](auto value) noexcept
                  {
                      outcome.emplace(std::move(value));
                      done.store(true, std::memory_order_release);
                      progress.notify();
                  });
        assert(lux::exec::spawn(
            tilemap_backpressure_scope, std::move(rejected)));
        progress.drive([&]() noexcept
        {
            return done.load(std::memory_order_acquire);
        });
        assert(outcome && !*outcome && outcome->error().isRuntime());
        assert(outcome->error().runtimeError() ==
            lux::exec::EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED);
    }
    closeOwner(runtime, tilemap_backpressure_scope.closeAsync());

    auto sample_record = generated->record({0, 0});
    assert(sample_record);
    auto scene_catalog = emptyCatalog();
    partition::EntitySectionRecordStore record_store{scene_catalog};
    auto demand_planner = partition::SpatialDemandPlanner::create(
        std::move(record_store),
        partition::SpatialPartitionBudget{
            sample_record->decoded_bytes *
                spatial2d::kSpatial2DResidentSectionCount,
            spatial2d::kSpatial2DResidentSectionCount});
    assert(demand_planner);
    auto spatial_source = spatial2d::Spatial2DSectionSource::procedural(
        generated->recordFactory());
    assert(spatial_source);

    lux::ecs::World world;
    lux::ecs::PersistentEntityIndex persistent_entities{world.registry()};
    lux::ecs::Schedule schedule{world};
    auto loader = std::make_unique<
        entity_runtime::EntitySectionLoaderSystem>(
            runtime,
            section_service->loadClient(),
            vfs,
            components,
            persistent_entities,
            entity_runtime::EntitySectionLoaderConfig{2u});
    auto* loader_owner = loader.get();
    assert(schedule.addSystem(std::move(loader)));
    auto partition_system = std::make_unique<
        partition::SpatialPartitionSystem>(
            loader_owner->client(), std::move(*demand_planner));
    auto* partition_owner = partition_system.get();
    assert(schedule.addSystem(std::move(partition_system)));
    auto interest_system = std::make_unique<
        spatial2d::SpatialInterest2DSystem>(
            *partition_owner,
            std::move(*spatial_source),
            spatial2d::SpatialInterest2DConfig{
                .section_world_size = 64.0,
                .channel = lux::scene::DemandChannelId{
                    std::string{kDemandChannel}},
                .resident_priority = 1u,
                .maximum_sources = 2u});
    auto* interest_owner = interest_system.get();
    assert(schedule.addSystem(std::move(interest_system)));

    lux::ecs::PixelFieldRuntime pixel_runtime{
        lux::ecs::PixelFieldRuntimeConfig{1u}};
    assert(pixel_runtime.materials().add({
        lux::ecs::EMaterialPhase::SOLID,
        255u,
        0xff65b96bu}) == kForegroundMaterial);
    const auto edited_material = pixel_runtime.materials().add({
        lux::ecs::EMaterialPhase::SOLID,
        254u,
        0xffd66b5fu});
    assert(edited_material == kLandmarkMaterial);
    assert(pixel_runtime.materials().add({
        lux::ecs::EMaterialPhase::POWDER,
        200u,
        0xffc2b280u}) == kSandMaterial);
    assert(pixel_runtime.materials().add({
        lux::ecs::EMaterialPhase::LIQUID,
        100u,
        0xff3060c0u}) == kWaterMaterial);
    assert(pixel_runtime.materials().add({
        lux::ecs::EMaterialPhase::SOLID,
        253u,
        0xffff4fd8u}) == kPlayerMaterial);
    lux::ecs::PixelChunkPersistenceStore pixel_persistence;
    auto field_system = std::make_unique<lux::ecs::PixelFieldSystem>(
        pixel_runtime, persistent_entities);
    auto* field_owner = field_system.get();
    assert(schedule.addSystem(std::move(field_system)));
    auto pixel_system = std::make_unique<spatial2d::Infinite2DPixelSystem>(
        runtime,
        pixel_prepare_service->client(),
        pixel_runtime,
        *field_owner,
        pixel_persistence,
        loader_owner->contentBlobs(),
        *interest_owner);
    auto* pixel_owner = pixel_system.get();
    assert(schedule.addSystem(std::move(pixel_system)));
    lux::ecs::TilemapRuntime tilemap_runtime;
    lux::exec::AsyncScope tilemap_scope{runtime};
    auto tilemap_system = std::make_unique<lux::ecs::TilemapSystem>(
        tilemap_runtime, persistent_entities);
    auto* tilemap_owner_system = tilemap_system.get();
    assert(schedule.addSystem(std::move(tilemap_system)));
    TilemapInterestAdapter tilemap_activity{*interest_owner};
    auto tilemap_chunks = std::make_unique<
        spatial2d::TilemapChunkSystem>(
            runtime,
            tilemap_scope,
            tilemap_prepare_service->client(),
            tilemap_runtime,
            *tilemap_owner_system,
            loader_owner->contentBlobs(),
            &tilemap_activity,
            spatial2d::TilemapChunkSystemConfig{
                .maximum_tracked_chunks = 64u,
                .maximum_retirements_per_update = 1u});
    auto* tilemap_chunk_owner = tilemap_chunks.get();
    assert(schedule.addSystem(std::move(tilemap_chunks)));
    g_tilemap_chunk_system = tilemap_chunk_owner;
    g_test_extension = extension;
    if (extension && !extension->install(world, schedule, pixel_runtime))
    {
        g_test_extension = nullptr;
        return 1;
    }
    assert(schedule.compile().valid());

    assert(lux::scene::validateSectionRecord(field_record));
    assert(loader_owner->client());
    assert(section_service->loadClient());
    assert(section_service->loadClient().supports(field_record));
    auto field_ticket_result = loader_owner->client().acquire(field_record);
    assert(field_ticket_result);
    auto field_ticket = std::move(*field_ticket_result);
    auto& registry = world.registry();
    const auto interest = registry.create();
    registry.emplace<lux::ecs::SpatialInterest2DComponent>(interest);
    registry.emplace<lux::ecs::ResolvedTransform2DComponent>(interest);

    const auto readyAt = [&](lux::spatial::GridCoord2i64 center)
    {
        return [&, center]() noexcept
        {
            const auto spatial = interest_owner->snapshot();
            const auto partitions = partition_owner->snapshot();
            const auto pixels = pixel_owner->snapshot();
            const auto runtime_stats = pixel_runtime.stats();
            return field_ticket.state() ==
                    entity_runtime::EEntitySectionState::ACTIVE &&
                spatial.active_sections ==
                    spatial2d::kSpatial2DActiveSectionCount &&
                spatial.resident_sections ==
                    spatial2d::kSpatial2DResidentSectionCount &&
                partitions.active_sections ==
                    spatial2d::kSpatial2DResidentSectionCount &&
                partitions.demand.dynamic_records ==
                    spatial2d::kSpatial2DResidentSectionCount &&
                hasExactChunkWindow(registry, center) &&
                pixels.ready_chunks ==
                    spatial2d::kSpatial2DResidentSectionCount &&
                pixels.resident_chunks ==
                    spatial2d::kSpatial2DResidentSectionCount &&
                pixels.active_chunks ==
                    spatial2d::kSpatial2DActiveSectionCount &&
                runtime_stats.resident_chunks ==
                    spatial2d::kSpatial2DResidentSectionCount &&
                runtime_stats.presentation_active_chunks ==
                    spatial2d::kSpatial2DActiveSectionCount &&
                runtime_stats.simulation_active_chunks ==
                    spatial2d::kSpatial2DActiveSectionCount;
        };
    };

    drive(
        runtime,
        schedule,
        *loader_owner,
        *partition_owner,
        *interest_owner,
        *field_owner,
        *pixel_owner,
        registry,
        readyAt({0, 0}));
    validateWindow(
        registry, loader_owner->contentBlobs(), {0, 0}, field_id);
    {
        const auto preparation_stats = pixel_runtime.stats();
        // Infinite2D must use the move-only worker-prepared adoption path.
        // A legacy synchronous preparation or capturing unload here would put
        // a 65,536-cell scan/copy back on the owner thread.
        assert(preparation_stats.synchronous_chunk_preparations == 0u);
        assert(preparation_stats.prepared_chunk_adoptions >=
            spatial2d::kSpatial2DResidentSectionCount);
        assert(preparation_stats.capturing_chunk_unloads == 0u);
    }
    const auto field_entity = persistent_entities.find(field_id);
    assert(field_entity != entt::null);
    const auto& loaded_field =
        registry.get<lux::ecs::PixelField2DComponent>(field_entity);
    assert(loaded_field.definition == field_facts.definition);
    assert(loaded_field.material == field_facts.material);
    assert(loaded_field.cell_size == field_facts.cell_size);
    assert(loaded_field.draw_priority == field_facts.draw_priority);
    assert(loaded_field.visible == field_facts.visible);
    assert(loaded_field.simulation_enabled ==
        field_facts.simulation_enabled);
    const auto field_handle = field_owner->resolveField(
        lux::ecs::PersistentEntityRef{field_id});
    assert(field_handle.isValid());
    if (extension)
    {
        assert(extension->checkpoint(
            EInfinite2DCheckpoint::ORIGIN_READY,
            world,
            pixel_runtime,
            field_handle,
            {0, 0}));
    }
    lux::ecs::PixelFieldCommand edit_origin;
    edit_origin.field = field_handle;
    edit_origin.minimum = {7, 11};
    edit_origin.extent = {1u, 1u};
    edit_origin.material = edited_material;
    pixel_runtime.enqueue(edit_origin);
    pixel_runtime.applyCommands();
    lux::ecs::PixelChunkDeltaSnapshot expected_origin_delta;
    assert(pixel_runtime.captureChunkDelta(
        field_handle, {0, 0}, expected_origin_delta));
    assert(expected_origin_delta.sequence == 1u);
    assert(expected_origin_delta.delta.size() == 1u);
    const lux::ecs::PixelChunkDeltaCell expected_edit{
        7u, 11u, edited_material};
    assert(expected_origin_delta.delta.front() == expected_edit);
    const auto domain_record =
        lux::ecs::encodePixelChunkPersistence(expected_origin_delta);
    assert(domain_record);
    const auto generic_wire =
        lux::ecs::scene_format::encodePersistenceJournalRecord(*domain_record);
    assert(generic_wire);
    const auto generic_roundtrip =
        lux::ecs::scene_format::decodePersistenceJournalRecord(*generic_wire);
    assert(generic_roundtrip && *generic_roundtrip == *domain_record);
    auto& tilemap_owner = registry.emplace<lux::ecs::TilemapComponent>(
        field_entity);
    tilemap_owner.id = lux::ecs::TilemapId{ordinalUuid(300u)};
    tilemap_owner.tileset_cols = 1u;
    tilemap_owner.tileset_rows = 1u;

    // Stored and generated Section sources publish the same ECS fact and LXTC
    // payload through the one generic loader/barrier path.
    auto stored_tile_ticket_result = loader_owner->client().acquire(
        stored_tile_record);
    assert(stored_tile_ticket_result);
    auto stored_tile_ticket = std::move(*stored_tile_ticket_result);
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        return stored_tile_ticket.state() ==
                entity_runtime::EEntitySectionState::ACTIVE &&
            tilemap_chunk_owner->snapshot().ready_chunks == 1u &&
            tilemap_runtime.stats().resident_chunks == 1u;
    });
    assert(validateTileProof(
        registry,
        persistent_entities,
        loader_owner->contentBlobs(),
        kTileProofCoordinate,
        field_id));
    const auto stored_tile_entity =
        *registry.view<const lux::ecs::TileChunk2DComponent>().begin();
    const auto stored_tile_fact = registry.get<
        lux::ecs::TileChunk2DComponent>(stored_tile_entity);
    std::vector<std::byte> stored_tile_bytes;
    {
        auto stored_tile_lease = loader_owner->contentBlobs().resolve(
            stored_tile_fact.content);
        assert(stored_tile_lease);
        const auto stored_span = stored_tile_lease->bytes().view();
        stored_tile_bytes.assign(stored_span.begin(), stored_span.end());
    }
    stored_tile_ticket.reset();
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        return registry.storage<lux::ecs::TileChunk2DComponent>().empty() &&
            tilemap_chunk_owner->snapshot().tracked_chunks == 0u &&
            tilemap_runtime.stats().resident_chunks == 0u;
    });

    // Open/closed proof: a second 2D domain contributes a generated LXES
    // provider and its real ECS owner consumes the production LXTC blob.
    // Neither EntityScene nor SpatialPartition switches on Tilemap content.
    auto tile_ticket_result = loader_owner->client().acquire(
        tileProofRecord(*tile_proof_state, kTileProofCoordinate));
    assert(tile_ticket_result);
    auto tile_ticket = std::move(*tile_ticket_result);
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        return tile_ticket.state() ==
                entity_runtime::EEntitySectionState::ACTIVE &&
            registry.storage<lux::ecs::TileChunk2DComponent>().size() == 1u &&
            tilemap_chunk_owner->snapshot().ready_chunks == 1u &&
            tilemap_runtime.stats().resident_chunks == 1u;
    });
    assert(validateTileProof(
        registry,
        persistent_entities,
        loader_owner->contentBlobs(),
        kTileProofCoordinate,
        field_id));

    const auto generated_tile_entity =
        *registry.view<const lux::ecs::TileChunk2DComponent>().begin();
    const auto generated_tile_fact = registry.get<
        lux::ecs::TileChunk2DComponent>(generated_tile_entity);
    assert(generated_tile_fact.coordinate == stored_tile_fact.coordinate);
    assert(generated_tile_fact.tilemap == stored_tile_fact.tilemap);
    assert(generated_tile_fact.content == stored_tile_fact.content);
    {
        auto generated_tile_content = loader_owner->contentBlobs().resolve(
            generated_tile_fact.content);
        assert(generated_tile_content);
        assert(std::ranges::equal(
            generated_tile_content->bytes().view(), stored_tile_bytes));
    }

    // A Tilemap-domain failure never rolls back the generic generated
    // Section or Pixel. Patching the authoritative fact retries only this
    // participant.
    auto invalid_tile_content = generated_tile_fact.content;
    invalid_tile_content.id.digest[0] ^= std::byte{0x6du};
    registry.patch<lux::ecs::TileChunk2DComponent>(
        generated_tile_entity,
        [&invalid_tile_content](auto& component) noexcept
        {
            component.content = invalid_tile_content;
        });
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        return tilemap_chunk_owner->snapshot().failed_chunks == 1u &&
            tilemap_runtime.stats().resident_chunks == 0u;
    });
    assert(tile_ticket.state() ==
        entity_runtime::EEntitySectionState::ACTIVE);
    assert(pixel_owner->snapshot().ready_chunks ==
        spatial2d::kSpatial2DResidentSectionCount);
    registry.patch<lux::ecs::TileChunk2DComponent>(
        generated_tile_entity,
        [&generated_tile_fact](auto& component) noexcept
        {
            component.content = generated_tile_fact.content;
        });
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        return tilemap_chunk_owner->snapshot().ready_chunks == 1u &&
            tilemap_runtime.stats().resident_chunks == 1u;
    });

    // Feed the same pinned generated content to a full 5x5 Tilemap resident
    // window. The optional activity adapter yields the same exact 3x3 active
    // set as Pixel without either domain depending on the other.
    std::vector<entt::entity> tile_window;
    tile_window.reserve(spatial2d::kSpatial2DResidentSectionCount);
    entt::entity replaced_tile = entt::null;
    for (std::int64_t y = -2; y <= 2; ++y)
    {
        for (std::int64_t x = -2; x <= 2; ++x)
        {
            const auto entity = registry.create();
            registry.emplace<lux::ecs::TileChunk2DComponent>(
                entity,
                lux::ecs::TileChunk2DComponent{
                    {x, y},
                    lux::ecs::PersistentEntityRef{field_id},
                    generated_tile_fact.content});
            tile_window.push_back(entity);
            if (x == 2 && y == 2)
                replaced_tile = entity;
        }
    }
    assert(replaced_tile != entt::null);
    tickSchedule(schedule, 0.0f);
    tickSchedule(schedule, 0.0f);
    registry.patch<lux::ecs::TileChunk2DComponent>(
        replaced_tile,
        [](auto& component) noexcept
        {
            component.coordinate = {3, 3};
        });
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        const auto tilemap = tilemap_chunk_owner->snapshot();
        const auto storage = tilemap_runtime.stats();
        return tilemap.ready_chunks ==
                spatial2d::kSpatial2DResidentSectionCount + 1u &&
            storage.resident_chunks ==
                spatial2d::kSpatial2DResidentSectionCount + 1u &&
            storage.active_chunks ==
                spatial2d::kSpatial2DActiveSectionCount;
    });
    const auto tilemap_handle = tilemap_owner_system->resolveTilemap(
        lux::ecs::PersistentEntityRef{field_id});
    assert(tilemap_handle.isValid());
    assert(!tilemap_runtime.chunkResident(tilemap_handle, {2, 2}));
    assert(tilemap_runtime.chunkResident(tilemap_handle, {3, 3}));

    // Section hide removes its ECS fact immediately. The other 25 leases pin
    // the content while their independent runtime chunks remain resident.
    tile_ticket.reset();
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        const auto tilemap = tilemap_chunk_owner->snapshot();
        const auto storage = tilemap_runtime.stats();
        return registry.storage<lux::ecs::TileChunk2DComponent>().size() ==
                spatial2d::kSpatial2DResidentSectionCount &&
            tilemap.ready_chunks ==
                spatial2d::kSpatial2DResidentSectionCount &&
            tilemap.owned_blob_leases ==
                spatial2d::kSpatial2DResidentSectionCount &&
            storage.resident_chunks ==
                spatial2d::kSpatial2DResidentSectionCount &&
            storage.active_chunks ==
                spatial2d::kSpatial2DActiveSectionCount;
    });
    for (const auto entity : tile_window)
        registry.destroy(entity);
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        const auto tilemap = tilemap_chunk_owner->snapshot();
        return registry.storage<lux::ecs::TileChunk2DComponent>().empty() &&
            tilemap.tracked_chunks == 0u &&
            tilemap.owned_blob_leases == 0u &&
            tilemap_runtime.stats().resident_chunks == 0u;
    });
    assert(tilemap_chunk_owner->snapshot()
        .maximum_retirement_granules_per_update == 1u);
    tilemap_chunk_owner->requestClose();
    tickSchedule(schedule, 0.0f);
    assert(tilemap_chunk_owner->closeComplete());
    assert(tilemap_chunk_owner->snapshot().owned_blob_leases == 0u);
    closeOwner(runtime, tilemap_scope.closeAsync());

    // A leaf-domain failure cannot roll back the generic Section. Restore
    // the authored reference afterwards and prove retry rebuilds only Pixel.
    auto pixel_chunk_view =
        registry.view<const lux::ecs::PixelChunk2DComponent>();
    const auto center_chunk = std::find_if(
        pixel_chunk_view.begin(),
        pixel_chunk_view.end(),
        [&](entt::entity entity)
        {
            return registry.get<lux::ecs::PixelChunk2DComponent>(entity)
                .coordinate == lux::spatial::GridCoord2i64{0, 0};
        });
    assert(center_chunk != pixel_chunk_view.end());
    const auto center_chunk_entity = *center_chunk;
    const auto saved_chunk =
        registry.get<lux::ecs::PixelChunk2DComponent>(center_chunk_entity);
    auto missing_content = saved_chunk.content;
    missing_content.id.digest[0] ^= std::byte{0x5au};
    registry.patch<lux::ecs::PixelChunk2DComponent>(
        center_chunk_entity,
        [&missing_content](auto& component) noexcept
        {
            component.content = missing_content;
        });
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        return pixel_owner->snapshot().failed_chunks == 1u;
    });
    assert(partition_owner->snapshot().active_sections ==
        spatial2d::kSpatial2DResidentSectionCount);
    assert(chunkCount(registry) ==
        spatial2d::kSpatial2DResidentSectionCount);
    registry.patch<lux::ecs::PixelChunk2DComponent>(
        center_chunk_entity,
        [&saved_chunk](auto& component) noexcept
        {
            component.content = saved_chunk.content;
        });
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry,
        readyAt({0, 0}));

    tickSchedule(schedule, 1.0f / 60.0f);
    assert(pixel_runtime.stats().simulation_chunks_visited_last_step ==
        spatial2d::kSpatial2DActiveSectionCount);

    const auto moveInterest = [&](lux::spatial::Position2D position)
    {
        registry.patch<lux::ecs::ResolvedTransform2DComponent>(
            interest,
            [position](auto& transform) noexcept
            {
                transform.position = position;
            });
    };

    // -0.25 is the critical floor-vs-truncation boundary: it belongs to -1.
    moveInterest({-0.25, -0.25});
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry,
        readyAt({-1, -1}));
    validateWindow(
        registry, loader_owner->contentBlobs(), {-1, -1}, field_id);

    // 10k cells at 0.25 world units/cell maps to chunk floor(10000/256).
    moveInterest({2500.0, 2500.0});
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry,
        readyAt({39, 39}));
    validateWindow(
        registry, loader_owner->contentBlobs(), {39, 39}, field_id);
    const auto persisted_origin = pixel_persistence.latest({0, 0});
    assert(persisted_origin != nullptr);
    const auto persisted_delta =
        lux::ecs::decodePixelChunkPersistence(*persisted_origin);
    assert(persisted_delta && *persisted_delta == expected_origin_delta);

    // Do not drain main completions while replacing this accepted far
    // request. First stop as soon as at least one Pixel preparation is owned
    // by the background operation; then retire that entity through two ECS
    // barriers before its main-thread completion is adopted. Both generic
    // Section and Pixel-domain completions must be generation-safe.
    constexpr double kFar = 64.0 * 1'000'000.0;
    const auto pixel_stale_before = pixel_owner->snapshot().stale_completions;
    moveInterest({kFar, -kFar});
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        if (pixel_owner->snapshot().background_chunks == 0u)
            return false;
        return std::any_of(
            registry.view<const lux::ecs::PixelChunk2DComponent>().begin(),
            registry.view<const lux::ecs::PixelChunk2DComponent>().end(),
            [&](entt::entity entity)
            {
                return registry.get<lux::ecs::PixelChunk2DComponent>(entity)
                    .coordinate == lux::spatial::GridCoord2i64{
                        1'000'000, -1'000'000};
            });
    });
    moveInterest({-64.0, -64.0});
    tickSchedule(schedule, 0.0f);
    tickSchedule(schedule, 0.0f);
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        return readyAt({-1, -1})() &&
            pixel_owner->snapshot().stale_completions >
                pixel_stale_before;
    });

    moveInterest({-kFar, kFar});
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry,
        readyAt({-1'000'000, 1'000'000}));
    validateWindow(
        registry,
        loader_owner->contentBlobs(),
        {-1'000'000, 1'000'000},
        field_id);

    moveInterest({kFar, -kFar});
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry,
        readyAt({1'000'000, -1'000'000}));
    validateWindow(
        registry,
        loader_owner->contentBlobs(),
        {1'000'000, -1'000'000},
        field_id);
    if (extension)
    {
        assert(extension->checkpoint(
            EInfinite2DCheckpoint::FAR_READY,
            world,
            pixel_runtime,
            field_handle,
            {1'000'000, -1'000'000}));
    }

    // This coordinate never appeared in a manifest or a finite lookup table.
    // The generated record/source path proves the address space is procedural,
    // not a fixture with five pre-populated windows.
    moveInterest({64.0 * 7'654'321.0, 64.0 * -6'543'210.0});
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry,
        readyAt({7'654'321, -6'543'210}));
    validateWindow(
        registry,
        loader_owner->contentBlobs(),
        {7'654'321, -6'543'210},
        field_id);
    assert(partition_owner->snapshot().demand.dynamic_records == 25u);

    moveInterest({0.0, 0.0});
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry,
        readyAt({0, 0}));
    validateWindow(
        registry, loader_owner->contentBlobs(), {0, 0}, field_id);
    lux::ecs::PixelChunkDeltaSnapshot recovered_origin_delta;
    assert(pixel_runtime.captureChunkDelta(
        field_handle, {0, 0}, recovered_origin_delta));
    assert(recovered_origin_delta == expected_origin_delta);
    const auto persistence_snapshot = pixel_persistence.snapshot();
    assert(persistence_snapshot.chunks == 1u);
    assert(persistence_snapshot.captures == 1u);
    assert(pixel_owner->snapshot().persistence_recoveries >= 1u);
    assert(pixel_owner->snapshot().persistence_failures == 0u);
    if (extension)
    {
        assert(extension->checkpoint(
            EInfinite2DCheckpoint::ORIGIN_RECOVERED,
            world,
            pixel_runtime,
            field_handle,
            {0, 0}));
    }

    interest_owner->requestClose();
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        const auto partition_snapshot = partition_owner->snapshot();
        const auto pixel_snapshot = pixel_owner->snapshot();
        return interest_owner->snapshot().closed &&
            partition_snapshot.loader_tickets == 0u &&
            partition_snapshot.demand.dynamic_records == 0u &&
            chunkCount(registry) == 0u &&
            pixel_snapshot.resident_chunks == 0u &&
            pixel_snapshot.active_chunks == 0u &&
            pixel_runtime.stats().resident_chunks == 0u &&
            loader_owner->snapshot().active_sections == 1u;
    });

    field_ticket.reset();
    drive(runtime, schedule, *loader_owner, *partition_owner,
        *interest_owner, *field_owner, *pixel_owner, registry, [&]()
    {
        const auto loaded = loader_owner->snapshot();
        return loaded.active_sections == 0u &&
            loaded.outstanding_tickets == 0u &&
            loaded.blobs.current_bytes == 0u &&
            loaded.blobs.allocation_count == 0u &&
            pixel_runtime.fieldCount() == 0u &&
            tilemap_owner_system->snapshot().live_owned_bindings == 0u;
    });
    tilemap_owner_system->requestClose();
    assert(tilemap_owner_system->closeComplete());

    pixel_owner->requestClose();
    assert(!pixel_owner->closeComplete());
    tickSchedule(schedule, 0.0f);
    closeOwner(runtime, pixel_owner->closeAsync());
    assert(pixel_owner->closeComplete());
    assert(pixel_owner->snapshot().scope_closed);
    assert(pixel_owner->snapshot().closed);
    assert(registry.storage<spatial2d::PixelChunkDomainStateComponent>()
        .size() == 0u);
    assert(pixel_runtime.stats().capturing_chunk_unloads == 0u);
    assert(pixel_owner->snapshot().retirement_granules != 0u);
    assert(pixel_owner->snapshot().maximum_retirement_granules_per_update ==
        1u);
    if (extension)
        extension->shutdown(world);
    g_test_extension = nullptr;
    g_tilemap_chunk_system = nullptr;
    loader_owner->requestClose();
    tickSchedule(schedule, 0.0f);
    closeOwner(runtime, loader_owner->closeAsync());
    pixel_prepare_service->close();
    tilemap_prepare_service->close();
    section_service->close();
    const auto closed = lux::exec::testing::closeRuntime(runtime);
    assert(closed.clean());
    return 0;
}
