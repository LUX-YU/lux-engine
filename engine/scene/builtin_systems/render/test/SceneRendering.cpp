#include "../../../../../modules/function/material/test/MaterialTest.hpp"

#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/storage/RasterMeshInstances.hpp>
#include <lux/engine/rendering/RenderAssets.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <latch>
#include <map>
#include <semaphore>
#include <thread>

using namespace lux;
using namespace lux::render;
using namespace lux::rendering;
using namespace std::chrono_literals;

#define CHECK(value) material_test::check(static_cast<bool>(value))

template <class T> auto take(T result, std::source_location source = std::source_location::current())
{
    if constexpr (requires { result.error().type; })
    {
        if (!result)
        {
            reportRenderFailure(result.error(), source.function_name());
        }
    }
    material_test::check(static_cast<bool>(result), source);
    return std::move(*result);
}

namespace
{
    class CookedAssets final : public asset::IAssetProvider
    {
    public:
        std::optional<asset::AssetId> resolve(std::string_view) const override
        {
            return {};
        }

        bool contains(const asset::AssetId& id) const override
        {
            return images.contains(id);
        }

        cxx::expected<asset::AssetBlob, asset::EAssetStorageError> open(const asset::AssetId& id, std::size_t limit)
            const override
        {
            CHECK(std::this_thread::get_id() != main_thread);
            const auto found = images.find(id);
            if (found == images.end())
            {
                return cxx::unexpected(asset::EAssetStorageError::NOT_FOUND);
            }
            if (found->second.size() > limit)
            {
                return cxx::unexpected(asset::EAssetStorageError::LIMIT_EXCEEDED);
            }
            return asset::AssetBlob::fromShared(cxx::SharedBytes<>::copyOf(found->second));
        }

        void enumerate(const std::function<void(const asset::ProviderEntry&)>&) const override {}

        std::optional<std::string> pathOf(const asset::AssetId&) const override
        {
            return {};
        }

        std::map<asset::AssetId, std::vector<std::byte>> images;
        std::thread::id main_thread = std::this_thread::get_id();
    };

    template <RenderCommand C> typename C::Response call(RenderExecution& rendering, typename C::Request input)
    {
        static_assert(!C::frame_scoped, "Frame-scoped controls require the application's maintenance frames");
        auto bound = take(rendering.mainClient().bind<C>());
        typename C::Response response;
        bool complete = false;
        auto task = take(rendering.execution().submit(
            {},
            [&](execution::TaskReporter) noexcept
            {
                return stdexec::then(
                    rendering.mainClient().request(bound, input),
                    [&](typename C::Response result) noexcept
                    {
                        response = std::move(result);
                        complete = true;
                    }
                );
            }
        ));
        CHECK(rendering.execution().waitUntil(
            [&]() noexcept
            {
                rendering.consumeResponses();
                return complete;
            }
        ));
        CHECK(response.error.type == 0);
        return response;
    }

    rdesc::MaterialProgramSet materialProgram(asset::AssetId texture)
    {
        using namespace material;
        MaterialGraph graph;
        graph.shading_model = rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
        graph.render_state.double_sided = false;
        graph.texture_slots = {{"albedo", texture}};
        auto output = material_test::add(graph, MaterialOutputSurface{});
        auto albedo = material_test::add(graph, MaterialSampleTexture{0});
        auto uv = material_test::add(graph, MaterialInput{EMaterialInput::UV0});
        CHECK(material_test::connect(graph, uv, 0, albedo, 0));
        CHECK(material_test::connect(graph, albedo, 0, output, 0));
        constexpr std::array purposes{rdesc::EMaterialPurpose::GBUFFER, rdesc::EMaterialPurpose::SHADOW};
        return take(compileMaterial(graph, purposes)).programs;
    }
} // namespace

