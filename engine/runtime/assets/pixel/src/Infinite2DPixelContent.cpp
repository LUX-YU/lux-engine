#include <lux/engine/runtime/assets/pixel/Infinite2DPixelContent.hpp>

#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/ecs/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>

namespace lux::runtime::assets::pixel
{
    namespace
    {
        inline constexpr std::uint32_t kParameterMagic = 0x3250584cu;
        inline constexpr std::uint32_t kContentMagic = 0x3243584cu;

        struct ChunkParameters final
        {
            lux::math::GridCoord2i64 coordinate;
        };

        [[nodiscard]] std::vector<std::byte> encodeParameters(
            lux::math::GridCoord2i64 coordinate)
        {
            std::vector<std::byte> result;
            lux::serialize::ArchiveWriter writer{result};
            writer.writePod(kParameterMagic);
            writer.writePod(kInfinite2DPixelChunkContentVersion);
            writer.writePod(coordinate.x);
            writer.writePod(coordinate.y);
            return result;
        }

        [[nodiscard]] bool decodeParameters(
            std::span<const std::byte> bytes,
            ChunkParameters& result) noexcept
        {
            lux::serialize::ArchiveReader reader{bytes.data(), bytes.size()};
            const auto magic = reader.readPod<std::uint32_t>();
            const auto version = reader.readPod<std::uint32_t>();
            result.coordinate.x = reader.readPod<std::int64_t>();
            result.coordinate.y = reader.readPod<std::int64_t>();
            return reader.ok() && reader.eof() && magic == kParameterMagic &&
                version == kInfinite2DPixelChunkContentVersion;
        }

        [[nodiscard]] lux::ecs::scene_format::EntitySectionId makeSectionId(
            const Infinite2DPixelSectionConfig& config,
            lux::math::GridCoord2i64 coordinate)
        {
            std::vector<std::byte> identity;
            lux::serialize::ArchiveWriter writer{identity};
            writer.writeString(config.generator.name());
            writer.writeUuid(config.field.value());
            writer.writePod(config.seed);
            writer.writePod(coordinate.x);
            writer.writePod(coordinate.y);
            writer.writePod(config.foreground_material);
            writer.writePod(config.landmark_material);
            writer.writePod(config.sand_material);
            writer.writePod(config.water_material);
            writer.writePod(config.player_material);
            const auto digest = lux::cxx::algorithm::Sha256::hash(identity);
            std::array<std::uint8_t, 16u> bytes{};
            std::memcpy(bytes.data(), digest.data(), bytes.size());
            // RFC-4122 variant/version bits make diagnostics and external
            // UUID tools render the deterministic identity conventionally.
            bytes[6] = static_cast<std::uint8_t>(
                (bytes[6] & 0x0fu) | 0x50u);
            bytes[8] = static_cast<std::uint8_t>(
                (bytes[8] & 0x3fu) | 0x80u);
            return lux::ecs::scene_format::EntitySectionId{uuids::uuid{bytes}};
        }

        [[nodiscard]] std::vector<std::byte> encodeCoordinate(
            lux::math::GridCoord2i64 coordinate)
        {
            std::vector<std::byte> nested;
            lux::serialize::ArchiveWriter nested_writer{nested};
            nested_writer.writePod<std::uint32_t>(4u); // x
            nested_writer.writePod<std::uint8_t>(static_cast<std::uint8_t>(
                lux::ecs::serialization::EArchiveType::Int64));
            nested_writer.writePod<std::uint32_t>(sizeof(coordinate.x));
            nested_writer.writePod(coordinate.x);
            nested_writer.writePod<std::uint32_t>(5u); // y
            nested_writer.writePod<std::uint8_t>(static_cast<std::uint8_t>(
                lux::ecs::serialization::EArchiveType::Int64));
            nested_writer.writePod<std::uint32_t>(sizeof(coordinate.y));
            nested_writer.writePod(coordinate.y);
            nested_writer.writePod(lux::ecs::serialization::kEndOfObject);

            std::vector<std::byte> result;
            lux::serialize::ArchiveWriter writer{result};
            writer.writePod<std::uint32_t>(2u); // coordinate
            writer.writePod<std::uint8_t>(static_cast<std::uint8_t>(
                lux::ecs::serialization::EArchiveType::Struct));
            writer.writePod<std::uint32_t>(
                static_cast<std::uint32_t>(nested.size()));
            writer.writeBytes(nested.data(), nested.size());
            writer.writePod(lux::ecs::serialization::kEndOfObject);
            return result;
        }

