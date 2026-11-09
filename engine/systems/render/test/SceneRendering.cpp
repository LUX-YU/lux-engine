#include "RenderTest.hpp"
#include "../../../../modules/function/material/test/MaterialTest.hpp"
#include <lux/engine/Engine.hpp>
#include <lux/engine/Init.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/ecs/RenderableMeshComponent.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/platform/FileIo.hpp>
#include <lux/engine/render/capabilities/RasterSurface.hpp>
#include <lux/engine/resource/asset/storage/directory/DirectoryAssetProvider.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/systems/render/RenderSystemConfiguration.hpp>
#include <lux/engine/ecs/VisibilityComponent.hpp>
#include <lux/engine/systems/render/mesh/MeshInstanceReaction.hpp>
#include <lux/engine/systems/render/mesh/MeshRenderResourceComponent.hpp>
#include <lux/engine/systems/render/mesh/MeshTransformCollector.hpp>
#include <lux/engine/systems/render/view/ViewCollector.hpp>
#include <lux/engine/systems/render/view/ViewReaction.hpp>
#include <lux/engine/systems/render/view/RenderViewComponent.hpp>
#include <lux/engine/systems/render/view/RenderTargetComponent.hpp>
#include <lux/engine/world/World.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>

using namespace integration_test;

lux::cxx::type_info noGeometryCapabilityType(bool surface) noexcept;
lux::render::RenderResult<void> registerNoGeometryCapabilities(void*, lux::render::RenderServer&) noexcept;

namespace
{
    template <class Sender> auto wait(execution::ExecutionRuntime& execution, Sender sender)
    {
        std::optional<typename execution::detail::ExecutionTypes<Sender>::Result> result;
        check(bool(execution.spawn(stdexec::then(
            execution::asResult(stdexec::starts_on(execution.mainScheduler(), std::move(sender))),
            [&](auto value) noexcept { result.emplace(std::move(value)); }
        ))));
        check(bool(execution.waitUntil([&]() noexcept { return result.has_value(); })));
        return std::move(*result);
    }

    rdesc::MaterialProgramSet materialProgram(asset::AssetId texture)
    {
        using namespace material;
        MaterialGraph graph;
        graph.shading_model = rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
        graph.texture_slots = {{"albedo", texture}};
        auto output = material_test::add(graph, MaterialOutputSurface{});
        auto albedo = material_test::add(graph, MaterialSampleTexture{0});
        auto uv = material_test::add(graph, MaterialInput{EMaterialInput::UV0});
        check(bool(material_test::connect(graph, uv, 0, albedo, 0)));
        check(bool(material_test::connect(graph, albedo, 0, output, 0)));
        constexpr std::array purposes{rdesc::EMaterialPurpose::GBUFFER, rdesc::EMaterialPurpose::SHADOW};
        return take(compileMaterial(graph, purposes)).programs;
    }
}