int main()
{
    auto execution =
        take(execution::ExecutionRuntime::create({2, 256, {16}, execution::BlockingSchedulerConfig{1, 16}}));
    execution::TaskScope reads(execution), project_tasks(execution), frames(execution);
    auto provider = std::make_shared<CookedAssets>();
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("f390c841-9e76-475e-9c70-0addab58a707")};
    const asset::AssetId mesh_id{ids("mesh")}, material_id{ids("material")}, texture_id{ids("texture")};
    auto mesh = std::make_shared<rdesc::Mesh>();
    mesh->vertices.resize(3);
    mesh->indices = {0, 1, 2};
    mesh->vertices[0].position = {-0.5f, -0.5f, 0};
    mesh->vertices[1].position = {0.5f, -0.5f, 0};
    mesh->vertices[2].position = {0, 0.5f, 0};
    for (auto& vertex : mesh->vertices)
    {
        vertex.normal = {0, 0, 1};
        vertex.tangent = {1, 0, 0};
        vertex.bitangent = {0, 1, 0};
        vertex.uv = {0, 0};
        vertex.bone = {};
    }
    mesh->bounds = math::AABB{{-0.5f, -0.5f, 0}, {0.5f, 0.5f, 0}};
    const auto mesh_asset = take(asset::MeshAsset::create({mesh_id, asset::MeshAsset::asset_type}, mesh));
    provider->images[mesh_id] =
        take(asset::TAssetSerDeser<asset::MeshAsset>::encode(*mesh_asset, asset::AssetEncodeLimits{1 << 20}));
    auto material = std::make_shared<rdesc::MaterialDescription>();
    material->programs = materialProgram(texture_id);
    material->texture_slot_ids = {texture_id};
    const auto material_asset =
        take(asset::MaterialAsset::create({material_id, asset::MaterialAsset::asset_type}, material));
    provider->images[material_id] =
        take(asset::TAssetSerDeser<asset::MaterialAsset>::encode(*material_asset, asset::AssetEncodeLimits{1 << 20}));
    rdesc::TextureInfo info{};
    info.width = info.height = 2;
    info.channel = 4;
    info.mip_ranges[0] = {0, 16, 2, 2};
    const std::array<std::byte, 16> pixels{};
    auto texture = std::make_shared<rdesc::Texture>(take(rdesc::Texture::copyOf(info, pixels)));
    auto texture_asset = take(asset::TextureAsset::create({texture_id, asset::TextureAsset::asset_type}, texture));
    provider->images[texture_id] =
        take(asset::TAssetSerDeser<asset::TextureAsset>::encode(*texture_asset, asset::AssetEncodeLimits{1 << 20}));
    asset::AssetVfs vfs;
    auto mount = take(vfs.mount({"/assets", provider}));
    auto endpoint = take(process::asset_loading::VfsAssetReadEndpoint::create(
        vfs.view(),
        take(execution.blocking()),
        (reads).execution(),
        {8},
        (reads).stopToken()
    ));
    RenderExecutionConfig config;
    config.render.channel_name = "engine.scene.integration";
    config.render.validation = true;
    config.render.frames_in_flight = 3;
    config.render.max_frames_ahead = 2;
    config.render.uploads.emplace();
    config.device.buffer_device_address = true;
    config.device.texture_descriptors = true;
    config.device.multiple_queues = true;
    auto rendering = take(RenderExecution::create(std::move(config), execution));
    auto assets = take(RenderAssets::create(*rendering, endpoint->port()));
    const auto catalog = call<ListCommands>(*rendering, {});
    const auto before_upload = rendering->stats().commands;
    CHECK(assets->acquireTexture({texture_id, 1}));
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    // Deliberately do not dispatch Main. Blocking read, CPU decode and Asset transport must still advance.
    while (rendering->stats().commands == before_upload)
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
    CHECK(!assets->texture({texture_id, 1}));
    CHECK(execution.waitUntil(
        [&]() noexcept
        {
            rendering->consumeResponses();
            return assets->texture({texture_id, 1}) != nullptr;
        }
    ));

    scene::RenderAssetInput inputs;
    scene::CameraTarget view_request;
    view_request.description = {64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1};
    view_request.target = call<CreateTarget>(*rendering, {view_request.description}).target;
    scene::RenderSystemDependencies dependencies{rendering->mainClient(), execution, *assets, inputs};
    simulation::ecs::Registry registry;
    const auto camera = registry.create();
    registry.emplace<scene::Camera>(camera, scene::Camera{{scene::PerspectiveProjection{}}, true});
    Eigen::Affine3d camera_transform = Eigen::Affine3d::Identity();
    camera_transform.translation().z() = 3;
    registry.emplace<simulation::ecs::WorldTransform3D>(camera, camera_transform);
    const auto first = registry.create(), second = registry.create(), light = registry.create();
    for (auto entity : {first, second})
    {
        registry.emplace<simulation::ecs::Mesh3D>(entity, rdesc::MeshVisualDescription{mesh_id, material_id});
        registry.emplace<simulation::ecs::WorldTransform3D>(entity, Eigen::Affine3d::Identity());
    }
    rdesc::LightDescription sunlight;
    sunlight.type = rdesc::ELightType::DIRECTIONAL;
    sunlight.cast_shadow = true;
    sunlight.cascade_count = 2;
    sunlight.cascade_splits = {.25F, 1.F};
    registry.emplace<simulation::ecs::Light3D>(light, sunlight);
    registry.emplace<simulation::ecs::WorldTransform3D>(light, camera_transform);
    scene::RenderSystemConfiguration scene_configuration;
    scene_configuration.shadow_resolution = 64;
    scene_configuration.shadow_distance = 20;
    auto codec = scene::renderConfigurationCodec();
    std::vector<std::byte> encoded_configuration;
    CHECK(codec.encode(&scene_configuration, encoded_configuration));
    scene::RenderSystemConfiguration decoded_configuration;
    CHECK(codec.decode(encoded_configuration, &decoded_configuration));
    CHECK(decoded_configuration.shadow_resolution == 64 && scene::validRenderConfiguration(decoded_configuration));
    auto scene = std::make_unique<scene::RenderSystem>(registry, dependencies, scene_configuration);
    const auto first_added = scene->addCameraTarget(camera, view_request);
    CHECK(first_added);
    const auto first_view = *first_added;
    std::vector<std::pair<AttachmentId, MeshViewParameters>> published_views;
    std::uint64_t completed_frames = 0;
    std::stop_source scene_stop;
    bool scene_retired = false;
    bool compilation_held = false, projected_during_compile = false;
    const auto transform_command_id =
        std::ranges::find(catalog.commands, UpdateTransforms::name, &CommandInfo::name)->id;
    const auto inputs_command_id =
        std::ranges::find(catalog.commands, UpdateAttachmentInputs::name, &CommandInfo::name)->id;
    const auto published = [&](AttachmentId attachment) -> const MeshViewParameters*
    {
        auto found =
            std::ranges::find(published_views, attachment, &std::pair<AttachmentId, MeshViewParameters>::first);
        return found == published_views.end() ? nullptr : &found->second;
    };
    const auto drive = [&](bool draw) noexcept
    {
        rendering->consumeResponses();
        CHECK(execution.dispatchMainCompletions());
        CHECK(execution.dispatchTaskEvents());
        assets->maintain();
        if (scene && scene_stop.stop_requested())
        {
            scene::SceneStageContext context;
            context.stop = scene_stop.get_token();
            scene_retired = take(scene->maintain(context)) == scene::ESceneProgress::COMPLETE;
        }
        if (!draw && !rendering->mainClient().hasQueuedFrameCommands())
        {
            return;
        }
        auto available = rendering->mainClient().beginFrame();
        if (!available)
        {
            CHECK(available.error().type == kRuntimeCapacity);
            return;
        }
        auto& frame = **available;

        if (scene && !scene_stop.stop_requested())
        {
            scene::SceneStageContext context;
            CHECK(scene->maintain(context));
            CHECK(scene->prepareInputs(context));
            CHECK(scene->publish(context));
            for (const auto& command : frame.commands)
            {
                if (command.command != inputs_command_id)
                {
                    continue;
                }
                const auto& input = command.request.get<AttachmentInputs>();
                CHECK(input.parameters.size() >= sizeof(MeshViewParameters));
                MeshViewParameters value;
                std::memcpy(&value, input.parameters.data(), sizeof(value));
                auto found = std::ranges::find(
                    published_views,
                    input.attachment,
                    &std::pair<AttachmentId, MeshViewParameters>::first
                );
                if (found == published_views.end())
                {
                    published_views.push_back({input.attachment, value});
                }
                else
                {
                    found->second = value;
                }
            }
            if (compilation_held)
            {
                projected_during_compile |= std::ranges::any_of(
                    frame.commands,
                    [&](const auto& command) noexcept { return command.command == transform_command_id; }
                );
            }
        }

        CHECK(frames.submit(
            {},
            [&](execution::TaskReporter) noexcept
            {
                return stdexec::then(
                    rendering->mainClient().submitFrame(frame),
                    [&, carrier = &frame](FrameCompletion result) noexcept
                    {
                        if (result.error.type != 0)
                        {
                            reportRenderFailure(result.error, "Scene frame");
                            std::fprintf(
                                stderr,
                                "Frame error %llu, outcome %u\n",
                                static_cast<unsigned long long>(result.error.type),
                                unsigned(result.outcome)
                            );
                            for (const auto& input : carrier->commands)
                            {
                                const auto command =
                                    std::ranges::find(catalog.commands, input.command, &CommandInfo::id);
                                std::fprintf(stderr, "  %s\n", command->name.c_str());
                            }
                        }
                        CHECK(result.error.type == 0);
                        ++completed_frames;
                        CHECK(rendering->mainClient().releaseFrame(*carrier));
                    }
                );
            }
        ));
    };
    while (scene->attachment(camera, first_view).isNull() || !published(scene->attachment(camera, first_view)) ||
           completed_frames < 12)
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        drive(true);
    }
    const auto* initial_parameters = published(scene->attachment(camera, first_view));
    CHECK(initial_parameters != nullptr);
    CHECK(std::abs(initial_parameters->view[14] + 3.F) < .0001F);
    const auto first_matrix = initial_parameters->view;
    registry.patch<simulation::ecs::WorldTransform3D>(
        camera,
        [](auto& pose) noexcept { pose.value.translation().x() = 1; }
    );
    while (published(scene->attachment(camera, first_view))->view == first_matrix)
    {
        drive(true);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    CHECK(std::abs(published(scene->attachment(camera, first_view))->view[12] + 1.F) < .0001F);
    // A second ordinary ECS CameraMan has no author primary role and owns an independent output.
    const auto editor_camera = registry.create();
    registry.emplace<scene::Camera>(editor_camera);
    registry.emplace<simulation::ecs::WorldTransform3D>(editor_camera, camera_transform);
    scene::CameraTarget editor_view = view_request;
    editor_view.description.width = 128;
    editor_view.target = call<CreateTarget>(*rendering, {editor_view.description}).target;
    const auto second_added = scene->addCameraTarget(editor_camera, editor_view);
    CHECK(second_added);
    const auto second_view = *second_added;
    while (scene->attachment(editor_camera, second_view).isNull() ||
           !published(scene->attachment(editor_camera, second_view)))
    {
        drive(true);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    CHECK(
        std::abs(
            published(scene->attachment(camera, first_view))->projection[0] /
                published(scene->attachment(editor_camera, second_view))->projection[0] -
            2.F
        ) < .0001F
    );
    CHECK(std::abs(published(scene->attachment(editor_camera, second_view))->view[12]) < .0001F);
    const auto first_attachment = scene->attachment(camera, first_view);
    const auto second_attachment = scene->attachment(editor_camera, second_view);
    registry.patch<scene::Camera>(camera, [](auto& value) noexcept { value.primary = false; });
    drive(true);
    CHECK(scene->attachment(camera, first_view) == first_attachment); // Selection belongs to Editor, not RenderSystem.
    registry.remove<scene::Camera>(camera);
    while (!scene->attachment(camera, first_view).isNull())
    {
        drive(true);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    CHECK(scene->cameraTargetError(camera, first_view).type != 0);
    CHECK(scene->attachment(editor_camera, second_view) == second_attachment);
    registry.emplace<scene::Camera>(camera);
    while (scene->attachment(camera, first_view).isNull() || !published(scene->attachment(camera, first_view)))
    {
        drive(true);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    CHECK(scene->attachment(camera, first_view) != first_attachment);
    CHECK(scene->cameraTargetError(camera, first_view).type == 0);
    const auto submission_start = rendering->stats().submissions;
    for (unsigned frame = 0; frame < 12; ++frame)
    {
        registry.patch<simulation::ecs::WorldTransform3D>(
            first,
            [frame](auto& pose) noexcept { pose.value.translation().x() = 0.01 * frame; }
        );
        registry.patch<simulation::ecs::WorldTransform3D>(
            light,
            [frame](auto& pose) noexcept { pose.value.translation().y() = 0.01 * frame; }
        );
        drive(true);
    }
    while (rendering->stats().submissions <= submission_start)
    {
        drive(true);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    const auto previous_target = view_request.target;
    std::latch cpu_entered{2};
    std::counting_semaphore<2> release_cpu{0};
    for (unsigned i = 0; i < 2; ++i)
    {
        CHECK(execution.spawn(
            stdexec::schedule(execution.cpu()) | stdexec::then(
                                                     [&]() noexcept
                                                     {
                                                         cpu_entered.count_down();
                                                         release_cpu.acquire();
                                                     }
                                                 )
        ));
    }
    cpu_entered.wait();
    compilation_held = true;
    const auto previous_attachment = scene->attachment(camera, first_view);
    view_request.target = call<CreateTarget>(*rendering, {view_request.description}).target;
    CHECK(scene->updateCameraTarget(camera, first_view, view_request));
    while (!projected_during_compile)
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        registry.patch<simulation::ecs::WorldTransform3D>(
            first,
            [](auto& pose) noexcept { pose.value.translation().x() += 0.01; }
        );
        drive(true);
    }
    CHECK(projected_during_compile);
    compilation_held = false;
    release_cpu.release(2);
    while (scene->attachment(camera, first_view) == previous_attachment)
    {
        drive(true);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    // Moving the view prepares the new output before revoking the previous writer.
    // The old Target is now independently releasable, without recreating Mesh/Material assets.
    auto old_target = previous_target;
    const auto remove_target = take(rendering->mainClient().bind<RemoveTarget>());
    bool target_released = false;
    auto removal = take(execution.submit(
        {},
        [&](execution::TaskReporter) noexcept
        {
            return stdexec::then(
                rendering->mainClient().request(remove_target, old_target),
                [&](AckResponse response) noexcept
                {
                    CHECK(response.error.type == 0);
                    target_released = true;
                }
            );
        }
    ));
    while (!target_released)
    {
        drive(false);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    CHECK(assets->mesh({mesh_id, inputs.revision}) != nullptr);
    // A valid layered asset reaches the real upload Handler, which rejects this single-layer
    // Storage contract. No previous instance/Effect is removed just because a candidate failed.
    auto rejected_provider = std::make_shared<CookedAssets>();
    rejected_provider->images = provider->images;
    auto layered_info = info;
    layered_info.layers = 2;
    layered_info.mip_ranges[0].size = 32;
    const std::array<std::byte, 32> layered_pixels{};
    auto layered = std::make_shared<rdesc::Texture>(take(rdesc::Texture::copyOf(layered_info, layered_pixels)));
    auto layered_asset = take(asset::TextureAsset::create({texture_id, asset::TextureAsset::asset_type}, layered));
    rejected_provider->images[texture_id] =
        take(asset::TAssetSerDeser<asset::TextureAsset>::encode(*layered_asset, asset::AssetEncodeLimits{1 << 20}));
    std::array<asset::MountLease, 1> previous_mount{std::move(mount)};
    const std::array<asset::MountDesc, 1> rejected_mount{{{"/assets", rejected_provider}}};
    auto rejected_registration = take(vfs.replaceMounts(previous_mount, rejected_mount));
    const auto last_good = scene->attachment(camera, first_view);
    const auto previous_asset_revision = inputs.revision;
    const auto last_mesh = assets->mesh({mesh_id, inputs.revision})->mesh;
    inputs.revision += 2; // Revision one was preloaded by the independent Asset Client check.
    inputs.snapshot = vfs.view().capture();
    while (scene->assetError(first).type == 0)
    {
        drive(true);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    CHECK(scene->assetError(first).type == kDeviceArgument);
    CHECK(
        scene->attachment(camera, first_view) == last_good &&
        assets->mesh({mesh_id, previous_asset_revision})->mesh == last_mesh
    );
    const auto continued = rendering->stats().submissions;
    while (rendering->stats().submissions < continued + 3)
    {
        drive(true);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    const std::array<asset::MountDesc, 1> restored_mount{{{"/assets", provider}}};
    auto restored_registration = take(vfs.replaceMounts(rejected_registration, restored_mount));
    ++inputs.revision;
    inputs.snapshot = vfs.view().capture();
    auto next_diagnostic = std::chrono::steady_clock::now() + 2s;
    while (scene->attachment(camera, first_view) == last_good || scene->assetError(first).type != 0)
    {
        drive(true);
        if (std::chrono::steady_clock::now() > next_diagnostic)
        {
            std::fprintf(
                stderr,
                "Candidate recovery: mesh=%d material=%d texture=%d error=%llu graphs=%llu queued=%d mesh_error=%llu "
                "texture_error=%llu commands=%llu\n",
                assets->mesh({mesh_id, inputs.revision}) != nullptr,
                assets->material({material_id, inputs.revision}) != nullptr,
                assets->texture({texture_id, inputs.revision}) != nullptr,
                static_cast<unsigned long long>(scene->assetError(first).type),
                static_cast<unsigned long long>(rendering->stats().compiled_graphs),
                rendering->mainClient().hasQueuedFrameCommands(),
                static_cast<unsigned long long>(assets->failure({mesh_id, inputs.revision}).type),
                static_cast<unsigned long long>(assets->failure({texture_id, inputs.revision}).type),
                static_cast<unsigned long long>(rendering->stats().commands)
            );
            next_diagnostic += 10s;
        }
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    // Separate project generations must not alias the same revision in the shared upload cache.
    const RenderAssetVersion other_project{texture_id, 1, 9};
    CHECK(assets->acquireTexture(other_project, endpoint->port(), inputs.snapshot));
    while (!assets->texture(other_project))
    {
        drive(true);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    CHECK(assets->texture(other_project)->texture != assets->texture({texture_id, 1})->texture);
    assets->releaseTexture(other_project);
    // These are the real data formats emitted by model cooking for AO, metal/roughness and normals.
    for (const auto format :
         {rdesc::ETexturePixelFormat::R8_UNORM,
          rdesc::ETexturePixelFormat::RG8_UNORM,
          rdesc::ETexturePixelFormat::BC5_UNORM})
    {
        rdesc::TextureInfo cooked_info{};
        cooked_info.pixel_format = format;
        cooked_info.color_space = rdesc::ETextureColorSpace::DATA;
        const bool compressed = rdesc::isCompressedFormat(format);
        cooked_info.width = cooked_info.height = compressed ? 4 : 2;
        cooked_info.channel = format == rdesc::ETexturePixelFormat::R8_UNORM ? 1 : 2;
        cooked_info.flags = compressed ? rdesc::toUnderlying(rdesc::ETextureAssetFlags::COMPRESSED) : 0;
        const auto bytes = compressed ? 16U : 4U * cooked_info.channel;
        cooked_info.mip_ranges[0] =
            {0, bytes, static_cast<std::uint32_t>(cooked_info.width), static_cast<std::uint32_t>(cooked_info.height)};
        std::vector<std::byte> data(bytes);
        const asset::AssetId id{ids(std::to_string(static_cast<unsigned>(format)))};
        auto value = std::make_shared<rdesc::Texture>(take(rdesc::Texture::copyOf(cooked_info, data)));
        auto cooked = take(asset::TextureAsset::create({id, asset::TextureAsset::asset_type}, value));
        auto format_provider = std::make_shared<CookedAssets>();
        format_provider->images[id] =
            take(asset::TAssetSerDeser<asset::TextureAsset>::encode(*cooked, asset::AssetEncodeLimits{1 << 20}));
        asset::AssetVfs format_vfs;
        auto format_mount = take(format_vfs.mount({"/assets", format_provider}));
        const RenderAssetVersion key{id, 1, 9};
        CHECK(assets->acquireTexture(key, endpoint->port(), format_vfs.view().capture()));
        while (!assets->texture(key))
        {
            drive(true);
            CHECK(assets->failure(key).type == 0 && std::chrono::steady_clock::now() < deadline);
        }
        CHECK(assets->texture(key)->description.format == rdesc::textureImageFormat(format));
        assets->releaseTexture(key);
    }
    while (assets->mesh({mesh_id, previous_asset_revision}) != nullptr)
    {
        drive(true);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    const auto compiled_before_removal = rendering->stats().compiled_graphs;
    registry.destroy(first);
    for (unsigned i = 0; i < 8; ++i)
    {
        drive(true);
    }
    CHECK(rendering->stats().compiled_graphs == compiled_before_removal);
    CHECK(scene->removeCameraTarget(editor_camera, second_view));
    while (!scene->cameraTargetRetired(editor_camera, second_view))
    {
        drive(true);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    bool second_target_released = false;
    auto second_removal = take(execution.submit(
        {},
        [&](execution::TaskReporter) noexcept
        {
            return stdexec::then(
                rendering->mainClient().request(remove_target, editor_view.target),
                [&](AckResponse response) noexcept
                {
                    CHECK(response.error.type == 0);
                    second_target_released = true;
                }
            );
        }
    ));
    while (!second_target_released)
    {
        drive(false);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    project_tasks.requestStop(); // Scene cleanup no longer depends on fresh monitored-task admission.
    scene_stop.request_stop();
    while (!scene_retired)
    {
        drive(false);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    scene.reset();
    while (!project_tasks.settled() || !frames.settled())
    {
        drive(false);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    assets->releaseTexture({texture_id, 1});
    assets->close();
    while (!assets->settled() || !frames.settled())
    {
        drive(false);
        CHECK(std::chrono::steady_clock::now() < deadline);
    }
    assets.reset();
    CHECK(rendering->shutdown());
    const auto statistics = rendering->stats();
    CHECK(statistics.submissions != 0 && statistics.validation_errors == 0);
    rendering.reset();
    endpoint.reset();
    CHECK(reads.join());
    execution.requestStop();
    CHECK(execution.join());
    std::printf(
        "Engine scene: %llu frames, %llu submissions, validation errors=%llu warnings=%llu\n",
        static_cast<unsigned long long>(completed_frames),
        static_cast<unsigned long long>(statistics.submissions),
        static_cast<unsigned long long>(statistics.validation_errors),
        static_cast<unsigned long long>(statistics.validation_warnings)
    );
}
