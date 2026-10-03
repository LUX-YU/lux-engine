#include <lux/engine/editor/material/MaterialPreviewRecipe.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>
#include <numbers>
namespace lux::editor::material
{
    namespace
    {
        template <class T> T previewIdentity(std::uint8_t tail)
        {
            std::array<std::uint8_t, 16>
                bytes{0xf3, 0x29, 0x45, 0x93, 0x22, 0x13, 0x41, 0xca, 0xbb, 0x33, 0xa6, 0x24, 0x71, 0x06, 0x23, tail};
            return T{uuids::uuid{bytes}};
        }
        template <class Error> auto failed(std::string domain, Error error)
        {
            return lux::cxx::unexpected(
                MaterialPreviewFailure{EMaterialPreviewError::PREPARATION, std::move(domain), std::move(error)}
            );
        }
        lux::cxx::SharedBytes<> own(std::vector<std::byte> bytes)
        {
            auto owner = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
            return lux::cxx::SharedBytes<>::fromOwner(owner, *owner);
        }

        MaterialPreviewResult<lux::cxx::SharedBytes<>> sphereImage(lux::asset::AssetId id)
        {
            auto mesh = std::make_shared<lux::rdesc::Mesh>();
            constexpr std::uint32_t slices = 48, rings = 24;
            mesh->vertices.reserve((slices + 1) * (rings + 1));
            for (std::uint32_t y{}; y <= rings; ++y)
            {
                const float v = float(y) / rings, latitude = v * std::numbers::pi_v<float>;
                for (std::uint32_t x{}; x <= slices; ++x)
                {
                    const float u = float(x) / slices, longitude = u * 2 * std::numbers::pi_v<float>;
                    lux::rdesc::Vertex vertex{};
                    vertex.position = {
                        std::sin(latitude) * std::cos(longitude),
                        std::cos(latitude),
                        std::sin(latitude) * std::sin(longitude)
                    };
                    vertex.normal = vertex.position;
                    vertex.tangent = {-std::sin(longitude), 0, std::cos(longitude)};
                    vertex.bitangent = vertex.normal.cross(vertex.tangent);
                    vertex.uv = {u, v};
                    mesh->vertices.push_back(vertex);
                }
            }
            for (std::uint32_t y{}; y < rings; ++y)
                for (std::uint32_t x{}; x < slices; ++x)
                {
                    const auto a = y * (slices + 1) + x, b = a + slices + 1;
                    if (y != 0)
                        mesh->indices.insert(mesh->indices.end(), {a, a + 1, b});
                    if (y + 1 != rings)
                        mesh->indices.insert(mesh->indices.end(), {a + 1, b + 1, b});
                }
            auto asset = lux::asset::MeshAsset::create({id, lux::asset::MeshAsset::asset_type}, std::move(mesh));
            if (!asset)
                return failed("preview.mesh", asset.error());
            auto encoded = lux::asset::TAssetSerDeser<lux::asset::MeshAsset>::encode(
                **asset,
                lux::asset::AssetEncodeLimits{4U * 1024U * 1024U}
            );
            if (!encoded)
                return failed("preview.mesh.encode", encoded.error());
            return own(std::move(*encoded));
        }

