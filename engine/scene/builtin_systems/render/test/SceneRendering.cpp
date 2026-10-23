#include "../../../../../modules/function/material/test/MaterialTest.hpp"

#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
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
#include <map>
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

    rdesc::MaterialProgramSet materialProgram()
    {
        using namespace material;
        MaterialGraph graph;
        graph.shading_model = rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
        graph.render_state.double_sided = true;
        graph.param_slots = {{"albedo", EValueType::VEC4, {0.4f, 0.6f, 0.2f, 1}}};
        auto output = material_test::add(graph, MaterialOutputSurface{});
        auto albedo = material_test::add(graph, MaterialParameter{0, EValueType::VEC4});
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
    material->programs = materialProgram();
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
    auto endpoint =
        take(process::asset_loading::VfsAssetReadEndpoint::create(vfs.view(), take(execution.blocking()), reads, {8}));
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

    scene::SceneRenderInputs inputs;
    inputs.description = {64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1};
    inputs.target = call<CreateTarget>(*rendering, {inputs.description}).target;
    scene::SceneRenderServices services{*rendering, *assets, project_tasks, inputs};
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
    registry.emplace<simulation::ecs::Light3D>(light);
    registry.emplace<simulation::ecs::WorldTransform3D>(light, camera_transform);
    auto scene = std::make_unique<scene::RenderSystem>(registry, services);
    std::uint64_t completed_frames = 0;
    const auto drive = [&](bool draw) noexcept
    {
        rendering->consumeResponses();
        CHECK(execution.dispatchMainCompletions());
        CHECK(execution.dispatchTaskEvents());
        assets->maintain();
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

        if (scene)
        {
            scene::SceneStageContext context;
            CHECK(scene->maintain(context));
            CHECK(scene->prepareInputs(context));
            CHECK(scene->publish(context));
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
    while (scene->attachment().isNull() || completed_frames < 12)
    {
        CHECK(std::chrono::steady_clock::now() < deadline);
        drive(true);
    }
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
    const auto previous_target = inputs.target;
    const auto previous_attachment = scene->attachment();
    inputs.target = call<CreateTarget>(*rendering, {inputs.description}).target;
    ++inputs.revision;
    while (scene->attachment() == previous_attachment)
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
    CHECK(assets->mesh({mesh_id, inputs.asset_revision}) != nullptr);
    registry.destroy(first);
    for (unsigned i = 0; i < 8; ++i)
    {
        drive(true);
    }
    scene.reset();
    // Scene destruction admitted its real RemoveScene transaction before project admission closes.
    project_tasks.requestStop();
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