        [[nodiscard]] std::vector<std::byte> encodeContent(
            const Infinite2DPixelSectionConfig& config,
            lux::math::GridCoord2i64 coordinate)
        {
            std::vector<std::byte> result;
            lux::serialize::ArchiveWriter writer{result};
            writer.writePod(kContentMagic);
            writer.writePod(kInfinite2DPixelChunkContentVersion);
            writer.writePod(coordinate.x);
            writer.writePod(coordinate.y);
            writer.writePod(config.seed);
            writer.writePod(config.foreground_material);
            writer.writePod(config.landmark_material);
            writer.writePod(config.sand_material);
            writer.writePod(config.water_material);
            writer.writePod(config.player_material);
            return result;
        }

        [[nodiscard]] lux::ecs::scene_format::EntitySectionImage makeImage(
            const Infinite2DPixelSectionConfig& config,
            lux::ecs::scene_format::EntitySectionId section,
            lux::math::GridCoord2i64 coordinate)
        {
            using namespace lux::ecs::scene_format;
            EntitySectionImage image;
            image.section = section;
            image.component_names = {
                "", "content", "coordinate", "field", "x", "y"};
            image.schemas.push_back({
                config.chunk_schema,
                1u,
                EEntityComponentStorage::DATA});
            image.archetypes.push_back({{0u}});
            image.entities.push_back({0u, std::nullopt});

            auto coordinate_payload = encodeCoordinate(coordinate);
            image.columns.push_back(EntitySectionComponentColumn{
                0u,
                0u,
                {0u, static_cast<std::uint32_t>(
                    coordinate_payload.size())},
                std::move(coordinate_payload)});
            image.persistent_reference_relocations.push_back({
                0u,
                0u,
                3u,
                config.field});

            EntitySectionAttachment attachment;
            attachment.reference.type = config.content_type;
            attachment.reference.schema_version =
                kInfinite2DPixelChunkContentVersion;
            attachment.payload = encodeContent(config, coordinate);
            attachment.reference.id = makeContentBlobId(
                attachment.reference.type,
                attachment.reference.schema_version,
                attachment.payload);
            image.attachments.push_back(std::move(attachment));
            image.blob_relocations.push_back({0u, 0u, 1u, 0u});
            return image;
        }