int main(int argc, char** argv)
{
    check(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]) / "scene-rendering";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    constexpr std::size_t limit = 16U << 20U;
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("f390c841-9e76-475e-9c70-0addab58a707")};
    const asset::AssetId mesh_id{ids("mesh")}, material_id{ids("material")}, texture_id{ids("texture")};
    rdesc::Mesh mesh;
    mesh.vertices.resize(3);
    mesh.indices = {0, 1, 2};
    mesh.vertices[0].position = {-0.5f, -0.5f, 0};
    mesh.vertices[1].position = {0.5f, -0.5f, 0};
    mesh.vertices[2].position = {0, 0.5f, 0};
    for (auto& vertex : mesh.vertices)
    {
        vertex.normal = {0, 0, 1};
        vertex.tangent = {1, 0, 0};
        vertex.bitangent = {0, 1, 0};
        vertex.uv = {0, 0};
        vertex.bone = {};
    }
    mesh.bounds = math::AABB{{-0.5f, -0.5f, 0}, {0.5f, 0.5f, 0}};
    rdesc::MaterialDescription material;
    material.programs = materialProgram(texture_id);
    material.texture_slot_ids = {texture_id};
    rdesc::TextureInfo texture_info{};
    texture_info.width = texture_info.height = 2;
    texture_info.channel = 4;
    texture_info.mip_ranges[0] = {0, 16, 2, 2};
    const std::array<std::byte, 16> pixels{};
    auto texture = take(rdesc::Texture::copyOf(texture_info, pixels));
    const auto write_asset = [&]<class Asset>(
        asset::AssetId id, const typename Asset::data_type& data, std::string name
    )
    {
        auto encoded = take(asset::TAssetSerDeser<Asset>::encode(
            {.id = id, .type = Asset::asset_type}, data, {}, asset::AssetEncodeLimits{limit}
        ));
        check(bool(platform::writeFileAtomic(root / name, encoded, platform::EFileWriteMode::CREATE)));
    };
    write_asset.template operator()<asset::MeshAsset>(mesh_id, mesh, "mesh.luxasset");
    write_asset.template operator()<asset::MaterialAsset>(material_id, material, "material.luxasset");
    write_asset.template operator()<asset::TextureAsset>(texture_id, texture, "texture.luxasset");
    const std::array formats{rdesc::ETexturePixelFormat::R8_UNORM,
                             rdesc::ETexturePixelFormat::RG8_UNORM, rdesc::ETexturePixelFormat::BC5_UNORM};
    for (const auto format : formats)
    {
        rdesc::TextureInfo info{};
        info.pixel_format = format;
        info.color_space = rdesc::ETextureColorSpace::DATA;
        const bool compressed = rdesc::isCompressedFormat(format);
        info.width = info.height = compressed ? 4 : 2;
        info.channel = format == rdesc::ETexturePixelFormat::R8_UNORM ? 1 : 2;
        info.flags = compressed ? rdesc::toUnderlying(rdesc::ETextureAssetFlags::COMPRESSED) : 0;
        const auto size = compressed ? 16u : 4u * info.channel;
        info.mip_ranges[0] = {0, size, static_cast<std::uint32_t>(info.width), static_cast<std::uint32_t>(info.height)};
        std::vector<std::byte> bytes(size);
        auto data = take(rdesc::Texture::copyOf(info, bytes));
        const auto name = std::to_string(static_cast<unsigned>(format));
        write_asset.template operator()<asset::TextureAsset>(asset::AssetId{ids(name)}, data, name + ".luxasset");
    }
    auto provider = take(asset::DirectoryAssetProvider::create(root));
    asset::MountLease mount;
    std::optional<MeshUpload> meshes;
    std::optional<MaterialUpload> materials;
    std::optional<TextureUpload> textures;
    RenderServerConfig configuration{"engine.scene.integration", true};
    configuration.uploads.emplace();
    configuration.frames_in_flight = 3;
    configuration.max_frames_ahead = 2;
    DeviceOptions device;
    device.texture_descriptors = true;
    device.buffer_device_address = true;
    device.multiple_queues = true;
    RenderServer* server{};
    auto context = take(engine::init(
        {2, 64, {16}, execution::BlockingSchedulerConfig{1, 32}}, {limit, limit, 16}, configuration, device, {}, {},
        [&](RenderServer& value) noexcept -> RenderResult<void>
        { server = &value; return registerNoGeometryCapabilities(nullptr, value); }
    ));
    mount = take(context->assetVfs().mount({"/assets", provider}));
    auto& execution = context->execution();
    auto& assets = context->assets();
    auto& runtime = context->renderClient();
    auto& frames = RenderFrameSubmission::instance();
    const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    const EffectOutputDesc effect_output{target.format, target.samples, target.kind, target.sampled};
    const auto choices = call<ListCapabilities>(
        runtime,
        {std::string(kRasterSurfaceCapability), "lux.render.raster_surface", effect_output}
    );
    if (choices.implementations.size() != 1 || choices.implementations[0].unavailable.type)
    {
        std::fprintf(stderr, "Raster implementations=%zu unavailable=%llu\n", choices.implementations.size(),
            choices.implementations.empty() ? 0ULL : choices.implementations[0].unavailable.type);
    }
    check(choices.implementations.size() == 1 && !choices.implementations[0].unavailable.type);
    const auto implementation = choices.implementations[0].implementation_id;
    RenderSystemConfiguration saved;
    saved.output = effect_output;
    saved.composition.contributions.push_back({
        "mesh", std::string(kRasterSurfaceCapability), "lux.render.raster_surface",
        take(encodeRasterSurfaceConfiguration({}))
    });
    saved.composition.outputs.push_back({"mesh", "albedo_metallic"});
    const auto codec = renderConfigurationCodec();
    std::vector<std::byte> configuration_bytes;
    check(bool(codec.encode(&saved, configuration_bytes)));
    RenderSystemConfiguration decoded;
    check(bool(codec.decode(configuration_bytes, &decoded)) && decoded == saved);
    for (const bool surface : {false, true})
    {
        const auto list = call<ListCapabilities>(runtime, {
            std::string(noGeometryCapabilityType(surface).name()), "test.scene.no_geometry.a", effect_output
        });
        check(list.implementations.size() == 1);
        check(list.implementations[0].unavailable.type == (surface ? kRuntimeUnsupported : error::ErrorId{}));
    }
    const std::array command_names{
        CommandName{Mesh3dCommands::UploadMesh::name},
        CommandName{Mesh3dCommands::ReleaseMesh::name},
        CommandName{ImageCommands::Upload::name},
        CommandName{ImageCommands::Release::name},
        CommandName{MaterialCommands::Create::name},
        CommandName{MaterialCommands::Release::name},
        CommandName{Mesh3dCommands::CreateInstances::name},
        CommandName{Mesh3dCommands::ReleaseInstance::name}
    };
    const auto resolved_commands = call<QueryCommandIds>(runtime, command_names);
    const auto upload_mesh = resolved_commands.ids[0];
    const auto release_mesh = resolved_commands.ids[1];
    const auto upload_texture = resolved_commands.ids[2];
    const auto release_texture = resolved_commands.ids[3];
    const auto create_material = resolved_commands.ids[4];
    const auto release_material = resolved_commands.ids[5];
    const auto create_instances = resolved_commands.ids[6];
    const auto release_instance = resolved_commands.ids[7];
    meshes.emplace(take(MeshUpload::create(
        runtime, implementation, upload_mesh, release_mesh
    )));
    materials.emplace(take(MaterialUpload::create(
        runtime, implementation, create_material, release_material
    )));
    textures.emplace(take(TextureUpload::create(
        runtime, implementation, upload_texture, release_texture
    )));
    check(bool(assets.registerUse<MeshUploadDefinition>(*meshes)));
    check(bool(assets.registerUse<TextureUploadDefinition>(*textures)));
    check(bool(assets.registerUse<MaterialUploadDefinition>(*materials, *textures)));
    RenderCapabilityMetaRegistry metadata;
    using FactoryResult = RenderResult<std::unique_ptr<MeshInstanceReaction>>;
    using Factory = FactoryResult (*)(RenderSystem&, CapabilityImplementationId) noexcept;
    const auto rejected_factory = metadata.addReaction<MeshInstanceReaction>(Factory{});
    check(!rejected_factory && rejected_factory.error().type == kCapabilityDeclaration);
    check(metadata.find(MeshInstanceReaction::capability) == nullptr);
    check(bool(metadata.addSceneReaction<ViewReaction>()));
    check(bool(metadata.addReaction<MeshInstanceReaction>(
        [&meshes, &materials, create = create_instances, release = release_instance](
            RenderSystem& system, CapabilityImplementationId selected
        ) noexcept
        {
            return MeshInstanceReaction::create(system, selected, *meshes, *materials, create, release);
        }
    )));
    check(bool(metadata.addCollector<MeshTransformCollector>()));
    check(bool(metadata.addCollector<ViewCollector<Mesh3dCapability>>()));
    // Storage and Feature are not visible to this ordinary Engine consumer.
    CapabilityComposition effect;
    effect.contributions.push_back({
        "mesh", cxx::make_basic_type_info<Mesh3dCapability>(), implementation,
        take(encodeRasterSurfaceConfiguration({}))
    });
    effect.outputs.push_back({"mesh", "albedo_metallic"});
    engine::Engine engine{*context};
    auto world = std::make_unique<world::World>(world::WorldDescription{"Scene integration", {{"mesh3d", {}}}});
    auto& registry = world->registry();
    const std::array entities{registry.create(), registry.create()};
    for (auto entity : entities)
    {
        ecs::Mesh3D geometry;
        geometry.value.mesh = mesh_id;
        registry.emplace<ecs::Mesh3D>(entity, geometry);
        registry.emplace<ecs::MaterialComponent>(entity, std::vector{material_id});
        registry.emplace<ecs::Transform3D>(entity);
        registry.emplace<ecs::RenderableMeshComponent>(entity);
    }
    const std::array cameras{registry.create(), registry.create()};
    for (auto entity : cameras)
    {
        registry.emplace<scene::ViewComponent>(entity);
        registry.emplace<ecs::Transform3D>(entity).translation.z() = 3;
        registry.emplace<RenderTargetComponent>(entity, target);
    }
    for (auto entity : entities)
        registry.emplace<ecs::VisibilityComponent>(entity, true, ecs::EVisibilityRequirement::CANDIDATE);
    auto system = take(RenderSystem::create(
        std::move(effect), effect_output, *world, *context, metadata
    ));
    auto* rendering = system.get();
    auto candidate = std::make_unique<scene::SceneInstance>(
        std::move(world), std::make_unique<simulation::Simulation>()
    );
    candidate->add(std::move(system));
    const auto scene_id = take(engine.addScene(candidate, true));
    check(bool(wait(execution, engine.whenSceneInitialized(scene_id))));
    const auto compiled = call<QueryRenderStats>(runtime, {}).compiled_graphs;
    auto texture_ref = take(assets.acquire<asset::TextureAsset>(texture_id));
    auto material_ref = take(assets.acquire<asset::MaterialAsset>(material_id));
    auto mesh_ref = take(assets.acquire<asset::MeshAsset>(mesh_id));
    unsigned notifications = 0, unrelated = 0;
    auto notified = [&](asset::AssetId id) noexcept { check(id == texture_id); ++notifications; };
    auto other = [&](asset::AssetId) noexcept { ++unrelated; };
    auto observation = assets.observe<&decltype(notified)::operator()>(texture_ref, notified);
    auto unrelated_observation = assets.observe<&decltype(other)::operator()>(mesh_ref, other);
    auto layered_info = texture_info;
    layered_info.layers = 2;
    layered_info.mip_ranges[0].size = 32;
    const std::array<std::byte, 32> layered_pixels{};
    auto layered = take(rdesc::Texture::copyOf(layered_info, layered_pixels));
    bool rejected = false, restored = false;
    RenderError preparation_error;
    unsigned phase = 0, rounds = 0, failure_round = 0;
    AttachmentId original, secondary, resized;
    MeshInstanceId previous_instance;
    MeshId shared_mesh;
    std::vector<std::pair<AttachmentId, CameraInput>> published;
    const auto camera_input = frameDataBinding<Mesh3dCapability, CameraInput>({}, 0, SceneId{}).destination.input;
    const auto transform_input = frameDataBinding<Mesh3dCapability, TransformData>({}, 0, SceneId{}).destination.input;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    engine.addEngineStageCallback([&]() noexcept -> engine::Engine::Result
    {
        check(std::chrono::steady_clock::now() < deadline);
        ++rounds;
        const auto packet = frames.frame().seal(1); // Read-only inspection; only FrameSubmission sends the packet.
        const FrameDataView data{packet.blocks};
        std::size_t camera_updates = 0, transform_updates = 0;
        for (const auto& entry : packet.entries)
        {
            if (entry.destination.input == transform_input) ++transform_updates;
            const auto* attachment = std::get_if<AttachmentId>(&entry.destination.scope);
            if (!attachment || entry.destination.input != camera_input) continue;
            ++camera_updates;
            const auto bytes = data.read(entry.payload);
            check(bool(bytes) && bytes->size() == sizeof(CameraInput));
            CameraInput value;
            std::memcpy(&value, bytes->data(), sizeof(value));
            const auto found = std::ranges::find(published, *attachment, &std::pair<AttachmentId, CameraInput>::first);
            if (found == published.end()) published.emplace_back(*attachment, value);
            else found->second = value;
        }
        const auto* first = registry.try_get<RenderViewComponent>(cameras[0]);
        const auto* second = registry.try_get<RenderViewComponent>(cameras[1]);
        const auto* resource = registry.try_get<MeshRenderResourceComponent>(entities[0]);
        const auto* shared = registry.try_get<MeshRenderResourceComponent>(entities[1]);
        const auto camera = [&](AttachmentId id) -> const CameraInput*
        {
            const auto found = std::ranges::find(published, id, &std::pair<AttachmentId, CameraInput>::first);
            return found == published.end() ? nullptr : &found->second;
        };
        if (phase == 0 && first && second && resource && shared && camera(first->attachment))
        {
            original = first->attachment;
            secondary = second->attachment;
            check(original != secondary && first->target != second->target && first->view != second->view);
            check(*resource->mesh == *shared->mesh && *resource->materials[0] == *shared->materials[0]);
            shared_mesh = *resource->mesh;
            check(std::abs(camera(original)->view[14] + 3.F) < .0001F);
            registry.patch<ecs::Transform3D>(cameras[0], [](auto& value) noexcept { value.translation.x() = 1; });
            phase = 1;
        }
        else if (phase == 1 && camera(original) && std::abs(camera(original)->view[12] + 1.F) < .0001F)
        {
            check(first->attachment == original && second->attachment == secondary);
            check(transform_updates == 0); // Moving a View cannot dirty Mesh transforms.
            check(camera(secondary) && std::abs(camera(secondary)->view[12]) < .0001F);
            for (auto entity : entities)
            {
                registry.patch<ecs::Transform3D>(entity, [](auto& value) noexcept { value.translation.y() += .1; });
            }
            phase = 10;
        }
        else if (phase == 10)
        {
            // Mesh movement must not produce CameraInput for either unchanged View.
            check(camera_updates == 0);
            registry.remove<RenderTargetComponent>(cameras[0]);
            phase = 2;
        }
        else if (phase == 2 && !first)
        {
            check(second && second->attachment == secondary);
            registry.emplace<RenderTargetComponent>(cameras[0], target);
            phase = 3;
        }
        else if (phase == 3 && first)
        {
            check(first->attachment != original);
            resized = first->attachment;
            registry.patch<RenderTargetComponent>(cameras[0], [](auto& value) noexcept
            { value.description.width = 96; });
            registry.remove<RenderTargetComponent>(cameras[1]);
            phase = 4;
        }
        else if (phase == 4 && first && first->description.width == 96 && !second)
        {
            check(first->attachment == resized && resource && *resource->mesh == shared_mesh);
            previous_instance = resource->instances[0];
            auto failed_candidate = stdexec::let_value(assets.write(texture_ref, layered), [&](auto written) noexcept
            {
                check(bool(written));
                return assets.use<MaterialUploadDefinition>(material_ref, *materials);
            });
            check(bool(execution.spawn(stdexec::then(execution::asResult(std::move(failed_candidate)),
                [&](auto result) noexcept
                {
                    check(!result && result.error().domainFailure());
                    preparation_error = *result.error().domainFailure();
                    rejected = true;
                }
            ))));
            phase = 5;
        }
        else if (phase == 5 && rejected)
        {
            check(preparation_error.type == kDeviceArgument && resource && resource->instances[0] == previous_instance);
            failure_round = rounds;
            phase = 6;
        }
        else if (phase == 6 && rounds >= failure_round + 4)
        {
            check(first && first->attachment == resized && resource->instances[0] == previous_instance);
            check(bool(execution.spawn(stdexec::then(execution::asResult(assets.write(texture_ref, texture)),
                [&](auto result) noexcept { check(bool(result)); restored = true; }
            ))));
            phase = 7;
        }
        else if (phase == 7 && restored && resource && resource->instances[0] != previous_instance)
        {
            check(*resource->mesh == shared_mesh && notifications == 2 && unrelated == 0);
            registry.destroy(entities[0]);
            phase = 8;
        }
        else if (phase == 8 && rounds > failure_round + 12)
        {
            check(shared && *shared->mesh == shared_mesh);
            phase = 9;
            engine.requestStop();
        }
        return {};
    }, engine::EEngineStage::END_UPDATE);
    check(bool(engine.run()) && phase == 9);
    check(call<QueryRenderStats>(runtime, {}).compiled_graphs == compiled);
    check(bool(wait(execution, engine.removeScene(scene_id))));
    check(assets.usage(mesh_id)->use_readers == 0);
    for (const auto format : formats)
    {
        const auto id = asset::AssetId{ids(std::to_string(static_cast<unsigned>(format)))};
        auto reference = take(assets.acquire<asset::TextureAsset>(id));
        auto uploaded = wait(execution, assets.use<TextureUploadDefinition>(reference, *textures));
        check(bool(uploaded) && !(**uploaded).isNull());
    }
    check(server->stats().validation_errors == 0);
    std::puts("PASS Scene assembly, shared assets, camera collection, resize, candidate failure/recovery and RAII");
}