        MaterialPreviewResult<std::shared_ptr<const lux::world::WorldDescription>> previewWorld(
            lux::cxx::SharedBytes<>& volume
        )
        {
            using namespace lux::world;
            const auto bundle = previewIdentity<WorldBundleId>(1);
            const auto generation = previewIdentity<WorldBundleGeneration>(2);
            const std::array records{WorldPartitionRecord{previewIdentity<WorldPartitionId>(3), 0, 1}};
            const std::array extents{WorldPartitionExtent{0, 1, 1}};
            auto data = encodeWorldPartitionData({0}, {});
            auto table = encodeWorldPartitionTablePage({0}, records, extents);
            if (!data)
                return failed("preview.partition", data.error());
            if (!table)
                return failed("preview.table", table.error());
            const std::array chunks{
                WorldStorageChunkInput{EWorldStorageChunkKind::PARTITION_TABLE_PAGE, EWorldStorageCodec::NONE, *table},
                WorldStorageChunkInput{EWorldStorageChunkKind::WORLD_PARTITION_DATA, EWorldStorageCodec::NONE, *data}
            };
            auto encoded = encodeWorldStorageVolume(bundle, generation, 0, chunks);
            if (!encoded)
                return failed("preview.volume", encoded.error());
            volume = own(std::move(*encoded));
            WorldDescriptionBuilder world;
            auto built = world.setIdentity(bundle, generation, "Material preview");
            if (built)
                built = world.setPartitioner({worldPartitionerId("lux.spatial.builtin.single"), 1}, 1);
            if (built)
                built = world.addStorageVolume({"Preview.wvol", 1, 2, volume.size()});
            if (built)
                built = world.addPartitionTablePage({{0}, 1, {0, 0}});
            if (!built)
                return failed("preview.world", built.error());
            auto result = std::move(world).build();
            if (!result)
                return failed("preview.world", result.error());
            return std::make_shared<const WorldDescription>(std::move(*result));
        }
    }
    MaterialPreviewResult<MaterialPreviewRecipe>
    makeMaterialPreviewRecipe(std::span<const render::RenderFeatureRegistration> features)
    {
        MaterialPreviewRecipe recipe;
        recipe.mesh = previewIdentity<asset::AssetId>(4);
        auto mesh = sphereImage(recipe.mesh);
        if (!mesh)
            return cxx::unexpected(mesh.error());
        recipe.mesh_image = std::move(*mesh);
        auto world = previewWorld(recipe.world_volume);
        if (!world)
            return cxx::unexpected(world.error());
        lux::simulation::SimulationDescriptionBuilder simulation;
        auto transform = lux::scene::makeTransformSystemConfiguration(64, {256, 65536});
        if (!transform)
            return failed("preview.transform", transform.error());
        auto rules = std::move(simulation).build();
        if (!rules)
            return failed("preview.simulation", rules.error());
        lux::scene::RenderSystemConfiguration configuration;
        for (const auto name :
             {"lux.render.view_camera.v1",
              "lux.render.material.v1",
              "lux.render.mesh_stack.v1",
              "lux.render.light.v1",
              "lux.render.forward_mesh.v1",
              "lux.render.shadow_map.v1"})
        {
            auto found = std::ranges::find(features, std::string_view{name}, [](const auto& entry) {
                return entry.factory.descriptor.canonical_name;
            });
            if (found == features.end())
                return failed("preview.feature.missing", std::string(name));
            std::vector<std::byte> bytes;
            auto encoded = found->configuration.portable.encode_default(bytes);
            if (!encoded)
                return failed("preview.feature.configuration", encoded.error());
            configuration.features.push_back(
                {found->factory.descriptor.type,
                 std::move(bytes),
                 std::string(found->configuration.schema),
                 found->configuration.schema_version}
            );
        }
        const auto registration = lux::scene::builtinRenderSystemRegistration();
        std::vector<std::byte> bytes;
        auto encoded = registration.configuration.encode(&configuration, bytes);
        if (!encoded)
            return failed("preview.render.configuration", encoded.error());
        lux::scene::SceneDescriptionBuilder description;
        const auto transform_registration = lux::scene::transformSystemRegistration();
        auto transform_added = description.addSystem(
            {1},
            "transform",
            transform_registration.type,
            transform_registration.description->version,
            transform_registration.description->configuration_schema_name,
            transform_registration.description->configuration_schema_version,
            *transform
        );
        if (!transform_added)
            return failed("preview.transform", transform_added.error());
        auto render_added = description.addSystem(
            system::SystemInstanceId{2},
            "preview-render",
            registration.type,
            registration.description->version,
            registration.description->configuration_schema_name,
            registration.description->configuration_schema_version,
            bytes
        );
        if (!render_added)
            return failed("preview.description", render_added.error());
        auto scene = std::move(description).buildResolved();
        if (!scene)
            return failed("preview.description", scene.error());

        recipe.world = std::move(*world);
        recipe.scene = std::make_shared<const lux::scene::SceneDescription>(std::move(*scene));
        recipe.simulation = std::make_shared<const lux::simulation::SimulationDescription>(std::move(*rules));
        recipe.camera_pose.translation = {0, 0, 3.5};
        recipe.camera = lux::scene::Camera{lux::scene::PerspectiveProjection{}, true};
        recipe.light_pose.rotation = Eigen::Quaterniond(
            Eigen::AngleAxisd(-0.6, Eigen::Vector3d::UnitX()) * Eigen::AngleAxisd(-0.5, Eigen::Vector3d::UnitY())
        );
        recipe.light.type = lux::rdesc::ELightType::DIRECTIONAL;
        recipe.light.intensity = 3.0F;
        recipe.light.cast_shadow = false;
        return recipe;
    }
}