        [[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept
        {
            value ^= value >> 30u;
            value *= 0xbf58476d1ce4e5b9ull;
            value ^= value >> 27u;
            value *= 0x94d049bb133111ebull;
            return value ^ (value >> 31u);
        }
    }

    struct Infinite2DPixelSectionSource::State final
    {
        explicit State(Infinite2DPixelSectionConfig value) noexcept
            : config(std::move(value))
        {}

        Infinite2DPixelSectionConfig config;
    };

    bool Infinite2DPixelSectionConfig::valid() const noexcept
    {
        return !field.empty() &&
            lux::ecs::scene_format::isValidSectionGeneratorId(generator) &&
            lux::ecs::isValidComponentSchemaId(chunk_schema) &&
            lux::ecs::scene_format::isValidStableId(content_type) &&
            lux::ecs::scene_format::isValidDemandChannelId(demand_channel) &&
            foreground_material != lux::ecs::kEmptyMaterial &&
            landmark_material != lux::ecs::kEmptyMaterial &&
            sand_material != lux::ecs::kEmptyMaterial &&
            water_material != lux::ecs::kEmptyMaterial &&
            player_material != lux::ecs::kEmptyMaterial;
    }

    lux::cxx::expected<
        Infinite2DPixelSectionSource,
        Infinite2DPixelContentFailure>
    Infinite2DPixelSectionSource::create(
        Infinite2DPixelSectionConfig config)
    {
        if (!config.valid())
        {
            return lux::cxx::unexpected(Infinite2DPixelContentFailure{
                EInfinite2DPixelContentError::INVALID_CONFIG});
        }
        return Infinite2DPixelSectionSource{
            std::make_shared<const State>(std::move(config))};
    }

    lux::runtime::entity_scene::EntitySectionGeneratorDescriptor
    Infinite2DPixelSectionSource::generatorDescriptor() const
    {
        using namespace lux::runtime::entity_scene;
        EntitySectionGeneratorDescriptor result;
        result.id = state_->config.generator;
        result.state = std::shared_ptr<const void>{state_};
        result.generate = [](
            const void* opaque,
            GeneratedEntitySectionRequest request) noexcept
            -> lux::cxx::expected<
                lux::ecs::scene_format::EntitySectionImage,
                EntitySectionGeneratorFailure>
        {
            const auto& state = *static_cast<const State*>(opaque);
            const auto* source = std::get_if<
                lux::ecs::scene_format::GeneratedSectionSource>(
                    &request.record.source);
            ChunkParameters parameters;
            if (!source || source->generator != state.config.generator ||
                source->seed != state.config.seed ||
                !decodeParameters(source->parameters, parameters))
            {
                return lux::cxx::unexpected(
                    EntitySectionGeneratorFailure{
                        EEntitySectionGeneratorError::GENERATION_FAILED,
                        state.config.generator,
                        {},
                        "invalid infinite-2d Pixel Section source"});
            }
            const auto expected = makeSectionId(
                state.config, parameters.coordinate);
            if (request.record.id != expected)
            {
                return lux::cxx::unexpected(
                    EntitySectionGeneratorFailure{
                        EEntitySectionGeneratorError::GENERATION_FAILED,
                        state.config.generator,
                        {},
                        "infinite-2d Pixel Section identity mismatch"});
            }
            return makeImage(
                state.config,
                request.record.id,
                parameters.coordinate);
        };
        return result;
    }

    lux::cxx::expected<
        lux::ecs::scene_format::SectionRecord,
        Infinite2DPixelContentFailure>
    Infinite2DPixelSectionSource::record(
        lux::math::GridCoord2i64 coordinate) const
    {
        lux::ecs::scene_format::SectionRecord result;
        result.id = makeSectionId(state_->config, coordinate);
        result.source = lux::ecs::scene_format::GeneratedSectionSource{
            state_->config.generator,
            state_->config.seed,
            encodeParameters(coordinate)};
        auto image = makeImage(
            state_->config,
            result.id,
            coordinate);
        auto encoded = lux::ecs::scene_format::encodeEntitySectionImage(
            image);
        if (!encoded)
        {
            return lux::cxx::unexpected(Infinite2DPixelContentFailure{
                EInfinite2DPixelContentError::ENCODE_FAILED,
                coordinate});
        }
        result.content_digest =
            lux::ecs::scene_format::entitySectionContentDigest(*encoded);
        result.compression = lux::ecs::scene_format::SectionCompression::NONE;
        result.encoded_bytes = encoded->size();
        result.decoded_bytes = encoded->size();
        result.entity_count = 1u;
        result.demand_channels.push_back(state_->config.demand_channel);
        result.required_components.push_back({
            state_->config.chunk_schema,
            1u});
        return result;
    }

    lux::ecs::spatial2d::streaming::Spatial2DSectionRecordFactory
    Infinite2DPixelSectionSource::recordFactory() const
    {
        auto state = state_;
        return [state = std::move(state)](
            lux::math::GridCoord2i64 coordinate)
            -> lux::cxx::expected<
                lux::ecs::scene_format::SectionRecord,
                lux::ecs::spatial2d::streaming::Spatial2DIndexFailure>
        {
            Infinite2DPixelSectionSource source{state};
            auto record = source.record(coordinate);
            if (!record)
            {
                return lux::cxx::unexpected(
                    lux::ecs::spatial2d::streaming::Spatial2DIndexFailure{
                        .code = lux::ecs::spatial2d::streaming::
                            ESpatial2DIndexError::INVALID_SECTION,
                        .coordinate = coordinate});
            }
            return std::move(*record);
        };
    }

    const Infinite2DPixelSectionConfig&
    Infinite2DPixelSectionSource::config() const noexcept
    {
        return state_->config;
    }

    lux::cxx::expected<
        lux::ecs::PixelChunkLoad,
        Infinite2DPixelContentFailure>
    decodeInfinite2DPixelChunk(
        const lux::ecs::entity_scene::ContentBlobLease& content)
    {
        if (!content)
        {
            return lux::cxx::unexpected(Infinite2DPixelContentFailure{
                EInfinite2DPixelContentError::INVALID_SOURCE});
        }
        return decodeInfinite2DPixelChunk(
            content.bytes(), content.reference());
    }

    lux::cxx::expected<
        lux::ecs::PixelChunkLoad,
        Infinite2DPixelContentFailure>
    decodeInfinite2DPixelChunk(
        lux::cxx::SharedBytes<> bytes,
        lux::ecs::scene_format::ContentBlobRef reference)
    {
        if (bytes.empty() || !reference.valid())
        {
            return lux::cxx::unexpected(Infinite2DPixelContentFailure{
                EInfinite2DPixelContentError::INVALID_SOURCE});
        }
        lux::serialize::ArchiveReader reader{
            bytes.data(), bytes.size()};
        const auto magic = reader.readPod<std::uint32_t>();
        const auto version = reader.readPod<std::uint32_t>();
        lux::math::GridCoord2i64 coordinate;
        coordinate.x = reader.readPod<std::int64_t>();
        coordinate.y = reader.readPod<std::int64_t>();
        const auto seed = reader.readPod<std::uint64_t>();
        const auto foreground = reader.readPod<lux::ecs::MaterialId>();
        const auto landmark = reader.readPod<lux::ecs::MaterialId>();
        const auto sand = reader.readPod<lux::ecs::MaterialId>();
        const auto water = reader.readPod<lux::ecs::MaterialId>();
        const auto player = reader.readPod<lux::ecs::MaterialId>();
        if (!reader.ok() || !reader.eof() || magic != kContentMagic ||
            version != kInfinite2DPixelChunkContentVersion ||
            reference.schema_version != version ||
            foreground == lux::ecs::kEmptyMaterial ||
            landmark == lux::ecs::kEmptyMaterial ||
            sand == lux::ecs::kEmptyMaterial ||
            water == lux::ecs::kEmptyMaterial ||
            player == lux::ecs::kEmptyMaterial ||
            lux::ecs::scene_format::makeContentBlobId(
                reference.type,
                reference.schema_version,
                bytes.view()) != reference.id)
        {
            return lux::cxx::unexpected(Infinite2DPixelContentFailure{
                EInfinite2DPixelContentError::INVALID_PARAMETERS,
                coordinate});
        }

        lux::ecs::PixelChunkLoad result;
        result.coordinate = coordinate;
        result.materials.resize(
            lux::ecs::PixelFieldRuntime::kChunkCellCount,
            lux::ecs::kEmptyMaterial);
        for (std::uint32_t y = 0u;
             y < lux::ecs::PixelFieldRuntime::kChunkSizeCells;
             ++y)
        {
            for (std::uint32_t x = 0u;
                 x < lux::ecs::PixelFieldRuntime::kChunkSizeCells;
                 ++x)
            {
                const auto cell = static_cast<std::size_t>(y) *
                    lux::ecs::PixelFieldRuntime::kChunkSizeCells + x;
                const auto key = mix(seed ^
                    mix(static_cast<std::uint64_t>(coordinate.x)) ^
                    (mix(static_cast<std::uint64_t>(coordinate.y)) << 1u) ^
                    (static_cast<std::uint64_t>(x / 16u) << 32u) ^
                    static_cast<std::uint64_t>(y / 16u));
                // The compact source expands into a deterministic checker /
                // cave terrain. Simulation materials live in sealed,
                // periodically repeated basins so the same fixture proves
                // sand/water evolution without either material escaping the
                // active window. Landmarks and the player marker remain
                // visually identifiable at every streamed coordinate.
                if (((x / 16u + y / 16u) & 1u) != 0u ||
                    key % 11u == 0u)
                {
                    result.materials[cell] = foreground;
                }

                constexpr std::uint32_t kPatternEdge = 128u;
                const auto pattern_x = x % kPatternEdge;
                const auto pattern_y = y % kPatternEdge;
                const bool sand_floor = pattern_y == 10u &&
                    pattern_x >= 10u && pattern_x <= 46u;
                const bool sand_wall =
                    (pattern_x == 10u || pattern_x == 46u) &&
                    pattern_y >= 10u && pattern_y <= 34u;
                const bool sand_interior = pattern_x > 10u &&
                    pattern_x < 46u && pattern_y > 10u &&
                    pattern_y < 34u;
                const bool water_floor = pattern_y == 10u &&
                    pattern_x >= 74u && pattern_x <= 118u;
                const bool water_wall =
                    (pattern_x == 74u || pattern_x == 118u) &&
                    pattern_y >= 10u && pattern_y <= 34u;
                const bool water_interior = pattern_x > 74u &&
                    pattern_x < 118u && pattern_y > 10u &&
                    pattern_y < 34u;

                if (sand_interior || water_interior)
                    result.materials[cell] = lux::ecs::kEmptyMaterial;
                if (sand_floor || sand_wall || water_floor || water_wall)
                    result.materials[cell] = foreground;
                if (sand_interior && pattern_y <= 28u)
                    result.materials[cell] = sand;
                if (water_interior && pattern_y <= 24u)
                    result.materials[cell] = water;

                const auto landmark_x =
                    static_cast<std::int32_t>(pattern_x) - 26;
                const auto landmark_y =
                    static_cast<std::int32_t>(pattern_y) - 72;
                if (std::abs(landmark_x) + std::abs(landmark_y) <= 8)
                    result.materials[cell] = landmark;

                const bool player_vertical = pattern_x >= 91u &&
                    pattern_x <= 96u && pattern_y >= 69u &&
                    pattern_y <= 91u;
                const bool player_horizontal = pattern_x >= 84u &&
                    pattern_x <= 103u && pattern_y >= 76u &&
                    pattern_y <= 82u;
                if (player_vertical || player_horizontal)
                    result.materials[cell] = player;
            }
        }
        result.base_digest = reference.id.digest;
        result.presentation_active = false;
        result.simulation_active = false;
        return result;
    }
} // namespace lux::runtime::assets::pixel
