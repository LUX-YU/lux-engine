#include <lux/engine/toolchain/spatial3d_scene/Spatial3DAuthoringSource.hpp>

#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace lux::toolchain
{
    namespace
    {
        [[nodiscard]] std::optional<lux::math::GridCoord3i64>
        gridCoordinate(const lux::authoring::WorldCellKey& cell) noexcept
        {
            if (const auto* planar = std::get_if<
                    lux::authoring::PlanarCellCoord>(&cell.coordinate))
            {
                return cell.topology ==
                        lux::authoring::EPartitionTopology::PLANAR_XZ
                    ? std::optional{lux::math::GridCoord3i64{
                          planar->a, 0, planar->b}}
                    : std::nullopt;
            }
            const auto* volume = std::get_if<
                lux::authoring::VolumeCellCoord>(&cell.coordinate);
            return volume && cell.topology ==
                    lux::authoring::EPartitionTopology::VOLUMETRIC_XYZ
                ? std::optional{lux::math::GridCoord3i64{
                      volume->x, volume->y, volume->z}}
                : std::nullopt;
        }

        [[nodiscard]] bool is3DSpace(
            const lux::authoring::PartitionSpaceDescriptor& space) noexcept
        {
            return space.topology ==
                    lux::authoring::EPartitionTopology::PLANAR_XZ ||
                space.topology ==
                    lux::authoring::EPartitionTopology::VOLUMETRIC_XYZ;
        }

        [[nodiscard]] std::uint64_t stableInstancePickId(
            const uuids::uuid& set,
            std::uint64_t local_id) noexcept
        {
            if (set.is_nil() || local_id == 0u)
            {
                return 0u;
            }
            std::array<std::byte, 24u> identity{};
            std::ranges::copy(set.as_bytes(), identity.begin());
            for (std::size_t index = 0u; index < sizeof(local_id); ++index)
            {
                identity[16u + index] = static_cast<std::byte>(
                    (local_id >> ((7u - index) * 8u)) & 0xffu);
            }
            const auto digest = lux::cxx::algorithm::Sha256::hash(identity);
            std::uint64_t result = 0u;
            for (std::size_t index = 0u; index < sizeof(result); ++index)
            {
                result = (result << 8u) |
                    std::to_integer<std::uint8_t>(digest[index]);
            }
            return result == 0u ? 1u : result;
        }

        [[nodiscard]] std::optional<lux::scene::SceneFeatureRequest>
        toSceneFeature(
            const lux::authoring::WorldSceneFeatureRequest& feature)
        {
            if (!feature.id.valid() ||
                !lux::scene::isValidSceneFeatureIdName(feature.id.name()))
            {
                return std::nullopt;
            }
            return lux::scene::SceneFeatureRequest{
                lux::scene::SceneFeatureId{feature.id.name()},
                feature.config_schema_version,
                feature.config};
        }

        [[nodiscard]] std::optional<lux::scene::RequiredExtension>
        toRequiredExtension(
            const lux::authoring::WorldRequiredExtension& requirement)
        {
            if (!requirement.id.valid() ||
                !lux::extensions::isCanonicalStableName(
                    requirement.id.name()))
            {
                return std::nullopt;
            }
            return lux::scene::RequiredExtension{
                lux::extensions::ExtensionId{requirement.id.name()},
                requirement.required_major,
                requirement.minimum_minor};
        }

        [[nodiscard]] lux::ecs::PersistentEntityId toRuntimeEntityId(
            const lux::authoring::WorldActorId& id) noexcept
        {
            return lux::ecs::PersistentEntityId{id.value()};
        }

        static_assert(!std::is_convertible_v<
            lux::authoring::WorldId,
            lux::asset::asset_id_t>);
        static_assert(!std::is_convertible_v<
            lux::asset::asset_id_t,
            lux::authoring::WorldId>);
        static_assert(!std::is_constructible_v<
            lux::authoring::WorldActorId,
            lux::ecs::PersistentEntityId>);
        static_assert(!std::is_constructible_v<
            lux::ecs::PersistentEntityId,
            lux::authoring::WorldActorId>);
    } // namespace

    lux::cxx::expected<Spatial3DAuthoringSource, std::string>
    loadSpatial3DAuthoringSource(
        const std::filesystem::path& root_document,
        const Spatial3DAuthoringLoadLimits& limits) noexcept
    {
        auto root = lux::authoring::loadWorldSource(root_document);
        if (!root)
        {
            return lux::cxx::unexpected(root.error());
        }

        Spatial3DAuthoringSource result;
        result.scene = lux::asset::asset_id_t{root->world.value()};
        result.features.reserve(root->contributions.size());
        for (const auto& feature : root->contributions)
        {
            auto cooked = toSceneFeature(feature);
            if (!cooked)
            {
                return lux::cxx::unexpected(
                    std::string{"invalid Authoring scene feature ID"});
            }
            result.features.push_back(std::move(*cooked));
        }
        result.spaces.reserve(root->spaces.size());
        std::unordered_set<std::string> spaces;
        for (const auto& space : root->spaces)
        {
            if (!is3DSpace(space))
            {
                continue;
            }
            const auto key = uuids::to_string(space.id.value());
            spaces.insert(key);
            result.spaces.push_back({
                space.id.value(),
                space.topology == lux::authoring::EPartitionTopology::PLANAR_XZ
                    ? ESpatial3DSourceTopology::PLANAR_XZ
                    : ESpatial3DSourceTopology::VOLUMETRIC_XYZ,
                static_cast<double>(space.cell_edge)
            });
        }
        result.required_extensions.reserve(root->required_extensions.size());
        for (const auto& requirement : root->required_extensions)
        {
            auto cooked = toRequiredExtension(requirement);
            if (!cooked)
            {
                return lux::cxx::unexpected(
                    std::string{"invalid Authoring Extension ID"});
            }
            result.required_extensions.push_back(std::move(*cooked));
        }

        lux::authoring::WorldSourceCodecLimits actor_limits;
        actor_limits.maximum_bytes = limits.maximum_actor_document_bytes;
        for (const auto& reference : root->descriptor_pages)
        {
            if (!spaces.contains(uuids::to_string(reference.space.value())))
            {
                continue;
            }
            auto page = lux::authoring::loadWorldDescriptorPage(
                root_document, *root, reference);
            if (!page)
            {
                return lux::cxx::unexpected(page.error());
            }

            for (const auto& descriptor : page->actors)
            {
                auto document = lux::authoring::loadWorldActorDocument(
                    root_document, descriptor, actor_limits);
                if (!document)
                {
                    return lux::cxx::unexpected(document.error());
                }
                if (document->world != root->world ||
                    document->actor != descriptor.id ||
                    document->actor_class != descriptor.actor_class ||
                    document->space != descriptor.space ||
                    document->position != descriptor.position ||
                    document->transform_parent !=
                        descriptor.transform_parent ||
                    document->data_layers != descriptor.data_layers ||
                    document->references != descriptor.references)
                {
                    return lux::cxx::unexpected(std::string{
                        "LXAD metadata does not match its LXAI descriptor"
                    });
                }
                const auto* position = std::get_if<lux::math::Position3d>(
                    &document->position);
                if (!position)
                {
                    return lux::cxx::unexpected(std::string{
                        "3D Descriptor Page contains a non-3D Actor"
                    });
                }
                Spatial3DActorSource actor;
                actor.id = toRuntimeEntityId(document->actor);
                actor.space = document->space.value();
                actor.position = *position;
                if (document->transform_parent)
                {
                    actor.transform_parent =
                        toRuntimeEntityId(*document->transform_parent);
                }
                actor.data_layers.reserve(document->data_layers.size());
                for (const auto& layer : document->data_layers)
                {
                    actor.data_layers.emplace_back(layer.name());
                }
                actor.name_table = std::move(document->name_table);
                actor.components.reserve(document->components.size());
                for (auto& component : document->components)
                {
                    actor.components.push_back({
                        std::move(component.schema_name),
                        component.schema_version,
                        std::move(component.tagged_payload)
                    });
                }
                result.actors.push_back(std::move(actor));
            }

            for (const auto& descriptor : page->pages)
            {
                if (descriptor.kind ==
                    lux::authoring::EWorldPageSourceKind::INSTANCE)
                {
                    auto source = lux::authoring::loadWorldInstancePage(
                        root_document,
                        descriptor.document_path,
                        *root);
                    if (!source)
                    {
                        return lux::cxx::unexpected(source.error());
                    }
                    const auto* owner = std::get_if<
                        lux::authoring::InstanceSetId>(&descriptor.owner);
                    if (source->world != root->world || !owner ||
                        source->instance_set != *owner ||
                        source->space != descriptor.space ||
                        source->cell != descriptor.cell)
                    {
                        return lux::cxx::unexpected(std::string{
                            "LXIP identity does not match its LXAI descriptor"
                        });
                    }
                    const auto cell = gridCoordinate(source->cell);
                    if (!cell)
                    {
                        return lux::cxx::unexpected(std::string{
                            "3D Instance Page has a non-3D cell"
                        });
                    }
                    std::map<
                        std::vector<std::string>,
                        Spatial3DInstancePageSource> layer_groups;
                    for (const auto& source_instance : source->instances)
                    {
                        std::vector<std::string> data_layers;
                        data_layers.reserve(source_instance.data_layers.size());
                        for (const auto& layer : source_instance.data_layers)
                        {
                            data_layers.emplace_back(layer.name());
                        }
                        auto [group, inserted] = layer_groups.try_emplace(
                            data_layers);
                        if (inserted)
                        {
                            group->second.space = source->space.value();
                            group->second.cell = *cell;
                            group->second.data_layers = std::move(data_layers);
                        }
                        const auto* position = std::get_if<
                            lux::math::Position3d>(
                                &source_instance.position);
                        if (!position)
                        {
                            return lux::cxx::unexpected(std::string{
                                "3D Instance Page contains a non-3D position"
                            });
                        }
                        const auto instance_id = uuids::uuid_name_generator{
                            source_instance.id.set.value()}(
                                std::to_string(
                                    source_instance.id.local_id));
                        group->second.instances.push_back({
                            lux::ecs::PersistentEntityId{instance_id},
                            *position,
                            source_instance.rotation,
                            source_instance.scale,
                            source_instance.mesh,
                            source_instance.material_instance,
                            source_instance.rgba8,
                            stableInstancePickId(
                                source_instance.id.set.value(),
                                source_instance.id.local_id)
                        });
                    }
                    for (auto& [_, group] : layer_groups)
                    {
                        result.instance_pages.push_back(std::move(group));
                    }
                    continue;
                }
                if (descriptor.kind !=
                    lux::authoring::EWorldPageSourceKind::TERRAIN)
                {
                    continue;
                }
                auto source = lux::authoring::loadWorldTerrainPage(
                    root_document,
                    descriptor.document_path,
                    *root);
                if (!source)
                {
                    return lux::cxx::unexpected(source.error());
                }
                const auto* owner = std::get_if<
                    lux::authoring::TerrainSetId>(&descriptor.owner);
                if (source->world != root->world || !owner ||
                    source->terrain_set != *owner ||
                    source->space != descriptor.space ||
                    source->cell != descriptor.cell)
                {
                    return lux::cxx::unexpected(std::string{
                        "LXTP identity does not match its LXAI descriptor"
                    });
                }
                const auto cell = gridCoordinate(source->cell);
                if (!cell)
                {
                    return lux::cxx::unexpected(std::string{
                        "3D Terrain Page has a non-3D cell"
                    });
                }
                result.terrain_pages.push_back({
                    source->terrain_set.value(),
                    source->space.value(),
                    *cell,
                    source->height_min,
                    source->height_max,
                    source->sample_spacing,
                    source->weight_layer_count,
                    std::move(source->heights),
                    std::move(source->weight_planes),
                    std::move(source->holes)
                });
            }
        }
        return result;
    }
} // namespace lux::toolchain
