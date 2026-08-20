#include <lux/engine/toolchain/spatial3d_scene/Spatial3DEntitySceneAdapter.hpp>
#include <lux/engine/toolchain/spatial3d_scene/detail/Spatial3DNavigationCook.hpp>

#include <lux/engine/toolchain/entity_scene/EntitySectionImageBuilder.hpp>
#include <lux/engine/toolchain/entity_scene/TaggedPayloadTranscoder.hpp>

#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/NameTable.hpp>
#include <lux/engine/core/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/ecs/TypeToken.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/render/components/3d/ClassicMeshBatchComponent.hpp>
#include <lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/HeightFogComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SkyboxComponent.hpp>
#include <lux/engine/ecs/render/components/3d/VisualLodNodeComponent.hpp>
#include <lux/engine/ecs/navigation/components/NavigationRegion3DComponent.hpp>
#include <lux/engine/ecs/physics3d/components/Physics3DComponents.hpp>
#include <lux/engine/ecs/spatial3d/components/SpatialInterest3DComponent.hpp>
#include <lux/engine/ecs/terrain/components/TerrainTileComponent.hpp>
#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>
#include <lux/engine/function/render/standard/content/ClassicMeshBatch.hpp>
#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/MeshSerDeser.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/resource/asset/codecs/StaticColliderBatch3DCodec.hpp>
#include <lux/engine/spatial3d/SceneCatalog.hpp>
#include <lux/engine/resource/asset/codecs/TerrainTileCodec.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <iterator>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace lux::toolchain
{
    namespace
    {
        using AdapterError = ESpatial3DEntitySceneAdapterError;
        using AdapterFailure = Spatial3DEntitySceneAdapterFailure;
        inline constexpr std::uint32_t kMaximumVisualLodChildren = 16u;
        inline constexpr std::uint32_t kMaximumVisualLodLevels = 3u;

        [[nodiscard]] AdapterFailure failure(
            AdapterError code,
            std::string detail)
        {
            return {code, std::move(detail), std::nullopt, std::nullopt};
        }

        [[nodiscard]] AdapterFailure entityFailure(
            EntitySceneCookFailure cause,
            std::string detail)
        {
            AdapterFailure result{
                AdapterError::ENTITY_SCENE_COOK_REJECTED,
                std::move(detail)};
            result.entity_scene = std::move(cause);
            return result;
        }

        [[nodiscard]] std::string uuidKey(const uuids::uuid& value)
        {
            return uuids::to_string(value);
        }

        [[nodiscard]] lux::spatial3d::SourceId
        spatialSourceId(const uuids::uuid& space)
        {
            auto identity = uuidKey(space);
            identity.erase(
                std::remove(identity.begin(), identity.end(), '-'),
                identity.end());
            return lux::spatial3d::SourceId{
                "lux.spatial3d.source." + identity};
        }

        [[nodiscard]] bool validSectionContentPrefix(
            std::string_view value) noexcept
        {
            if (value.size() <= 1u || value.size() > 980u ||
                value.front() != '/')
            {
                return false;
            }
            if (value.back() == '/')
                value.remove_suffix(1u);
            if (value.size() <= 1u || value.back() == '/')
                return false;
            bool segment_has_character = false;
            for (std::size_t index = 1u; index < value.size(); ++index)
            {
                const auto character = static_cast<unsigned char>(value[index]);
                if (character == '/')
                {
                    if (!segment_has_character)
                        return false;
                    segment_has_character = false;
                    continue;
                }
                if (character < 0x20u || character == 0x7fu ||
                    value[index] == '\\' || value[index] == '.' ||
                    value[index] == ':' || value[index] == '"' ||
                    value[index] == '|' || value[index] == '?' ||
                    value[index] == '*' || value[index] == '<' ||
                    value[index] == '>')
                {
                    return false;
                }
                segment_has_character = true;
            }
            return segment_has_character;
        }

        [[nodiscard]] lux::ecs::scene_format::EntitySectionId derivedSectionId(
            const lux::scene::ScenePackageId& scene,
            std::string_view purpose)
        {
            std::vector<std::byte> identity;
            lux::serialize::ArchiveWriter writer{identity};
            writer.writeUuid(scene.value());
            writer.writeString(purpose);
            const auto digest = lux::cxx::algorithm::Sha256::hash(identity);
            std::array<std::uint8_t, 16u> bytes{};
            std::memcpy(bytes.data(), digest.data(), bytes.size());
            bytes[6] = static_cast<std::uint8_t>(
                (bytes[6] & 0x0fu) | 0x50u);
            bytes[8] = static_cast<std::uint8_t>(
                (bytes[8] & 0x3fu) | 0x80u);
            return lux::ecs::scene_format::EntitySectionId{uuids::uuid{bytes}};
        }

        using AttachmentKey = std::tuple<
            std::string,
            std::uint32_t,
            lux::cxx::algorithm::Sha256Digest>;

        struct EntitySectionAssembly final
        {
            explicit EntitySectionAssembly(
                lux::ecs::scene_format::EntitySectionId value)
                : id(value),
                  builder(value)
            {}

            lux::ecs::scene_format::EntitySectionId id;
            EntitySectionImageBuilder builder;
            std::map<AttachmentKey, std::uint32_t> attachments;
        };

        [[nodiscard]] lux::cxx::expected<std::uint32_t, AdapterFailure>
        internAttachment(
            EntitySectionAssembly& section,
            lux::ecs::scene_format::ContentTypeId type,
            std::uint32_t schema_version,
            std::vector<std::byte> payload)
        {
            const auto content = lux::ecs::scene_format::makeContentBlobId(
                type, schema_version, payload);
            AttachmentKey key{
                type.name(), schema_version, content.digest};
            if (const auto found = section.attachments.find(key);
                found != section.attachments.end())
            {
                return found->second;
            }
            const auto added = section.builder.addAttachment({
                std::move(type), schema_version, std::move(payload)});
            if (!added)
            {
                return lux::cxx::unexpected(entityFailure(
                    added.error(),
                    "cannot add a 3D domain attachment to LXES"));
            }
            section.attachments.emplace(std::move(key), *added);
            return *added;
        }

        [[nodiscard]] lux::navigation::NavigationRegionId
        navigationRegionId(
            const lux::ecs::scene_format::EntitySectionId& section,
            std::uint8_t profile)
        {
            std::vector<std::byte> identity;
            lux::serialize::ArchiveWriter writer{identity};
            writer.writeUuid(section.value());
            writer.writePod(profile);
            const auto digest = lux::cxx::algorithm::Sha256::hash(identity);
            lux::navigation::NavigationRegionId result;
            for (std::size_t index = 0u; index < 8u; ++index)
            {
                result.high = (result.high << 8u) |
                    std::to_integer<std::uint64_t>(digest[index]);
                result.low =
                    (result.low << 8u) |
                    std::to_integer<std::uint64_t>(digest[index + 8u]);
            }
            return result;
        }

        enum class ENavigationPortalBoundary : std::uint8_t
        {
            POSITIVE_X,
            POSITIVE_Z
        };

        [[nodiscard]] double portalNormal(
            const lux::math::Position3d& point,
            ENavigationPortalBoundary boundary) noexcept
        {
            return boundary == ENavigationPortalBoundary::POSITIVE_X
                ? point.x
                : point.z;
        }

        [[nodiscard]] double portalTangent(
            const lux::math::Position3d& point,
            ENavigationPortalBoundary boundary) noexcept
        {
            return boundary == ENavigationPortalBoundary::POSITIVE_X
                ? point.z
                : point.x;
        }

        [[nodiscard]] lux::navigation::NavigationPortalId
        navigationPortalId(
            const lux::navigation::NavigationRegionId& first,
            const lux::navigation::NavigationRegionId& second,
            ENavigationPortalBoundary boundary)
        {
            std::vector<std::byte> identity;
            lux::serialize::ArchiveWriter writer{identity};
            writer.writeString("lux.navigation.portal3d.v1");
            writer.writePod(first.high);
            writer.writePod(first.low);
            writer.writePod(second.high);
            writer.writePod(second.low);
            writer.writePod(static_cast<std::uint8_t>(boundary));
            const auto digest = lux::cxx::algorithm::Sha256::hash(identity);
            lux::navigation::NavigationPortalId result;
            for (std::size_t index = 0u; index < 8u; ++index)
            {
                result.high = (result.high << 8u) |
                    std::to_integer<std::uint64_t>(digest[index]);
                result.low =
                    (result.low << 8u) |
                    std::to_integer<std::uint64_t>(digest[index + 8u]);
            }
            if (!result.valid())
                result.low = 1u;
            return result;
        }

        /// Finds a real pair of points on the cooked traversable boundaries.
        /// The authored Cell seam is only used to select candidates; the
        /// portal endpoints themselves always come from navigation geometry.
        [[nodiscard]] std::optional<lux::navigation::NavigationPortal>
        navigationPortal(
            const lux::navigation::detour3d::
                NavigationRegion3DDescription& first,
            const lux::navigation::detour3d::
                NavigationRegion3DDescription& second,
            ENavigationPortalBoundary boundary,
            double seam) noexcept
        {
            if (!std::isfinite(seam) || first.agent != second.agent)
                return std::nullopt;

            const auto maximum_seam_distance = std::max({
                2.0,
                static_cast<double>(first.agent.radius) * 2.0 +
                    static_cast<double>(first.horizontal_resolution) * 4.0,
                static_cast<double>(second.agent.radius) * 2.0 +
                    static_cast<double>(second.horizontal_resolution) * 4.0});
            const auto candidate_slack = std::max(
                1.0e-4,
                static_cast<double>(std::max(
                    first.horizontal_resolution,
                    second.horizontal_resolution)) * 2.0);

            const auto nearestDistance = [boundary, seam](
                const auto& description) noexcept
            {
                auto nearest = (std::numeric_limits<double>::max)();
                for (const auto& area : description.areas)
                {
                    for (const auto& point : area.boundary)
                    {
                        nearest = std::min(
                            nearest,
                            std::abs(portalNormal(point, boundary) - seam));
                    }
                }
                return nearest;
            };
            const auto first_distance = nearestDistance(first);
            const auto second_distance = nearestDistance(second);
            if (first_distance > maximum_seam_distance ||
                second_distance > maximum_seam_distance)
            {
                return std::nullopt;
            }

            std::vector<lux::math::Position3d> first_candidates;
            std::vector<lux::math::Position3d> second_candidates;
            const auto collectCandidates = [
                boundary,
                seam,
                candidate_slack](
                const auto& description,
                double nearest,
                auto& output)
            {
                for (const auto& area : description.areas)
                {
                    for (const auto& point : area.boundary)
                    {
                        if (std::abs(portalNormal(point, boundary) - seam) <=
                            nearest + candidate_slack)
                        {
                            output.push_back(point);
                        }
                    }
                }
            };
            collectCandidates(first, first_distance, first_candidates);
            collectCandidates(second, second_distance, second_candidates);
            if (first_candidates.empty() || second_candidates.empty())
                return std::nullopt;

            const auto maximum_tangent_gap = std::max(
                2.0,
                static_cast<double>(std::max(
                    first.horizontal_resolution,
                    second.horizontal_resolution)) * 8.0);
            const auto maximum_height_gap = std::max(
                0.5,
                static_cast<double>(std::max(
                    first.agent.maximum_climb,
                    second.agent.maximum_climb)) +
                    static_cast<double>(std::max(
                        first.vertical_resolution,
                        second.vertical_resolution)) * 2.0);
            const lux::math::Position3d* selected_first = nullptr;
            const lux::math::Position3d* selected_second = nullptr;
            auto selected_score = (std::numeric_limits<double>::max)();
            for (const auto& first_point : first_candidates)
            {
                for (const auto& second_point : second_candidates)
                {
                    const auto tangent_gap = std::abs(
                        portalTangent(first_point, boundary) -
                        portalTangent(second_point, boundary));
                    const auto height_gap = std::abs(
                        first_point.y - second_point.y);
                    if (tangent_gap > maximum_tangent_gap ||
                        height_gap > maximum_height_gap)
                    {
                        continue;
                    }
                    const auto score = tangent_gap * tangent_gap +
                        height_gap * height_gap +
                        std::abs(portalNormal(first_point, boundary) - seam) +
                        std::abs(portalNormal(second_point, boundary) - seam);
                    const auto positions = std::tuple{
                        first_point.x,
                        first_point.y,
                        first_point.z,
                        second_point.x,
                        second_point.y,
                        second_point.z
                    };
                    const auto selected_positions = selected_first
                        ? std::tuple{
                              selected_first->x,
                              selected_first->y,
                              selected_first->z,
                              selected_second->x,
                              selected_second->y,
                              selected_second->z
                          }
                        : decltype(positions){};
                    if (!selected_first || score < selected_score ||
                        (score == selected_score &&
                         positions < selected_positions))
                    {
                        selected_first = &first_point;
                        selected_second = &second_point;
                        selected_score = score;
                    }
                }
            }
            if (!selected_first || !selected_second)
                return std::nullopt;

            lux::navigation::NavigationPortal result;
            result.id = navigationPortalId(
                first.region, second.region, boundary);
            result.first_region = first.region;
            result.second_region = second.region;
            result.first_position = *selected_first;
            result.second_position = *selected_second;
            return result;
        }

        [[nodiscard]] std::vector<std::string> namesOf(
            const lux::serialize::NameTable& names)
        {
            std::vector<std::string> result;
            result.reserve(names.size());
            for (std::uint32_t index = 0u; index < names.size(); ++index)
                result.emplace_back(names.at(index));
            return result;
        }

        [[nodiscard]] lux::cxx::expected<
            TaggedPayloadSource,
            AdapterFailure>
        exactPayload(
            const lux::ecs::ComponentSchemaDescriptor& descriptor,
            const void* component)
        {
            if (!descriptor.ref_class || !component)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::COMPONENT_SCHEMA_MISMATCH,
                    "component schema cannot produce an exact tagged payload: " +
                        descriptor.schema_id.name));
            }
            TaggedPayloadSource result;
            lux::serialize::NameTable names;
            lux::serialize::ArchiveWriter writer{result.payload};
            lux::serialize::TaggedPropertyWriter tagged{writer, names};
            tagged.writeObject(*descriptor.ref_class, component);
            result.names = namesOf(names);
            return result;
        }

        [[nodiscard]] const lux::ecs::ComponentSchemaDescriptor*
        checkedSchema(
            const lux::ecs::ComponentTypeCatalog& components,
            std::string_view schema,
            std::uint32_t version,
            AdapterFailure& error)
        {
            const auto* descriptor = components.findBySchema(schema);
            if (!descriptor)
            {
                error = failure(
                    AdapterError::MISSING_COMPONENT_SCHEMA,
                    "Toolchain component catalog is missing schema '" +
                        std::string{schema} + "'");
                return nullptr;
            }
            if (descriptor->schema_id.name != schema ||
                descriptor->schema_version != version ||
                descriptor->serialization !=
                    lux::ecs::EComponentSerializationPolicy::COOKED ||
                !descriptor->ref_class || !descriptor->operations.emplace ||
                !descriptor->operations.get)
            {
                error = failure(
                    AdapterError::COMPONENT_SCHEMA_MISMATCH,
                    "Toolchain component schema is not a cooked, materializable v" +
                        std::to_string(version) + " contract: " +
                        std::string{schema});
                return nullptr;
            }
            return descriptor;
        }

        template <class Component>
        [[nodiscard]] lux::cxx::expected<
            EntityComponentCookInput,
            AdapterFailure>
        typedComponent(
            const Component& value,
            const lux::ecs::ComponentTypeCatalog& components)
        {
            const auto* descriptor = components.findByType(
                lux::ecs::typeToken<Component>());
            if (!descriptor)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::MISSING_COMPONENT_SCHEMA,
                    "Toolchain component catalog is missing reflected type '" +
                        std::string{lux::ecs::typeToken<Component>().name} +
                        "'"));
            }
            AdapterFailure ignored;
            descriptor = checkedSchema(
                components,
                descriptor->schema_id.name,
                descriptor->schema_version,
                ignored);
            if (!descriptor)
                return lux::cxx::unexpected(std::move(ignored));
            auto payload = exactPayload(*descriptor, &value);
            if (!payload)
                return lux::cxx::unexpected(std::move(payload.error()));
            EntityComponentCookInput result;
            result.schema = lux::ecs::componentSchemaId(
                descriptor->schema_id.name);
            result.schema_version = descriptor->schema_version;
            result.value = std::move(*payload);
            return result;
        }

        template <class Tag>
        [[nodiscard]] lux::cxx::expected<
            EntityComponentCookInput,
            AdapterFailure>
        typedTag(const lux::ecs::ComponentTypeCatalog& components)
        {
            const auto* descriptor = components.findByType(
                lux::ecs::typeToken<Tag>());
            if (!descriptor || descriptor->schema_version == 0u ||
                descriptor->serialization !=
                    lux::ecs::EComponentSerializationPolicy::COOKED)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::MISSING_COMPONENT_SCHEMA,
                    "Toolchain component catalog is missing cooked tag '" +
                        std::string{lux::ecs::typeToken<Tag>().name} + "'"));
            }
            EntityComponentCookInput result;
            result.schema = lux::ecs::componentSchemaId(
                descriptor->schema_id.name);
            result.schema_version = descriptor->schema_version;
            result.storage =
                lux::ecs::scene_format::EEntityComponentStorage::TAG;
            return result;
        }

        void setComponent(
            EntityCookInput& entity,
            EntityComponentCookInput component)
        {
            const auto found = std::ranges::find(
                entity.components,
                component.schema.name,
                [](const EntityComponentCookInput& value)
                {
                    return value.schema.name;
                });
            if (found == entity.components.end())
                entity.components.push_back(std::move(component));
            else
                *found = std::move(component);
        }

        struct MaterializedActor final
        {
            EntityCookInput entity;
            bool primary_camera{false};
        };

        [[nodiscard]] bool declaresPrimaryCamera(
            const Spatial3DActorSource& actor)
        {
            const auto primary_schema = lux::ecs::defaultComponentSchemaName(
                lux::ecs::typeToken<lux::ecs::PrimaryCameraTag>().name);
            return std::ranges::any_of(
                actor.components,
                [&primary_schema](const auto& record)
                {
                    return lux::ecs::defaultComponentSchemaName(
                               record.schema_name) == primary_schema;
                });
        }

        [[nodiscard]] bool declaresStartupSceneFact(
            const Spatial3DActorSource& actor)
        {
            const std::array startup_schemas{
                lux::ecs::defaultComponentSchemaName(
                    lux::ecs::typeToken<lux::ecs::PrimaryCameraTag>().name),
                lux::ecs::defaultComponentSchemaName(
                    lux::ecs::typeToken<lux::ecs::SkyboxComponent>().name),
                lux::ecs::defaultComponentSchemaName(
                    lux::ecs::typeToken<
                        lux::ecs::DirectionalLightComponent>().name),
                lux::ecs::defaultComponentSchemaName(
                    lux::ecs::typeToken<lux::ecs::HeightFogComponent>().name
                )
            };
            return std::ranges::any_of(
                actor.components,
                [&startup_schemas](const auto& record)
                {
                    const auto schema =
                        lux::ecs::defaultComponentSchemaName(
                            record.schema_name);
                    return std::ranges::find(startup_schemas, schema) !=
                        startup_schemas.end();
                });
        }

        [[nodiscard]] lux::cxx::expected<MaterializedActor, AdapterFailure>
        materializeActor(
            const Spatial3DActorSource& actor,
            const lux::ecs::ComponentTypeCatalog& components)
        {
            const auto names = decodeTaggedPayloadNameTable(
                actor.name_table);
            if (!names)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::INVALID_COMPONENT_PAYLOAD,
                    "Actor '" + uuidKey(actor.id.value()) +
                        "' has an invalid private tagged NameTable: " +
                        names.error().detail));
            }

            lux::meta::EntityRegistry staging;
            const auto entity = staging.create();
            std::vector<const lux::ecs::ComponentSchemaDescriptor*>
                descriptors;
            std::set<std::string, std::less<>> seen_schemas;
            const auto primary_schema = lux::ecs::defaultComponentSchemaName(
                lux::ecs::typeToken<lux::ecs::PrimaryCameraTag>().name);
            bool primary_camera = false;

            for (const auto& record : actor.components)
            {
                const auto schema = lux::ecs::defaultComponentSchemaName(
                    record.schema_name);
                if (!seen_schemas.insert(schema).second)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::INVALID_ACTOR,
                        "Actor contains duplicate component schema '" +
                            schema + "'"));
                }
                if (schema == primary_schema)
                {
                    primary_camera = true;
                    continue;
                }

                AdapterFailure schema_error;
                const auto* descriptor = checkedSchema(
                    components,
                    schema,
                    record.schema_version,
                    schema_error);
                if (!descriptor)
                    return lux::cxx::unexpected(std::move(schema_error));
                void* value = descriptor->operations.emplace(staging, entity);
                if (!value)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::INVALID_COMPONENT_PAYLOAD,
                        "cannot materialize legacy Actor component '" +
                            schema + "'"));
                }
                lux::serialize::NameTable source_names;
                for (std::size_t index = 1u; index < names->size(); ++index)
                {
                    if (source_names.intern((*names)[index]) != index)
                    {
                        return lux::cxx::unexpected(failure(
                            AdapterError::INVALID_COMPONENT_PAYLOAD,
                            "legacy Actor NameTable changed index while materializing"));
                    }
                }
                lux::serialize::ArchiveReader reader{
                    record.tagged_payload.data(),
                    record.tagged_payload.size()
                };
                lux::serialize::TaggedPropertyReader tagged{
                    reader, source_names
                };
                tagged.readObject(*descriptor->ref_class, value);
                if (!reader.ok() || !reader.eof())
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::INVALID_COMPONENT_PAYLOAD,
                        "legacy Actor component payload is malformed: '" +
                            schema + "'"));
                }
                descriptors.push_back(descriptor);
            }

            const auto ensure = [&]<class Component>()
                -> lux::cxx::expected<
                    const lux::ecs::ComponentSchemaDescriptor*,
                    AdapterFailure>
            {
                const auto* descriptor = components.findByType(
                    lux::ecs::typeToken<Component>());
                if (!descriptor)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::MISSING_COMPONENT_SCHEMA,
                        "Toolchain component catalog is missing '" +
                            std::string{
                                lux::ecs::typeToken<Component>().name} +
                            "'"));
                }
                if (!staging.any_of<Component>(entity))
                {
                    AdapterFailure schema_error;
                    descriptor = checkedSchema(
                        components,
                        descriptor->schema_id.name,
                        descriptor->schema_version,
                        schema_error);
                    if (!descriptor)
                    {
                        return lux::cxx::unexpected(
                            std::move(schema_error));
                    }
                    if (!descriptor->operations.emplace(staging, entity))
                    {
                        return lux::cxx::unexpected(failure(
                            AdapterError::INVALID_COMPONENT_PAYLOAD,
                            "cannot install required Actor component '" +
                                descriptor->schema_id.name + "'"));
                    }
                    descriptors.push_back(descriptor);
                }
                return descriptor;
            };

            const auto transform_descriptor =
                ensure.template operator()<lux::ecs::Transform3DComponent>();
            if (!transform_descriptor)
            {
                return lux::cxx::unexpected(
                    std::move(transform_descriptor.error()));
            }
            if (!actor.transform_parent)
            {
                if (!lux::math::isFinite(actor.position))
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::INVALID_POSITION,
                        "3D Actor root has an invalid absolute position"));
                }
                staging.get<lux::ecs::Transform3DComponent>(entity).position =
                    actor.position;
            }
            if (primary_camera)
            {
                const auto camera =
                    ensure.template operator()<lux::ecs::Camera3DComponent>();
                if (!camera)
                    return lux::cxx::unexpected(std::move(camera.error()));
            }

            std::ranges::sort(
                descriptors,
                {},
                [](const auto* descriptor)
                {
                    return descriptor->schema_id.name;
                });
            descriptors.erase(
                std::unique(
                    descriptors.begin(),
                    descriptors.end(),
                    [](const auto* lhs, const auto* rhs)
                    {
                        return lhs->schema_id.name == rhs->schema_id.name;
                    }),
                descriptors.end());

            MaterializedActor result;
            result.entity.persistent_id =
                lux::ecs::PersistentEntityId{actor.id.value()};
            for (const auto* descriptor : descriptors)
            {
                const void* value = descriptor->operations.get(
                    staging, entity);
                auto payload = exactPayload(*descriptor, value);
                if (!payload)
                    return lux::cxx::unexpected(std::move(payload.error()));
                EntityComponentCookInput component;
                component.schema = lux::ecs::componentSchemaId(
                    descriptor->schema_id.name);
                component.schema_version = descriptor->schema_version;
                component.value = std::move(*payload);
                result.entity.components.push_back(std::move(component));
            }
            result.primary_camera = primary_camera;
            return result;
        }

        [[nodiscard]] lux::cxx::expected<
            lux::terrain::TerrainTileBlobV1,
            AdapterFailure>
        terrainBlob(const Spatial3DTerrainPageSource& source)
        {
            using namespace lux::terrain;
            if (!std::isfinite(source.height_min) ||
                !std::isfinite(source.height_max) ||
                !(source.height_max > source.height_min) ||
                !std::isfinite(source.sample_spacing) ||
                !(source.sample_spacing > 0.0f) ||
                source.weight_layer_count >
                    kTerrainTileMaximumWeightLayers ||
                source.heights.size() != kTerrainTileSampleCount ||
                source.weight_planes[0].size() !=
                    kTerrainTileWeightPlaneBytes ||
                source.weight_planes[1].size() !=
                    kTerrainTileWeightPlaneBytes ||
                source.holes.size() != kTerrainTileHoleBytes)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::TERRAIN_CONTENT_REJECTED,
                    "legacy Terrain Page does not match the v1 tile layout"));
            }

            TerrainTileBlobV1 result;
            result.height_min = source.height_min;
            result.height_max = source.height_max;
            result.sample_spacing = source.sample_spacing;
            result.weight_layer_count = source.weight_layer_count;
            result.weight_planes = source.weight_planes;
            result.holes = source.holes;
            result.holes.back() &= 0x01u;

            result.heights.resize(kTerrainTileSampleCount);
            const auto scale = 65535.0 /
                static_cast<double>(source.height_max - source.height_min);
            for (std::size_t index = 0u;
                 index < source.heights.size();
                 ++index)
            {
                const auto value = source.heights[index];
                if (!std::isfinite(value) || value < source.height_min ||
                    value > source.height_max)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::TERRAIN_CONTENT_REJECTED,
                        "legacy Terrain Page contains an invalid height"));
                }
                result.heights[index] = static_cast<std::uint16_t>(
                    std::llround(
                        static_cast<double>(value - source.height_min) *
                        scale));
            }

            std::uint32_t edge = kTerrainTileQuadEdge;
            std::vector<std::uint16_t> minima(
                static_cast<std::size_t>(edge) * edge);
            std::vector<std::uint16_t> maxima(minima.size());
            for (std::uint32_t y = 0u; y < edge; ++y)
            {
                for (std::uint32_t x = 0u; x < edge; ++x)
                {
                    const auto sample = static_cast<std::size_t>(y) *
                        kTerrainTileSampleEdge + x;
                    const std::array<std::uint16_t, 4u> values{
                        result.heights[sample],
                        result.heights[sample + 1u],
                        result.heights[
                            sample + kTerrainTileSampleEdge],
                        result.heights[
                            sample + kTerrainTileSampleEdge + 1u]
                    };
                    const auto output =
                        static_cast<std::size_t>(y) * edge + x;
                    minima[output] = *std::ranges::min_element(values);
                    maxima[output] = *std::ranges::max_element(values);
                }
            }
            result.min_max_pairs.reserve(
                static_cast<std::size_t>(
                    kTerrainTileMinMaxNodeCount) * 2u);
            while (true)
            {
                for (std::size_t index = 0u; index < minima.size(); ++index)
                {
                    result.min_max_pairs.push_back(minima[index]);
                    result.min_max_pairs.push_back(maxima[index]);
                }
                if (edge == 1u)
                    break;
                const auto parent_edge = edge / 2u;
                std::vector<std::uint16_t> parent_minima(
                    static_cast<std::size_t>(parent_edge) * parent_edge);
                std::vector<std::uint16_t> parent_maxima(
                    parent_minima.size());
                for (std::uint32_t y = 0u; y < parent_edge; ++y)
                {
                    for (std::uint32_t x = 0u; x < parent_edge; ++x)
                    {
                        const auto first =
                            static_cast<std::size_t>(y * 2u) * edge +
                            x * 2u;
                        const std::array<std::uint16_t, 4u> child_minima{
                            minima[first],
                            minima[first + 1u],
                            minima[first + edge],
                            minima[first + edge + 1u]
                        };
                        const std::array<std::uint16_t, 4u> child_maxima{
                            maxima[first],
                            maxima[first + 1u],
                            maxima[first + edge],
                            maxima[first + edge + 1u]
                        };
                        const auto output =
                            static_cast<std::size_t>(y) * parent_edge + x;
                        parent_minima[output] =
                            *std::ranges::min_element(child_minima);
                        parent_maxima[output] =
                            *std::ranges::max_element(child_maxima);
                    }
                }
                minima = std::move(parent_minima);
                maxima = std::move(parent_maxima);
                edge = parent_edge;
            }

            result.parent_fallback_heights.resize(
                kTerrainTileFallbackSampleCount);
            for (std::uint32_t y = 0u;
                 y < kTerrainTileFallbackEdge;
                 ++y)
            {
                for (std::uint32_t x = 0u;
                     x < kTerrainTileFallbackEdge;
                     ++x)
                {
                    result.parent_fallback_heights[
                        static_cast<std::size_t>(y) *
                            kTerrainTileFallbackEdge + x] =
                        result.heights[
                            static_cast<std::size_t>(y * 2u) *
                                kTerrainTileSampleEdge + x * 2u];
                }
            }
            const auto valid = validateTerrainTileBlob(result);
            if (!valid)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::TERRAIN_CONTENT_REJECTED,
                    valid.error().detail));
            }
            return result;
        }

        [[nodiscard]] std::optional<lux::math::GridCoord2i64>
        planarCoordinate(const lux::math::GridCoord3i64& cell) noexcept
        {
            if (cell.y != 0)
                return std::nullopt;
            return lux::math::GridCoord2i64{
                cell.x, cell.z};
        }

        [[nodiscard]] lux::math::GridCoord3i64
        spatialCoordinate(const lux::math::GridCoord3i64& cell) noexcept
        {
            return cell;
        }

        [[nodiscard]] std::optional<lux::math::GridCoord3i64> actorCell(
            const Spatial3DAuthoringSource& source,
            const Spatial3DActorSource& actor) noexcept
        {
            const auto found = std::ranges::find(
                source.spaces,
                actor.space,
                &Spatial3DSourceSpace::id);
            if (found == source.spaces.end() ||
                !lux::math::isFinite(actor.position) ||
                !std::isfinite(found->cell_edge) || found->cell_edge <= 0.0f)
                return std::nullopt;
            const auto coordinate = [edge = static_cast<double>(
                                         found->cell_edge)](double value)
                -> std::optional<std::int64_t>
            {
                const auto result = std::floor(value / edge);
                if (result < static_cast<double>(
                        std::numeric_limits<std::int64_t>::min()) ||
                    result > static_cast<double>(
                        std::numeric_limits<std::int64_t>::max()))
                {
                    return std::nullopt;
                }
                return static_cast<std::int64_t>(result);
            };
            const auto x = coordinate(actor.position.x);
            const auto z = coordinate(actor.position.z);
            if (!x || !z)
                return std::nullopt;
            if (found->topology == ESpatial3DSourceTopology::PLANAR_XZ)
            {
                return lux::math::GridCoord3i64{*x, 0, *z};
            }
            if (found->topology == ESpatial3DSourceTopology::VOLUMETRIC_XYZ)
            {
                const auto y = coordinate(actor.position.y);
                return y
                    ? std::optional{lux::math::GridCoord3i64{
                          *x, *y, *z}}
                    : std::nullopt;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<double> cellEdge(
            const Spatial3DAuthoringSource& source,
            const uuids::uuid& space) noexcept
        {
            const auto found = std::ranges::find(
                source.spaces,
                space,
                &Spatial3DSourceSpace::id);
            if (found == source.spaces.end() ||
                !std::isfinite(found->cell_edge) ||
                found->cell_edge <= 0.0f)
            {
                return std::nullopt;
            }
            return static_cast<double>(found->cell_edge);
        }

        [[nodiscard]] std::tuple<
            std::string,
            std::uint8_t,
            std::int64_t,
            std::int64_t,
            std::int64_t>
        pageOrder(
            const uuids::uuid& space,
            const lux::math::GridCoord3i64& cell)
        {
            return {
                uuidKey(space), 0u, cell.x, cell.y, cell.z};
        }

        [[nodiscard]] std::int64_t floorDivide(
            std::int64_t value,
            std::int64_t divisor) noexcept
        {
            const auto quotient = value / divisor;
            return value % divisor < 0 ? quotient - 1 : quotient;
        }

        [[nodiscard]] std::optional<lux::math::GridCoord3i64>
        visualLodParentCell(
            const lux::math::GridCoord3i64& cell,
            std::uint8_t level = 1u) noexcept
        {
            if (cell.y != 0 || level == 0u ||
                level > kMaximumVisualLodLevels)
            {
                return std::nullopt;
            }
            const auto edge = std::int64_t{1}
                << (static_cast<unsigned>(level) * 2u);
            return lux::math::GridCoord3i64{
                floorDivide(cell.x, edge) * edge,
                0,
                floorDivide(cell.z, edge) * edge
            };
        }

        [[nodiscard]] std::string visualLodKey(
            const uuids::uuid& space,
            const lux::math::GridCoord3i64& cell,
            std::span<const std::string> layers = {})
        {
            const auto order = pageOrder(space, cell);
            std::string result = std::get<0>(order) + "/" +
                std::to_string(std::get<1>(order)) + "/" +
                std::to_string(std::get<2>(order)) + "/" +
                std::to_string(std::get<3>(order)) + "/" +
                std::to_string(std::get<4>(order));
            for (const auto& layer : layers)
                result += "/" + layer;
            return result;
        }

        [[nodiscard]] lux::ecs::PersistentEntityId
        visualLodEntityId(
            const lux::scene::ScenePackageId& scene,
            std::string_view kind,
            const uuids::uuid& space,
            const lux::math::GridCoord3i64& cell,
            std::uint8_t level,
            std::span<const std::string> layers = {})
        {
            uuids::uuid_name_generator generator{scene.value()};
            return lux::ecs::PersistentEntityId{generator(
                "lux.visual-lod/" + std::string{kind} + "/" +
                std::to_string(level) + "/" +
                visualLodKey(space, cell, layers))};
        }

        struct VisualHlodNodePlan final
        {
            uuids::uuid space{};
            lux::math::GridCoord3i64 cell;
            std::vector<std::string> data_layers;
            std::vector<std::size_t> pages;
            std::optional<std::size_t> parent;
            std::uint8_t level{1u};
            lux::ecs::PersistentEntityId persistent_id;
        };

        struct VisualHlodPlan final
        {
            std::vector<VisualHlodNodePlan> nodes;
            std::vector<std::optional<std::size_t>> page_parents;
        };

        [[nodiscard]] VisualHlodPlan visualHlodPlan(
            const Spatial3DAuthoringSource& source,
            const lux::scene::ScenePackageId& scene)
        {
            VisualHlodPlan result;
            result.page_parents.resize(source.instance_pages.size());
            std::map<std::string, std::vector<std::size_t>> candidates;
            for (std::size_t index = 0u;
                 index < source.instance_pages.size();
                 ++index)
            {
                const auto parent = visualLodParentCell(
                    source.instance_pages[index].cell);
                if (parent)
                {
                    candidates[visualLodKey(
                        source.instance_pages[index].space, *parent)]
                        .push_back(index);
                }
            }
            std::vector<std::size_t> previous_level;
            for (auto& [_, pages] : candidates)
            {
                if (pages.size() < 2u ||
                    pages.size() > kMaximumVisualLodChildren)
                {
                    continue;
                }
                std::ranges::sort(
                    pages,
                    {},
                    [&source](std::size_t index)
                    {
                        const auto& page = source.instance_pages[index];
                        return pageOrder(page.space, page.cell);
                    });
                const auto& first = source.instance_pages[pages.front()];
                if (!std::ranges::all_of(
                        pages,
                        [&source, &first](std::size_t index)
                        {
                            return source.instance_pages[index].data_layers ==
                                first.data_layers;
                        }))
                {
                    continue;
                }
                const auto parent = visualLodParentCell(first.cell);
                if (!parent)
                    continue;
                VisualHlodNodePlan node;
                node.space = first.space;
                node.cell = *parent;
                node.data_layers = first.data_layers;
                node.pages = std::move(pages);
                node.level = 1u;
                node.persistent_id = visualLodEntityId(
                    scene,
                    "coarse",
                    node.space,
                    node.cell,
                    node.level,
                    node.data_layers);
                const auto node_index = result.nodes.size();
                for (const auto page : node.pages)
                    result.page_parents[page] = node_index;
                result.nodes.push_back(std::move(node));
                previous_level.push_back(node_index);
            }

            for (std::uint8_t level = 2u;
                 level <= kMaximumVisualLodLevels &&
                 !previous_level.empty();
                 ++level)
            {
                std::map<std::string, std::vector<std::size_t>> parents;
                for (const auto child : previous_level)
                {
                    const auto parent = visualLodParentCell(
                        result.nodes[child].cell, level);
                    if (parent)
                    {
                        parents[visualLodKey(
                            result.nodes[child].space,
                            *parent,
                            result.nodes[child].data_layers)]
                            .push_back(child);
                    }
                }
                std::vector<std::size_t> next_level;
                for (auto& [_, children] : parents)
                {
                    if (children.size() < 2u ||
                        children.size() > kMaximumVisualLodChildren)
                    {
                        continue;
                    }
                    std::ranges::sort(children);
                    const auto& first = result.nodes[children.front()];
                    const auto parent = visualLodParentCell(
                        first.cell, level);
                    if (!parent)
                        continue;
                    VisualHlodNodePlan node;
                    node.space = first.space;
                    node.cell = *parent;
                    node.data_layers = first.data_layers;
                    node.level = level;
                    for (const auto child : children)
                    {
                        result.nodes[child].parent = result.nodes.size();
                        node.pages.insert(
                            node.pages.end(),
                            result.nodes[child].pages.begin(),
                            result.nodes[child].pages.end());
                    }
                    std::ranges::sort(node.pages);
                    node.pages.erase(
                        std::unique(node.pages.begin(), node.pages.end()),
                        node.pages.end());
                    node.persistent_id = visualLodEntityId(
                        scene,
                        "coarse",
                        node.space,
                        node.cell,
                        node.level,
                        node.data_layers);
                    next_level.push_back(result.nodes.size());
                    result.nodes.push_back(std::move(node));
                }
                previous_level = std::move(next_level);
            }
            return result;
        }

        struct PreparedClassicMeshBatch final
        {
            lux::classic_mesh::ClassicMeshBatchBlobV1 blob;
            lux::ecs::ClassicMeshBatchComponent component;
            lux::math::Position3d origin;
            float geometric_error{0.0f};
        };

        [[nodiscard]] lux::cxx::expected<
            PreparedClassicMeshBatch,
            AdapterFailure>
        classicMeshBatch(
            const Spatial3DInstancePageSource& page)
        {
            if (page.instances.empty())
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                    "legacy Instance Page is empty"));
            }
            std::vector<const Spatial3DInstanceSource*> instances;
            instances.reserve(page.instances.size());
            for (const auto& instance : page.instances)
                instances.push_back(&instance);
            std::ranges::sort(
                instances,
                [](const auto* lhs, const auto* rhs)
                {
                    return std::pair{
                               uuidKey(lhs->id.value()), 0u} <
                        std::pair{
                               uuidKey(rhs->id.value()), 0u};
                });

            std::array<double, 3u> minimum{
                (std::numeric_limits<double>::max)(),
                (std::numeric_limits<double>::max)(),
                (std::numeric_limits<double>::max)()
            };
            std::array<double, 3u> maximum{
                (std::numeric_limits<double>::lowest)(),
                (std::numeric_limits<double>::lowest)(),
                (std::numeric_limits<double>::lowest)()
            };
            std::vector<lux::math::Position3d> positions;
            positions.reserve(instances.size());
            for (const auto* instance : instances)
            {
                if (!lux::math::isFinite(instance->position))
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::INVALID_POSITION,
                        "Classic Mesh instance has an invalid 3D position"));
                }
                positions.push_back(instance->position);
                const auto extent = static_cast<double>(std::max({
                    std::abs(instance->scale[0]),
                    std::abs(instance->scale[1]),
                    std::abs(instance->scale[2]),
                    1.0f
                }));
                const std::array values{
                    instance->position.x,
                    instance->position.y,
                    instance->position.z
                };
                for (std::size_t axis = 0u; axis < values.size(); ++axis)
                {
                    minimum[axis] = std::min(
                        minimum[axis], values[axis] - extent);
                    maximum[axis] = std::max(
                        maximum[axis], values[axis] + extent);
                }
            }
            const lux::math::Position3d origin{
                (minimum[0] + maximum[0]) * 0.5,
                (minimum[1] + maximum[1]) * 0.5,
                (minimum[2] + maximum[2]) * 0.5
            };
            const auto half_x = (maximum[0] - minimum[0]) * 0.5;
            const auto half_y = (maximum[1] - minimum[1]) * 0.5;
            const auto half_z = (maximum[2] - minimum[2]) * 0.5;
            const auto radius = std::sqrt(
                half_x * half_x + half_y * half_y + half_z * half_z);
            if (!lux::math::isFinite(origin) ||
                !std::isfinite(radius) ||
                radius > (std::numeric_limits<float>::max)())
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                    "Classic Mesh batch bounds exceed the finite contract"));
            }

            PreparedClassicMeshBatch result;
            result.origin = origin;
            result.blob.instances.reserve(instances.size());
            for (std::size_t index = 0u; index < instances.size(); ++index)
            {
                const auto* source = instances[index];
                lux::classic_mesh::ClassicMeshBatchInstanceV1 row;
                const std::array<double, 3u> local{
                    positions[index].x - origin.x,
                    positions[index].y - origin.y,
                    positions[index].z - origin.z
                };
                for (std::size_t axis = 0u; axis < local.size(); ++axis)
                {
                    if (!std::isfinite(local[axis]) ||
                        std::abs(local[axis]) >
                            (std::numeric_limits<float>::max)())
                    {
                        return lux::cxx::unexpected(failure(
                            AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                            "Classic Mesh batch-local translation exceeds float range"));
                    }
                    row.translation[axis] =
                        static_cast<float>(local[axis]);
                }
                row.rotation = source->rotation;
                row.scale = source->scale;
                row.mesh_asset = source->mesh;
                row.material_asset = source->material_instance;
                row.stable_pick_id = source->stable_pick_id;
                row.rgba8 = source->rgba8;
                result.blob.instances.push_back(std::move(row));
            }
            const auto valid =
                lux::classic_mesh::validateClassicMeshBatchBlob(
                    result.blob);
            if (!valid)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                    valid.error().detail));
            }
            result.component.local_bounds_center = Eigen::Vector3f::Zero();
            result.component.local_bounds_radius =
                static_cast<float>(radius);
            // The caller installs the ordinary Transform3D at `origin`.
            result.component.content = {};
            return result;
        }

        struct DecodedMeshCatalog final
        {
            std::map<std::string, const Spatial3DMeshAssetSource*> sources;
            std::map<std::string, std::shared_ptr<const lux::rdesc::Mesh>>
                decoded;
        };

        [[nodiscard]] lux::cxx::expected<DecodedMeshCatalog, AdapterFailure>
        indexMeshAssets(const Spatial3DMeshAssetCatalog& catalog)
        {
            DecodedMeshCatalog result;
            for (const auto& source : catalog.meshes)
            {
                if (source.id.is_nil() || source.encoded_image.empty() ||
                    !result.sources.emplace(uuidKey(source.id), &source).second)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                        "Spatial3D Mesh asset catalog contains an invalid or duplicate identity"));
                }
            }
            return result;
        }

        [[nodiscard]] lux::cxx::expected<
            std::shared_ptr<const lux::rdesc::Mesh>,
            AdapterFailure>
        resolveMesh(
            DecodedMeshCatalog& catalog,
            const lux::asset::asset_id_t& id)
        {
            const auto key = uuidKey(id);
            if (const auto found = catalog.decoded.find(key);
                found != catalog.decoded.end())
            {
                return found->second;
            }
            const auto source = catalog.sources.find(key);
            if (source == catalog.sources.end())
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                    "visual HLOD is missing Runtime Mesh asset '" + key + "'"));
            }
            const auto& image = source->second->encoded_image;
            const auto codecs = lux::asset::runtimeAssetCodecCatalog();
            auto shell = lux::asset::makeShellFromMemory(
                *codecs, image.data(), image.size());
            if (!shell || (*shell)->type() != lux::asset::EAssetType::MESH ||
                (*shell)->id() != id)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                    "visual HLOD Mesh image has a mismatched header at '" +
                        key + "'"));
            }
            auto decoded = lux::asset::MeshSerDeser::decodeData(
                image.data(), image.size());
            if (!decoded || !*decoded || (*decoded)->vertices.empty() ||
                (*decoded)->indices.empty())
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                    "visual HLOD cannot decode Runtime Mesh asset '" + key +
                        "'"));
            }
            auto retained = std::shared_ptr<const lux::rdesc::Mesh>(
                std::move(*decoded));
            catalog.decoded.emplace(key, retained);
            return retained;
        }

        struct VisualGeometryGroupKey final
        {
            std::string material;
            std::uint32_t rgba8{0xffffffffu};

            auto operator<=>(const VisualGeometryGroupKey&) const = default;
        };

        struct VisualGeometryGroup final
        {
            lux::asset::asset_id_t material{};
            std::uint32_t rgba8{0xffffffffu};
            std::vector<const Spatial3DInstanceSource*> instances;
        };

        struct PreparedVisualHlod final
        {
            PreparedClassicMeshBatch batch;
            std::vector<CookedSpatial3DMeshAsset> generated_meshes;
        };

        [[nodiscard]] Eigen::Vector3f normalizedOr(
            const Eigen::Vector3f& value,
            const Eigen::Vector3f& fallback) noexcept
        {
            const auto squared = value.squaredNorm();
            return std::isfinite(squared) && squared > 1.0e-20f
                ? value / std::sqrt(squared)
                : fallback;
        }

        [[nodiscard]] lux::cxx::expected<PreparedVisualHlod, AdapterFailure>
        cookVisualHlod(
            const lux::scene::ScenePackageId& scene,
            const VisualHlodNodePlan& node,
            const Spatial3DInstancePageSource& aggregate,
            DecodedMeshCatalog& meshes,
            const Spatial3DEntitySceneAdapterConfig& config)
        {
            auto bounds = classicMeshBatch(aggregate);
            if (!bounds)
                return lux::cxx::unexpected(std::move(bounds.error()));
            if (aggregate.instances.size() >
                config.visual_lod_max_source_instances)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                    "visual HLOD node exceeds the source-instance cook budget"));
            }

            std::map<VisualGeometryGroupKey, VisualGeometryGroup> groups;
            for (const auto& instance : aggregate.instances)
            {
                const VisualGeometryGroupKey key{
                    uuidKey(instance.material_instance), instance.rgba8};
                const auto [found, inserted] = groups.try_emplace(key);
                if (inserted && groups.size() >
                    config.visual_lod_max_generated_meshes)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                        "visual HLOD node exceeds the generated-Mesh cook budget"));
                }
                auto& group = found->second;
                group.material = instance.material_instance;
                group.rgba8 = instance.rgba8;
                group.instances.push_back(&instance);
            }

            PreparedVisualHlod result;
            result.batch = std::move(*bounds);
            result.batch.blob.instances.clear();
            result.batch.blob.instances.reserve(groups.size());
            result.generated_meshes.reserve(groups.size());
            lux::math::AABB node_mesh_bounds;
            float maximum_relative_error = 0.0f;

            for (auto& [key, group] : groups)
            {
                std::ranges::sort(
                    group.instances,
                    [](const auto* lhs, const auto* rhs)
                    {
                        return uuidKey(lhs->id.value()) <
                            uuidKey(rhs->id.value());
                    });
                lux::rdesc::Mesh merged;
                std::uint64_t source_vertices = 0u;
                std::uint64_t source_triangles = 0u;
                float authored_lod_error = 0.0f;
                std::string content_identity =
                    "lux.visual-lod.mesh-input.v1/" +
                    uuidKey(node.persistent_id.value()) + "/" +
                    key.material + "/" + std::to_string(key.rgba8);
                content_identity += "/" + std::to_string(
                    std::bit_cast<std::uint32_t>(
                        config.visual_lod_triangle_ratio));
                content_identity += "/" + std::to_string(
                    std::bit_cast<std::uint32_t>(
                        config.visual_lod_max_simplification_error));
                for (const auto* instance : group.instances)
                {
                    auto source_mesh = resolveMesh(meshes, instance->mesh);
                    if (!source_mesh)
                        return lux::cxx::unexpected(
                            std::move(source_mesh.error()));
                    const auto& source = **source_mesh;
                    const auto source_record = meshes.sources.find(
                        uuidKey(instance->mesh));
                    if (source_record == meshes.sources.end())
                    {
                        return lux::cxx::unexpected(failure(
                            AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                            "visual HLOD source Mesh disappeared during cook"));
                    }
                    content_identity += "/" + uuidKey(instance->id.value()) +
                        "/" + uuidKey(instance->mesh) + "/" +
                        lux::cxx::algorithm::toHex(
                            lux::cxx::algorithm::Sha256::hash(
                                source_record->second->encoded_image)) +
                        "/" + std::to_string(std::bit_cast<std::uint64_t>(
                            instance->position.x)) +
                        "/" + std::to_string(std::bit_cast<std::uint64_t>(
                            instance->position.y)) +
                        "/" + std::to_string(std::bit_cast<std::uint64_t>(
                            instance->position.z));
                    for (const auto value : instance->rotation)
                    {
                        content_identity += "/" + std::to_string(
                            std::bit_cast<std::uint32_t>(value));
                    }
                    for (const auto value : instance->scale)
                    {
                        content_identity += "/" + std::to_string(
                            std::bit_cast<std::uint32_t>(value));
                    }
                    if (source.indices.size() % 3u != 0u ||
                        source.vertices.size() >
                            config.visual_lod_max_merged_vertices -
                                merged.vertices.size())
                    {
                        return lux::cxx::unexpected(failure(
                            AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                            "visual HLOD source Mesh exceeds the merge-buffer cook budget"));
                    }
                    const std::vector<std::uint32_t>* selected_indices =
                        &source.indices;
                    if (!source.lods.empty() && node.level != 0u)
                    {
                        const auto lod_index = std::min<std::size_t>(
                            static_cast<std::size_t>(node.level - 1u),
                            source.lods.size() - 1u);
                        const auto& authored_lod = source.lods[lod_index];
                        if (!authored_lod.indices.empty() &&
                            authored_lod.indices.size() <
                                selected_indices->size())
                        {
                            if (!std::isfinite(authored_lod.error) ||
                                authored_lod.error < 0.0f)
                            {
                                return lux::cxx::unexpected(failure(
                                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                                    "visual HLOD source Mesh has an invalid authored LOD error"));
                            }
                            selected_indices = &authored_lod.indices;
                            authored_lod_error = std::max(
                                authored_lod_error,
                                authored_lod.error);
                        }
                    }
                    if (selected_indices->size() % 3u != 0u ||
                        selected_indices->size() >
                            config.visual_lod_max_merged_indices -
                                merged.indices.size())
                    {
                        return lux::cxx::unexpected(failure(
                            AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                            "visual HLOD source Mesh exceeds the merge-buffer cook budget"));
                    }
                    const auto vertex_base = static_cast<std::uint32_t>(
                        merged.vertices.size());
                    const Eigen::Quaternionf rotation{
                        instance->rotation[3],
                        instance->rotation[0],
                        instance->rotation[1],
                        instance->rotation[2]
                    };
                    const Eigen::Vector3f scale{
                        instance->scale[0],
                        instance->scale[1],
                        instance->scale[2]
                    };
                    if (!rotation.coeffs().allFinite() ||
                        !scale.allFinite() || rotation.squaredNorm() < 1.0e-12f)
                    {
                        return lux::cxx::unexpected(failure(
                            AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                            "visual HLOD instance has an invalid transform"));
                    }
                    const auto linear = rotation.normalized().toRotationMatrix() *
                        scale.asDiagonal();
                    const auto determinant = linear.determinant();
                    const auto normal_matrix = std::isfinite(determinant) &&
                            std::abs(determinant) > 1.0e-12f
                        ? linear.inverse().transpose()
                        : rotation.normalized().toRotationMatrix();
                    const Eigen::Vector3f translation{
                        static_cast<float>(instance->position.x -
                            result.batch.origin.x),
                        static_cast<float>(instance->position.y -
                            result.batch.origin.y),
                        static_cast<float>(instance->position.z -
                            result.batch.origin.z)
                    };
                    if (!translation.allFinite())
                    {
                        return lux::cxx::unexpected(failure(
                            AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                            "visual HLOD local translation exceeds float range"));
                    }
                    merged.vertices.reserve(
                        merged.vertices.size() + source.vertices.size());
                    for (const auto& input : source.vertices)
                    {
                        lux::rdesc::Vertex output{};
                        output.position = translation + linear * input.position;
                        output.normal = normalizedOr(
                            normal_matrix * input.normal,
                            Eigen::Vector3f::UnitY());
                        output.tangent = normalizedOr(
                            normal_matrix * input.tangent,
                            Eigen::Vector3f::UnitX());
                        output.bitangent = normalizedOr(
                            normal_matrix * input.bitangent,
                            Eigen::Vector3f::UnitZ());
                        output.uv = input.uv;
                        output.bone = {};
                        if (!output.position.allFinite() ||
                            !output.uv.allFinite())
                        {
                            return lux::cxx::unexpected(failure(
                                AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                                "visual HLOD source Mesh has non-finite vertex data"));
                        }
                        merged.vertices.push_back(std::move(output));
                    }
                    merged.indices.reserve(
                        merged.indices.size() + selected_indices->size());
                    for (std::size_t triangle = 0u;
                         triangle < selected_indices->size(); triangle += 3u)
                    {
                        std::array<std::uint32_t, 3u> indices{
                            (*selected_indices)[triangle],
                            (*selected_indices)[triangle + 1u],
                            (*selected_indices)[triangle + 2u]
                        };
                        if (std::ranges::any_of(
                                indices,
                                [&source](std::uint32_t index)
                                {
                                    return index >= source.vertices.size();
                                }))
                        {
                            return lux::cxx::unexpected(failure(
                                AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                                "visual HLOD source Mesh contains an invalid index"));
                        }
                        if (determinant < 0.0f)
                            std::swap(indices[1u], indices[2u]);
                        for (const auto index : indices)
                            merged.indices.push_back(vertex_base + index);
                    }
                    source_triangles += source.indices.size() / 3u;
                    source_vertices += source.vertices.size();
                }

                std::vector<std::uint32_t> remap(merged.vertices.size());
                const auto compact_vertex_count = meshopt_generateVertexRemap(
                    remap.data(),
                    merged.indices.data(),
                    merged.indices.size(),
                    merged.vertices.data(),
                    merged.vertices.size(),
                    sizeof(lux::rdesc::Vertex));
                std::vector<lux::rdesc::Vertex> compact_vertices(
                    compact_vertex_count);
                std::vector<std::uint32_t> compact_indices(
                    merged.indices.size());
                meshopt_remapVertexBuffer(
                    compact_vertices.data(),
                    merged.vertices.data(),
                    merged.vertices.size(),
                    sizeof(lux::rdesc::Vertex),
                    remap.data());
                meshopt_remapIndexBuffer(
                    compact_indices.data(),
                    merged.indices.data(),
                    merged.indices.size(),
                    remap.data());
                merged.vertices = std::move(compact_vertices);
                merged.indices = std::move(compact_indices);

                const auto ratio = std::pow(
                    static_cast<double>(config.visual_lod_triangle_ratio),
                    static_cast<double>(node.level));
                const auto target = std::max<std::size_t>(
                    3u,
                    (static_cast<std::size_t>(
                         static_cast<double>(merged.indices.size()) * ratio) /
                        3u) * 3u);
                float simplification_error = authored_lod_error;
                if (target < merged.indices.size())
                {
                    std::vector<std::array<float, 3>> simplify_positions;
                    simplify_positions.reserve(merged.vertices.size());
                    for (const auto& vertex : merged.vertices)
                    {
                        simplify_positions.push_back({
                            vertex.position.x(),
                            vertex.position.y(),
                            vertex.position.z()});
                    }
                    std::vector<std::uint32_t> simplified(
                        merged.indices.size());
                    float optimizer_error = 0.0f;
                    auto count = meshopt_simplify(
                        simplified.data(),
                        merged.indices.data(),
                        merged.indices.size(),
                        simplify_positions.front().data(),
                        simplify_positions.size(),
                        sizeof(simplify_positions.front()),
                        target,
                        config.visual_lod_max_simplification_error,
                        0u,
                        &optimizer_error);
                    if (count >= merged.indices.size())
                    {
                        count = meshopt_simplifySloppy(
                            simplified.data(),
                            merged.indices.data(),
                            merged.indices.size(),
                            simplify_positions.front().data(),
                            simplify_positions.size(),
                            sizeof(simplify_positions.front()),
                            target,
                            config.visual_lod_max_simplification_error,
                            &optimizer_error);
                    }
                    if (count >= 3u && count < merged.indices.size())
                    {
                        count -= count % 3u;
                        if (count >= 3u)
                        {
                            simplified.resize(count);
                            merged.indices = std::move(simplified);
                            simplification_error = std::max(
                                simplification_error,
                                optimizer_error);
                        }
                    }
                }
                meshopt_optimizeVertexCache(
                    merged.indices.data(),
                    merged.indices.data(),
                    merged.indices.size(),
                    merged.vertices.size());
                std::vector<lux::rdesc::Vertex> fetched_vertices(
                    merged.vertices.size());
                const auto fetched_vertex_count =
                    meshopt_optimizeVertexFetch(
                        fetched_vertices.data(),
                        merged.indices.data(),
                        merged.indices.size(),
                        merged.vertices.data(),
                        merged.vertices.size(),
                        sizeof(lux::rdesc::Vertex));
                if (fetched_vertex_count == 0u ||
                    fetched_vertex_count > fetched_vertices.size())
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                        "visual HLOD vertex compaction failed"));
                }
                fetched_vertices.resize(fetched_vertex_count);
                merged.vertices = std::move(fetched_vertices);

                lux::math::AABB mesh_bounds;
                for (const auto& vertex : merged.vertices)
                    mesh_bounds.merge(vertex.position);
                merged.bounds = mesh_bounds;
                node_mesh_bounds.merge(mesh_bounds);
                merged.lods.clear();

                uuids::uuid_name_generator generator{scene.value()};
                const auto content_identity_bytes = std::as_bytes(
                    std::span{content_identity.data(),
                        content_identity.size()});
                const auto provisional_id = generator(
                    "lux.visual-lod.mesh.provisional.v1/" +
                    lux::cxx::algorithm::toHex(
                        lux::cxx::algorithm::Sha256::hash(
                            content_identity_bytes)));
                auto provisional = lux::asset::MeshSerDeser::encodeData(
                    provisional_id, merged);
                if (!provisional)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                        "cannot encode generated visual HLOD Mesh asset"));
                }
                const auto generated_id = generator(
                    "lux.visual-lod.mesh.v1/" +
                    lux::cxx::algorithm::toHex(
                        lux::cxx::algorithm::Sha256::hash(*provisional)));
                auto encoded = lux::asset::MeshSerDeser::encodeData(
                    generated_id, merged);
                if (!encoded)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                        "cannot finalize generated visual HLOD Mesh asset"));
                }
                const auto generated_key = uuidKey(generated_id);
                result.generated_meshes.push_back({
                    generated_id,
                    "Generated/HlodMeshes/" + generated_key,
                    std::move(*encoded),
                    static_cast<std::uint32_t>(group.instances.size()),
                    static_cast<std::uint32_t>(source_vertices),
                    static_cast<std::uint32_t>(source_triangles),
                    static_cast<std::uint32_t>(merged.vertices.size()),
                    static_cast<std::uint32_t>(merged.indices.size() / 3u),
                    simplification_error});

                lux::classic_mesh::ClassicMeshBatchInstanceV1 row;
                row.mesh_asset = generated_id;
                row.material_asset = group.material;
                row.rgba8 = group.rgba8;
                row.stable_pick_id = 0u;
                result.batch.blob.instances.push_back(std::move(row));
                maximum_relative_error = std::max(
                    maximum_relative_error, simplification_error);
            }
            const auto valid = lux::classic_mesh::validateClassicMeshBatchBlob(
                result.batch.blob);
            if (!valid)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                    valid.error().detail));
            }
            if (!node_mesh_bounds.isValid())
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                    "visual HLOD generated Mesh bounds are invalid"));
            }
            const auto bounds_center = node_mesh_bounds.center();
            const auto bounds_radius = node_mesh_bounds.halfExtents().norm();
            if (!bounds_center.allFinite() || !std::isfinite(bounds_radius))
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                    "visual HLOD generated Mesh bounds exceed the finite contract"));
            }
            result.batch.component.local_bounds_center = bounds_center;
            result.batch.component.local_bounds_radius = bounds_radius;
            result.batch.geometric_error = std::max(
                0.001f,
                maximum_relative_error *
                    result.batch.component.local_bounds_radius);
            return result;
        }

        [[nodiscard]] lux::cxx::expected<void, AdapterFailure>
        appendFeature(
            std::vector<lux::scene::SceneFeatureRequest>& values,
            lux::scene::SceneFeatureRequest feature)
        {
            if (!feature.id.isValid() ||
                !lux::scene::isValidSceneFeatureIdName(feature.id.name()))
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::INVALID_ARGUMENT,
                    "Scene Feature request has a non-canonical id"));
            }
            const auto found = std::ranges::find(
                values,
                feature.id.name(),
                [](const auto& value)
                {
                    return value.id.name();
                });
            if (found == values.end())
            {
                values.push_back(std::move(feature));
                return {};
            }
            if (*found != feature)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::INVALID_ARGUMENT,
                    std::string{
                        "Scene Feature requests define conflicting configuration for '"} +
                        std::string{feature.id.name()} + "'"));
            }
            return {};
        }

    } // namespace

    lux::cxx::expected<
        CookedSpatial3DEntitySceneBundle,
        Spatial3DEntitySceneAdapterFailure>
    adaptSpatial3DEntityScene(
        const Spatial3DAuthoringSource& source,
        const lux::ecs::ComponentTypeCatalog& components,
        const Spatial3DMeshAssetCatalog& mesh_assets,
        Spatial3DEntitySceneAdapterConfig config)
    {
        using lux::ecs::scene_format::EntityOrdinal;
        using lux::ecs::scene_format::EntitySectionId;
        using lux::scene::DemandChannelId;
        using lux::ecs::scene_format::kInvalidEntityOrdinal;

        if (source.scene.empty() ||
            !validSectionContentPrefix(config.section_content_prefix) ||
            !lux::math::isFinite(config.fallback_camera_position) ||
            !std::isfinite(config.fine_active_distance) ||
            config.fine_active_distance <= 0.0 ||
            !std::isfinite(config.fine_resident_distance) ||
            config.fine_resident_distance < config.fine_active_distance ||
            config.fine_active_priority == 0u ||
            !std::isfinite(config.visual_lod_enter_error_pixels) ||
            !std::isfinite(config.visual_lod_exit_error_pixels) ||
            config.visual_lod_enter_error_pixels < config.visual_lod_exit_error_pixels ||
            config.visual_lod_exit_error_pixels < 0.0f ||
            !std::isfinite(config.visual_lod_triangle_ratio) ||
            config.visual_lod_triangle_ratio <= 0.0f ||
            config.visual_lod_triangle_ratio >= 1.0f ||
            !std::isfinite(config.visual_lod_max_simplification_error) ||
            config.visual_lod_max_simplification_error <= 0.0f ||
            config.visual_lod_max_simplification_error > 1.0f ||
            config.visual_lod_max_source_instances == 0u ||
            config.visual_lod_max_generated_meshes == 0u ||
            config.visual_lod_max_merged_vertices == 0u ||
            config.visual_lod_max_merged_indices < 3u ||
            !std::isfinite(config.visual_lod_active_scale) ||
            config.visual_lod_active_scale < 1.0 ||
            !std::isfinite(config.visual_lod_resident_scale) ||
            config.visual_lod_resident_scale < config.visual_lod_active_scale)
        {
            return lux::cxx::unexpected(failure(
                AdapterError::INVALID_ARGUMENT,
                "Spatial3D EntityScene adapter configuration is invalid"));
        }
        auto decoded_meshes = indexMeshAssets(mesh_assets);
        if (!decoded_meshes)
            return lux::cxx::unexpected(std::move(decoded_meshes.error()));
        const lux::scene::ScenePackageId scene = config.scene_id.empty()
            ? source.scene
            : config.scene_id;
        const auto startup_section = derivedSectionId(
            scene, "lux.toolchain.spatial3d_scene.startup.v1");
        EntitySectionAssembly startup{startup_section};
        struct FineSection final
        {
            FineSection(
                EntitySectionId id,
                lux::spatial3d::SourceId source_value,
                lux::math::GridCoord3i64 coordinate_value,
                double cell_world_size_value)
                : section(id),
                  source(std::move(source_value)),
                  coordinate(coordinate_value),
                  cell_world_size(cell_world_size_value)
            {}

            EntitySectionAssembly section;
            lux::spatial3d::SourceId source;
            lux::math::GridCoord3i64 coordinate;
            double cell_world_size{0.0};
        };
        std::map<std::string, FineSection> fine_sections;
        const auto ensureFineSection = [
            &fine_sections,
            &scene,
            &source](
            const uuids::uuid& space,
            const lux::math::GridCoord3i64& cell)
            -> lux::cxx::expected<FineSection*, AdapterFailure>
        {
            const auto coordinate = spatialCoordinate(cell);
            const auto edge = cellEdge(source, space);
            if (!edge)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::INVALID_POSITION,
                    "Spatial3D fine Section has no valid cell coordinate"));
            }
            const auto key = visualLodKey(space, cell);
            const auto id = derivedSectionId(
                scene,
                "lux.toolchain.spatial3d_scene.fine.v1/" + key);
            const auto [found, inserted] = fine_sections.try_emplace(
                key, id, spatialSourceId(space), coordinate, *edge);
            if (!inserted &&
                (found->second.section.id != id ||
                 found->second.source != spatialSourceId(space) ||
                 found->second.coordinate != coordinate ||
                 found->second.cell_world_size != *edge))
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::INVALID_ARGUMENT,
                    "Spatial3D fine Section identity is inconsistent"));
            }
            return &found->second;
        };

        std::vector<std::size_t> actor_order(source.actors.size());
        for (std::size_t index = 0u; index < actor_order.size(); ++index)
            actor_order[index] = index;
        std::ranges::sort(
            actor_order,
            {},
            [&source](std::size_t index)
            {
                return uuidKey(source.actors[index].id.value());
            });
        std::unordered_map<std::string, std::size_t> actor_indices;
        actor_indices.reserve(actor_order.size());
        std::optional<std::size_t> primary_camera_actor;
        for (const auto index : actor_order)
        {
            const auto& actor = source.actors[index];
            if (actor.id.empty() || !lux::math::isFinite(actor.position) ||
                !actor_indices.emplace(
                    uuidKey(actor.id.value()), index).second)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::INVALID_ACTOR,
                    "Spatial3D Authoring source contains an invalid or duplicate Actor"));
            }
            if (declaresPrimaryCamera(actor))
            {
                if (primary_camera_actor)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::INVALID_ACTOR,
                        "more than one Actor declares PrimaryCameraTag"));
                }
                primary_camera_actor = index;
            }
        }

        struct ActorPlacement final
        {
            EntitySectionAssembly* section{};
            EntityOrdinal ordinal{kInvalidEntityOrdinal};
        };
        std::vector<ActorPlacement> actor_placements(source.actors.size());
        std::map<EntitySectionAssembly*, EntityOrdinal> section_counts;
        for (const auto index : actor_order)
        {
            const auto& actor = source.actors[index];
            const bool fixed = actor.space.is_nil() ||
                declaresStartupSceneFact(actor);
            EntitySectionAssembly* destination = &startup;
            if (!fixed)
            {
                const auto cell = actorCell(source, actor);
                if (!cell)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::INVALID_POSITION,
                        "spatial Actor cannot be assigned to a 3D fine cell"));
                }
                auto fine = ensureFineSection(actor.space, *cell);
                if (!fine)
                    return lux::cxx::unexpected(std::move(fine.error()));
                destination = &(*fine)->section;
            }
            auto& count = section_counts[destination];
            actor_placements[index] = {destination, count++};
        }

        for (const auto index : actor_order)
        {
            const auto& actor = source.actors[index];
            auto materialized = materializeActor(
                actor,
                components);
            if (!materialized)
                return lux::cxx::unexpected(std::move(materialized.error()));
            if (actor.transform_parent)
            {
                const auto parent = actor_indices.find(uuidKey(actor.transform_parent->value()));
                if (parent == actor_indices.end() ||
                    actor_placements[parent->second].section !=
                        actor_placements[index].section)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::INVALID_ACTOR,
                        "Actor transform parent crosses an EntitySection boundary"));
                }
                materialized->entity.parent =
                    actor_placements[parent->second].ordinal;
            }
            if (materialized->primary_camera)
            {
                auto tag = typedTag<lux::ecs::PrimaryCameraTag>(components);
                if (!tag)
                    return lux::cxx::unexpected(std::move(tag.error()));
                setComponent(materialized->entity, std::move(*tag));
                lux::ecs::SpatialInterest3DComponent interest;
                interest.active_distance = config.fine_active_distance;
                interest.resident_distance = config.fine_resident_distance;
                interest.active_priority = config.fine_active_priority;
                auto interest_input = typedComponent(interest, components);
                if (!interest_input)
                {
                    return lux::cxx::unexpected(
                        std::move(interest_input.error()));
                }
                setComponent(
                    materialized->entity,
                    std::move(*interest_input));
            }
            const auto added = actor_placements[index].section->builder.addEntity(
                std::move(materialized->entity));
            if (!added)
            {
                return lux::cxx::unexpected(entityFailure(
                    added.error(),
                    "cannot add legacy Actor to its LXES"));
            }
            if (*added != actor_placements[index].ordinal)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::INVALID_ACTOR,
                    "Actor ordinal changed during section assembly"));
            }
        }

        if (!primary_camera_actor)
        {
            EntityCookInput camera_entity;
            lux::ecs::Transform3DComponent transform;
            transform.position = config.fallback_camera_position;
            auto transform_input = typedComponent(transform, components);
            auto camera_input = typedComponent(
                lux::ecs::Camera3DComponent{}, components);
            auto primary_input =
                typedTag<lux::ecs::PrimaryCameraTag>(components);
            lux::ecs::SpatialInterest3DComponent interest;
            interest.active_distance = config.fine_active_distance;
            interest.resident_distance = config.fine_resident_distance;
            interest.active_priority = config.fine_active_priority;
            auto interest_input = typedComponent(interest, components);
            if (!transform_input)
                return lux::cxx::unexpected(std::move(transform_input.error()));
            if (!camera_input)
                return lux::cxx::unexpected(std::move(camera_input.error()));
            if (!primary_input)
                return lux::cxx::unexpected(std::move(primary_input.error()));
            if (!interest_input)
                return lux::cxx::unexpected(std::move(interest_input.error()));
            camera_entity.components.push_back(std::move(*transform_input));
            camera_entity.components.push_back(std::move(*camera_input));
            camera_entity.components.push_back(std::move(*primary_input));
            camera_entity.components.push_back(std::move(*interest_input));
            const auto added = startup.builder.addEntity(
                std::move(camera_entity));
            if (!added)
            {
                return lux::cxx::unexpected(entityFailure(
                    added.error(),
                    "cannot add fallback Camera to startup LXES"));
            }
        }

        std::vector<std::size_t> instance_pages(source.instance_pages.size());
        for (std::size_t index = 0u; index < instance_pages.size(); ++index)
            instance_pages[index] = index;
        std::ranges::sort(
            instance_pages,
            {},
            [&source](std::size_t index)
            {
                const auto& page = source.instance_pages[index];
                return pageOrder(page.space, page.cell);
            });
        const auto visual_lod = visualHlodPlan(source, scene);
        const auto appendClassicBatch = [
            &components,
            &config](
            EntitySectionAssembly& destination,
            PreparedClassicMeshBatch batch,
            lux::ecs::PersistentEntityId persistent_id,
            std::uint8_t level,
            std::optional<lux::ecs::PersistentEntityId> parent)
            -> lux::cxx::expected<void, AdapterFailure>
        {
            auto encoded = lux::classic_mesh::encodeClassicMeshBatchBlob(
                batch.blob);
            if (!encoded)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                    encoded.error().detail));
            }
            auto attachment = internAttachment(
                destination,
                lux::ecs::scene_format::ContentTypeId{std::string{
                    lux::classic_mesh::kClassicMeshBatchContentTypeName}},
                lux::classic_mesh::kClassicMeshBatchSchemaVersion,
                std::move(*encoded));
            if (!attachment)
                return lux::cxx::unexpected(std::move(attachment.error()));

            EntityCookInput entity;
            entity.persistent_id = persistent_id;
            lux::ecs::Transform3DComponent transform;
            transform.position = batch.origin;
            lux::ecs::VisualLodNodeComponent lod;
            lod.level = level;
            lod.geometric_error = level == 0u
                ? 0.0f
                : batch.geometric_error;
            lod.enter_error_pixels =
                config.visual_lod_enter_error_pixels;
            lod.exit_error_pixels =
                config.visual_lod_exit_error_pixels;
            auto transform_input = typedComponent(transform, components);
            auto batch_input = typedComponent(batch.component, components);
            auto lod_input = typedComponent(lod, components);
            if (!transform_input)
                return lux::cxx::unexpected(std::move(transform_input.error()));
            if (!batch_input)
                return lux::cxx::unexpected(std::move(batch_input.error()));
            if (!lod_input)
                return lux::cxx::unexpected(std::move(lod_input.error()));
            batch_input->blob_references.push_back({
                "content", *attachment});
            entity.components.push_back(std::move(*transform_input));
            entity.components.push_back(std::move(*batch_input));
            entity.components.push_back(std::move(*lod_input));
            if (parent)
            {
                auto parent_input = typedComponent(
                    lux::ecs::VisualLodParentComponent{}, components);
                if (!parent_input)
                {
                    return lux::cxx::unexpected(
                        std::move(parent_input.error()));
                }
                parent_input->persistent_references.push_back({
                    "parent", *parent});
                entity.components.push_back(std::move(*parent_input));
            }
            const auto added = destination.builder.addEntity(
                std::move(entity));
            if (!added)
            {
                return lux::cxx::unexpected(entityFailure(
                    added.error(),
                    "cannot add Classic Mesh visual LOD entity to LXES"));
            }
            return {};
        };
        for (const auto index : instance_pages)
        {
            const auto& page = source.instance_pages[index];
            auto batch = classicMeshBatch(page);
            if (!batch)
                return lux::cxx::unexpected(std::move(batch.error()));
            std::optional<lux::ecs::PersistentEntityId> parent;
            if (visual_lod.page_parents[index])
            {
                parent = visual_lod.nodes[
                    *visual_lod.page_parents[index]].persistent_id;
            }
            auto fine = ensureFineSection(page.space, page.cell);
            if (!fine)
                return lux::cxx::unexpected(std::move(fine.error()));
            const auto added = appendClassicBatch(
                (*fine)->section,
                std::move(*batch),
                visualLodEntityId(
                    scene,
                    "fine",
                    page.space,
                    page.cell,
                    0u,
                    page.data_layers),
                0u,
                std::move(parent));
            if (!added)
                return lux::cxx::unexpected(std::move(added.error()));
        }
        struct CoarseSection final
        {
            CoarseSection(
                EntitySectionId id,
                lux::spatial3d::SourceId source_value,
                lux::math::GridCoord3i64 coordinate_value,
                std::uint8_t level_value,
                double cell_world_size_value)
                : section(id),
                  source(std::move(source_value)),
                  coordinate(coordinate_value),
                  level(level_value),
                  cell_world_size(cell_world_size_value)
            {}

            EntitySectionAssembly section;
            lux::spatial3d::SourceId source;
            lux::math::GridCoord3i64 coordinate;
            std::uint8_t level{1u};
            double cell_world_size{0.0};
        };
        std::map<std::string, CoarseSection> coarse_sections;
        std::vector<CookedSpatial3DMeshAsset> generated_meshes;
        for (const auto& node : visual_lod.nodes)
        {
            Spatial3DInstancePageSource aggregate;
            aggregate.space = node.space;
            aggregate.cell = node.cell;
            aggregate.data_layers = node.data_layers;
            for (const auto page : node.pages)
            {
                const auto& instances = source.instance_pages[page].instances;
                aggregate.instances.insert(
                    aggregate.instances.end(),
                    instances.begin(),
                    instances.end());
            }
            auto prepared = cookVisualHlod(
                scene, node, aggregate, *decoded_meshes, config);
            if (!prepared)
                return lux::cxx::unexpected(std::move(prepared.error()));
            generated_meshes.insert(
                generated_meshes.end(),
                std::make_move_iterator(
                    prepared->generated_meshes.begin()),
                std::make_move_iterator(
                    prepared->generated_meshes.end()));
            std::optional<lux::ecs::PersistentEntityId> parent;
            if (node.parent)
                parent = visual_lod.nodes[*node.parent].persistent_id;
            const auto coordinate = spatialCoordinate(node.cell);
            const auto fine_edge = cellEdge(source, node.space);
            if (!fine_edge)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::INVALID_POSITION,
                    "visual LOD node has no valid spatial cell"));
            }
            const auto factor = std::uint64_t{1u}
                << (static_cast<unsigned>(node.level) * 2u);
            const auto coarse_edge = *fine_edge *
                static_cast<double>(factor);
            const lux::math::GridCoord3i64 coarse_coordinate{
                floorDivide(coordinate.x,
                    static_cast<std::int64_t>(factor)),
                floorDivide(coordinate.y,
                    static_cast<std::int64_t>(factor)),
                floorDivide(coordinate.z,
                    static_cast<std::int64_t>(factor))
            };
            const auto key = std::to_string(node.level) + "/" +
                visualLodKey(
                    node.space, node.cell, node.data_layers);
            const auto id = derivedSectionId(
                scene,
                "lux.toolchain.spatial3d_scene.visual_lod.v1/" + key);
            const auto [found, inserted] = coarse_sections.try_emplace(
                key,
                id,
                spatialSourceId(node.space),
                coarse_coordinate,
                node.level,
                coarse_edge);
            if (!inserted)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::INVALID_ARGUMENT,
                    "visual LOD Section identity is duplicated"));
            }
            const auto added = appendClassicBatch(
                found->second.section,
                std::move(prepared->batch),
                node.persistent_id,
                node.level,
                std::move(parent));
            if (!added)
                return lux::cxx::unexpected(std::move(added.error()));
        }

        auto navigation_profiles = source.navigation_agent_profiles;
        if (navigation_profiles.empty())
            navigation_profiles.push_back(Spatial3DNavigationAgentSource{});
        if (navigation_profiles.size() > 256u)
        {
            return lux::cxx::unexpected(failure(
                AdapterError::NAVIGATION_CONTENT_REJECTED,
                "too many Spatial3D navigation agent profiles"));
        }
        for (std::size_t index = 0u;
             index < navigation_profiles.size(); ++index)
        {
            if (navigation_profiles[index].profile_index != index)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::NAVIGATION_CONTENT_REJECTED,
                    "navigation agent profile indices are not canonical"));
            }
        }

        std::vector<std::size_t> terrain_pages(source.terrain_pages.size());
        for (std::size_t index = 0u; index < terrain_pages.size(); ++index)
            terrain_pages[index] = index;
        std::ranges::sort(
            terrain_pages,
            {},
            [&source](std::size_t index)
            {
                const auto& page = source.terrain_pages[index];
                return std::tuple_cat(
                    pageOrder(page.space, page.cell),
                    std::tuple{uuidKey(page.terrain_set)});
            });

        struct NavigationDescriptionDraft final
        {
            std::uint8_t profile_index{0u};
            lux::navigation::detour3d::NavigationRegion3DDescription
                description;
        };
        std::vector<std::vector<NavigationDescriptionDraft>>
            navigation_descriptions(source.terrain_pages.size());
        using NavigationPageKey =
            std::tuple<std::string, std::int64_t, std::int64_t>;
        std::map<NavigationPageKey, std::size_t> navigation_pages_by_cell;
        for (const auto index : terrain_pages)
        {
            const auto& page = source.terrain_pages[index];
            const auto coordinate = planarCoordinate(page.cell);
            if (!coordinate)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::NAVIGATION_CONTENT_REJECTED,
                    "NavigationRegion3D requires a PLANAR_XZ terrain Cell"));
            }
            if (!navigation_pages_by_cell.emplace(
                    NavigationPageKey{
                        uuidKey(page.space), coordinate->x, coordinate->y},
                    index).second)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::NAVIGATION_CONTENT_REJECTED,
                    "more than one Terrain page occupies a navigation Cell"));
            }
            auto fine = ensureFineSection(page.space, page.cell);
            if (!fine)
                return lux::cxx::unexpected(std::move(fine.error()));
            auto& descriptions = navigation_descriptions[index];
            descriptions.reserve(navigation_profiles.size());
            const auto edge = cellEdge(source, page.space);
            if (!edge)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::NAVIGATION_CONTENT_REJECTED,
                    "NavigationRegion3D has no valid Spatial3D Cell edge"));
            }
            for (const auto& profile : navigation_profiles)
            {
                auto description = detail::cookSpatial3DNavigationRegion(
                    page,
                    profile,
                    navigationRegionId(
                        (*fine)->section.id, profile.profile_index),
                    *edge);
                if (!description)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::NAVIGATION_CONTENT_REJECTED,
                        std::move(description.error())));
                }
                descriptions.push_back({
                    profile.profile_index, std::move(*description)});
            }
        }

        const auto connectNavigationNeighbor = [
            &source,
            &navigation_pages_by_cell,
            &navigation_descriptions](
            std::size_t first_index,
            std::int64_t neighbor_x,
            std::int64_t neighbor_z,
            ENavigationPortalBoundary boundary,
            double seam)
            -> lux::cxx::expected<void, AdapterFailure>
        {
            const auto& first_page = source.terrain_pages[first_index];
            const auto found = navigation_pages_by_cell.find({
                uuidKey(first_page.space), neighbor_x, neighbor_z
            });
            if (found == navigation_pages_by_cell.end())
                return {};
            auto& first_descriptions =
                navigation_descriptions[first_index];
            auto& second_descriptions =
                navigation_descriptions[found->second];
            for (auto& first : first_descriptions)
            {
                const auto second = std::ranges::find(
                    second_descriptions,
                    first.profile_index,
                    &NavigationDescriptionDraft::profile_index);
                if (second == second_descriptions.end())
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::NAVIGATION_CONTENT_REJECTED,
                        "adjacent navigation Cells disagree on agent profiles"));
                }
                auto portal = navigationPortal(
                    first.description,
                    second->description,
                    boundary,
                    seam);
                if (!portal)
                    continue;
                first.description.portals.push_back(*portal);
                second->description.portals.push_back(std::move(*portal));
            }
            return {};
        };
        for (const auto index : terrain_pages)
        {
            const auto& page = source.terrain_pages[index];
            const auto coordinate = planarCoordinate(page.cell);
            const auto edge = cellEdge(source, page.space);
            if (!coordinate || !edge)
                continue;
            if (coordinate->x != (std::numeric_limits<std::int64_t>::max)())
            {
                const auto seam = std::fma(
                    static_cast<double>(coordinate->x + 1), *edge, 0.0);
                const auto connected = connectNavigationNeighbor(
                    index,
                    coordinate->x + 1,
                    coordinate->y,
                    ENavigationPortalBoundary::POSITIVE_X,
                    seam);
                if (!connected)
                    return lux::cxx::unexpected(std::move(connected.error()));
            }
            if (coordinate->y != (std::numeric_limits<std::int64_t>::max)())
            {
                const auto seam = std::fma(
                    static_cast<double>(coordinate->y + 1), *edge, 0.0);
                const auto connected = connectNavigationNeighbor(
                    index,
                    coordinate->x,
                    coordinate->y + 1,
                    ENavigationPortalBoundary::POSITIVE_Z,
                    seam);
                if (!connected)
                    return lux::cxx::unexpected(std::move(connected.error()));
            }
        }
        for (auto& descriptions : navigation_descriptions)
        {
            for (auto& description : descriptions)
            {
                std::ranges::sort(
                    description.description.portals,
                    [](const auto& left, const auto& right) noexcept
                    {
                        return std::tie(left.id.high, left.id.low) <
                            std::tie(right.id.high, right.id.low);
                    });
            }
        }

        for (const auto index : terrain_pages)
        {
            const auto& page = source.terrain_pages[index];
            const auto coordinate = planarCoordinate(page.cell);
            const auto edge = cellEdge(source, page.space);
            if (!coordinate || !edge)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::TERRAIN_CONTENT_REJECTED,
                    "Terrain tile requires a valid PLANAR_XZ space"));
            }
            auto blob = terrainBlob(page);
            if (!blob)
                return lux::cxx::unexpected(std::move(blob.error()));
            auto encoded = lux::terrain::encodeTerrainTileBlob(*blob);
            if (!encoded)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::TERRAIN_CONTENT_REJECTED,
                    encoded.error().detail));
            }
            lux::physics3d::StaticColliderBatch3DBlobV1 physics_blob;
            auto& heightfield = physics_blob.heightfields.emplace_back();
            heightfield.sample_edge =
                lux::terrain::kTerrainTileSampleEdge;
            heightfield.sample_spacing = blob->sample_spacing;
            heightfield.height_min = blob->height_min;
            heightfield.height_max = blob->height_max;
            heightfield.samples = blob->heights;
            auto encoded_physics = lux::physics3d::
                encodeStaticColliderBatch3DBlob(physics_blob);
            if (!encoded_physics)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::PHYSICS_CONTENT_REJECTED,
                    encoded_physics.error().detail));
            }
            auto fine = ensureFineSection(page.space, page.cell);
            if (!fine)
                return lux::cxx::unexpected(std::move(fine.error()));
            auto attachment = internAttachment(
                (*fine)->section,
                lux::ecs::scene_format::ContentTypeId{std::string{
                    lux::terrain::kTerrainTileContentTypeName}},
                lux::terrain::kTerrainTileSchemaVersion,
                std::move(*encoded));
            if (!attachment)
                return lux::cxx::unexpected(std::move(attachment.error()));
            auto physics_attachment = internAttachment(
                (*fine)->section,
                lux::ecs::scene_format::ContentTypeId{std::string{
                    lux::physics3d::
                        kStaticColliderBatch3DContentTypeName}},
                lux::physics3d::kStaticColliderBatch3DSchemaVersion,
                std::move(*encoded_physics));
            if (!physics_attachment)
            {
                return lux::cxx::unexpected(
                    std::move(physics_attachment.error()));
            }

            lux::ecs::Transform3DComponent transform;
            transform.position = {
                static_cast<double>(coordinate->x) * *edge,
                0.0,
                static_cast<double>(coordinate->y) * *edge};
            lux::ecs::TerrainTileComponent terrain;
            terrain.coordinate = *coordinate;
            lux::ecs::StaticColliderBatch3DComponent static_collision;
            auto transform_input = typedComponent(transform, components);
            auto terrain_input = typedComponent(terrain, components);
            auto static_collision_input = typedComponent(
                static_collision, components);
            if (!transform_input)
                return lux::cxx::unexpected(std::move(transform_input.error()));
            if (!terrain_input)
                return lux::cxx::unexpected(std::move(terrain_input.error()));
            if (!static_collision_input)
            {
                return lux::cxx::unexpected(
                    std::move(static_collision_input.error()));
            }
            terrain_input->blob_references.push_back({
                "content", *attachment});
            static_collision_input->blob_references.push_back({
                "content", *physics_attachment});
            EntityCookInput entity;
            entity.components.push_back(std::move(*transform_input));
            entity.components.push_back(std::move(*terrain_input));
            entity.components.push_back(
                std::move(*static_collision_input));
            const auto added = (*fine)->section.builder.addEntity(
                std::move(entity));
            if (!added)
            {
                return lux::cxx::unexpected(entityFailure(
                    added.error(),
                    "cannot add Terrain tile entity to fine LXES"));
            }

            for (const auto& navigation : navigation_descriptions[index])
            {
                auto encoded = lux::navigation::detour3d::
                    encodeNavigationRegion3D(navigation.description);
                if (!encoded)
                {
                    return lux::cxx::unexpected(failure(
                        AdapterError::NAVIGATION_CONTENT_REJECTED,
                        encoded.error().detail));
                }
                const auto bytes = encoded->payload.view();
                auto attachment = internAttachment(
                    (*fine)->section,
                    lux::ecs::scene_format::ContentTypeId{std::string{
                        lux::navigation::detour3d::
                            kNavigationRegion3DContentTypeName}},
                    lux::navigation::detour3d::
                        kNavigationRegion3DSchemaVersion,
                    std::vector<std::byte>{bytes.begin(), bytes.end()});
                if (!attachment)
                    return lux::cxx::unexpected(
                        std::move(attachment.error()));

                auto navigation_input = typedComponent(
                    lux::ecs::NavigationRegion3DComponent{}, components);
                if (!navigation_input)
                    return lux::cxx::unexpected(
                        std::move(navigation_input.error()));
                navigation_input->blob_references.push_back({
                    "content", *attachment});
                EntityCookInput navigation_entity;
                navigation_entity.components.push_back(
                    std::move(*navigation_input));
                const auto navigation_added =
                    (*fine)->section.builder.addEntity(
                        std::move(navigation_entity));
                if (!navigation_added)
                {
                    return lux::cxx::unexpected(entityFailure(
                        navigation_added.error(),
                        "cannot add NavigationRegion3D entity to fine LXES"));
                }
            }
        }

        ScenePackageCookInput cook;
        cook.id = scene;
        cook.startup_sections.push_back(startup_section);
        while (config.section_content_prefix.size() > 1u &&
               config.section_content_prefix.back() == '/')
        {
            config.section_content_prefix.pop_back();
        }
        const auto appendSection = [
            &cook,
            &config](
            EntitySectionAssembly& section,
            std::optional<DemandChannelId> demand)
            -> lux::cxx::expected<void, AdapterFailure>
        {
            auto image = std::move(section.builder).build();
            if (!image)
            {
                return lux::cxx::unexpected(entityFailure(
                    image.error(),
                    "cannot finalize Spatial3D LXES: " +
                        image.error().detail));
            }
            EntitySectionCookInput input;
            input.image = std::move(*image);
            input.source = lux::scene::StoredSectionSource{
                config.section_content_prefix + "/" +
                uuidKey(section.id.value())};
            if (demand)
                input.demand_channels.push_back(std::move(*demand));
            cook.sections.push_back(std::move(input));
            return {};
        };
        const auto startup_added = appendSection(startup, std::nullopt);
        if (!startup_added)
            return lux::cxx::unexpected(std::move(startup_added.error()));

        lux::spatial3d::SceneCatalog spatial_catalog;
        spatial_catalog.residency = config.residency;
        const auto internBand = [&spatial_catalog](
            lux::spatial3d::SceneCatalogBand band)
            -> std::uint32_t
        {
            const auto found = std::ranges::find(
                spatial_catalog.bands, band);
            if (found != spatial_catalog.bands.end())
            {
                return static_cast<std::uint32_t>(
                    found - spatial_catalog.bands.begin());
            }
            const auto index = static_cast<std::uint32_t>(
                spatial_catalog.bands.size());
            spatial_catalog.bands.push_back(std::move(band));
            return index;
        };
        const DemandChannelId fine_channel{
            std::string{lux::spatial3d::kResidentDemandChannelName}};
        const DemandChannelId visual_lod_channel{
            std::string{lux::spatial3d::kVisualLodDemandChannelName}};
        for (auto& [_, fine] : fine_sections)
        {
            const auto band = internBand({
                fine.source,
                fine_channel,
                0u,
                fine.cell_world_size,
                1.0,
                1.0});
            spatial_catalog.entries.push_back({
                fine.coordinate, band, fine.section.id});
            const auto added = appendSection(
                fine.section, fine_channel);
            if (!added)
                return lux::cxx::unexpected(std::move(added.error()));
        }
        for (auto& [_, coarse] : coarse_sections)
        {
            const auto level_scale = std::pow(
                4.0, static_cast<double>(coarse.level - 1u));
            const auto active_scale =
                config.visual_lod_active_scale * level_scale;
            const auto resident_scale = std::max(
                active_scale,
                config.visual_lod_resident_scale * level_scale);
            const auto band = internBand({
                coarse.source,
                visual_lod_channel,
                coarse.level,
                coarse.cell_world_size,
                active_scale,
                resident_scale});
            spatial_catalog.entries.push_back({
                coarse.coordinate, band, coarse.section.id});
            const auto added = appendSection(
                coarse.section, visual_lod_channel);
            if (!added)
                return lux::cxx::unexpected(std::move(added.error()));
        }
        if (!spatial_catalog.entries.empty())
        {
            auto encoded =
                lux::spatial3d::encodeSceneCatalog(
                    std::move(spatial_catalog));
            if (!encoded)
            {
                return lux::cxx::unexpected(failure(
                    AdapterError::SPATIAL_CATALOG_REJECTED,
                    encoded.error().detail));
            }
            const auto added = appendFeature(
                cook.features,
                lux::scene::SceneFeatureRequest{
                    lux::scene::SceneFeatureId{std::string{
                        lux::spatial3d::kPartitionedFeatureName}},
                    lux::spatial3d::kSceneCatalogSchemaVersion,
                    std::move(*encoded)});
            if (!added)
                return lux::cxx::unexpected(std::move(added.error()));
        }
        for (auto& feature : config.selected_features)
        {
            const auto added = appendFeature(
                cook.features,
                std::move(feature));
            if (!added)
                return lux::cxx::unexpected(std::move(added.error()));
        }
        for (auto& feature : config.additional_features)
        {
            const auto added = appendFeature(
                cook.features, std::move(feature));
            if (!added)
                return lux::cxx::unexpected(std::move(added.error()));
        }
        for (const auto& feature : source.features)
        {
            const auto added = appendFeature(
                cook.features,
                feature);
            if (!added)
                return lux::cxx::unexpected(std::move(added.error()));
        }
        cook.required_extensions = source.required_extensions;

        auto cooked = cookScenePackage(std::move(cook));
        if (!cooked)
        {
            return lux::cxx::unexpected(entityFailure(
                cooked.error(),
                "generic ScenePackage cooker rejected Spatial3D output"));
        }
        std::ranges::sort(
            generated_meshes,
            {},
            [](const auto& mesh) { return uuidKey(mesh.id); });
        if (std::adjacent_find(
                generated_meshes.begin(),
                generated_meshes.end(),
                [](const auto& lhs, const auto& rhs)
                {
                    return lhs.id == rhs.id ||
                        lhs.virtual_path == rhs.virtual_path;
                }) != generated_meshes.end())
        {
            return lux::cxx::unexpected(failure(
                AdapterError::CLASSIC_MESH_CONTENT_REJECTED,
                "generated visual HLOD Mesh identity is duplicated"));
        }
        CookedSpatial3DEntitySceneBundle result;
        static_cast<CookedScenePackageBundle&>(result) = std::move(*cooked);
        result.generated_meshes = std::move(generated_meshes);
        return result;
    }

    lux::cxx::expected<
        CookedSpatial3DEntitySceneBundle,
        Spatial3DEntitySceneAdapterFailure>
    cookSpatial3DEntitySceneSource(
        const std::filesystem::path& root_document,
        const lux::ecs::ComponentTypeCatalog& components,
        const Spatial3DMeshAssetCatalog& mesh_assets,
        Spatial3DEntitySceneAdapterConfig config,
        const Spatial3DAuthoringLoadLimits& limits)
    {
        auto source = loadSpatial3DAuthoringSource(root_document, limits);
        if (!source)
        {
            AdapterFailure result{
                AdapterError::AUTHORING_SOURCE_REJECTED,
                "LXWA/LXAD source loader rejected Spatial3D Authoring input"};
            result.authoring_source = source.error();
            return lux::cxx::unexpected(std::move(result));
        }
        return adaptSpatial3DEntityScene(
            *source, components, mesh_assets, std::move(config));
    }
} // namespace lux::toolchain
