#include "RenderTest.hpp"
#include "SceneCapture.hpp"
#include "support/RenderRoleInputs.hpp"
#include <lux/engine/systems/render/ImguiFrameCapture.hpp>
#include <lux/engine/render/capabilities/ImguiCapability.hpp>
#include <lux/engine/render/capabilities/UiScene.hpp>
#include <lux/engine/render/commands/ImageCommands.hpp>
#include <lux/engine/render/extensions/GraphCommands.hpp>
#include <lux/engine/render/features/UiRasterFeature.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>

using namespace lux;
using namespace lux::render;
using namespace lux::engine::render;
using lux::engine::render::captureImguiFrame;

struct CopyPixelsCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<>;
    using PerFrameData = std::tuple<>;
};

struct CopyPixelsImplementation final
{
    using Capability = CopyPixelsCapability;
    using Features = std::tuple<CaptureFeature>;
    using Config = CaptureConfig;
    using Storages = std::tuple<CaptureBuffer>;
    using Bindings = std::tuple<>;
    using FrameBindings = std::tuple<>;

    static CapabilityImplementation implementation() noexcept
    {
        CapabilityImplementation result;
        result.implementation = "test.ui.copy_pixels";
        result.graph.ports = {
            {"color",
             ECapabilityPortDirection::INPUT,
             "lux.color.display.v1",
             CapabilityImageShape{rdesc::ETextureFormat::RGBA8_UNORM, 1, 1},
             "lux.coordinates.viewport"},
            {"pixels", ECapabilityPortDirection::OUTPUT, "test.pixels", CapabilityBufferShape{1, 1}, "bytes"}
        };
        return result;
    }
};

int main(int argc, char**)
{
    const bool large_vertex_offset = argc > 1;
    const unsigned vertex_base = large_vertex_offset ? 70000 : 0;
    auto execution = integration_test::take(execution::ExecutionRuntime::create({2, 128, {16}}));
    RenderExecutionConfig config;
    config.render.channel_name = "engine.ui.drawing";
    config.render.validation = true;
    config.render.frames_in_flight = 3;
    config.render.uploads.emplace();
    config.device.texture_descriptors = true;
    std::array<FeatureId, 2> graph_features;
    config.feature_registration = &graph_features;
    config.register_features = [](void* destination, RenderServer& server) noexcept -> RenderResult<void>
    {
        auto roles = server.registerCapabilityImplementation<fixture::RoleFeature>();
        if (!roles)
        {
            return cxx::unexpected(roles.error());
        }
        auto capture = server.registerCapabilityImplementation<CopyPixelsImplementation>();
        if (!capture)
        {
            return cxx::unexpected(capture.error());
        }
        auto registration = server.featureId(CaptureFeature::name.name());
        if (!registration)
        {
            return cxx::unexpected(registration.error());
        }
        auto ui = server.featureId("lux.render.feature.UiRasterFeature");
        if (!ui)
        {
            return cxx::unexpected(ui.error());
        }
        *static_cast<std::array<FeatureId, 2>*>(destination) = {*ui, *registration};
        return {};
    };
    auto rendering = integration_test::take(RenderExecution::create(std::move(config), execution));
    auto& client = rendering->mainClient();
    const auto create_effect = [&](CapabilityImplementationId selected, VCapabilityConfiguration configuration)
    {
        CreateEffectRequest request{{64, 64, rdesc::ETextureFormat::RGBA8_UNORM}};
        request.composition.contributions.push_back({"selected", selected, std::move(configuration)});
        const auto ui = integration_test::call<ListCapabilities>(*rendering, {std::string(kUiSceneCapability)});
        request.composition.contributions.push_back(
            {"output",
             ui.implementations.front().implementation_id,
             integration_test::take(ConfigValue::make(UiConfiguration{}))}
        );
        request.composition.outputs.push_back({"output", "color"});
        return integration_test::call<CreateEffect>(*rendering, std::move(request)).effect;
    };
    {
        auto supports = integration_test::call<ListCapabilities>(
            *rendering,
            {std::string(cxx::make_basic_type_info<fixture::RoleCapability>().name())}
        );
        const auto selected = supports.implementations.front().implementation_id;
        const auto role_effect = create_effect(selected, ConfigValue{});
        CHECK(fixture::RoleStorage::live == 0);
        const CommandDestination global_routes{role_effect, 0, GlobalRenderScope{}};
        const auto first_route = integration_test::take(client.commandBinding<fixture::RoleCommand<0>>(global_routes));
        const auto second_route = integration_test::take(client.commandBinding<fixture::RoleCommand<1>>(global_routes));
        CHECK(first_route.command != second_route.command);
        CHECK(
            integration_test::call<fixture::RoleCommand<0>>(*rendering, first_route, {first_route.destination})
                .storage !=
            integration_test::call<fixture::RoleCommand<1>>(*rendering, second_route, {second_route.destination})
                .storage
        );
        const auto scene_id = integration_test::call<CreateScene>(*rendering, {}).scene;
        const auto a = integration_test::call<CreateView>(*rendering, {scene_id}).view;
        const auto b = integration_test::call<CreateView>(*rendering, {scene_id}).view;
        const auto route_a = integration_test::take(
            client.commandBinding<fixture::RoleCommand<2>>({role_effect, 0, ViewScope{scene_id, a}})
        );
        const auto route_b = integration_test::take(
            client.commandBinding<fixture::RoleCommand<2>>({role_effect, 0, ViewScope{scene_id, b}})
        );
        CHECK(route_a.destination != route_b.destination);
        const auto storage_a =
            integration_test::call<fixture::RoleCommand<2>>(*rendering, route_a, {route_a.destination}).storage;
        const auto storage_b =
            integration_test::call<fixture::RoleCommand<2>>(*rendering, route_b, {route_b.destination}).storage;
        CHECK(storage_a != storage_b && fixture::RoleStorage::live == 4);
        integration_test::call<RemoveView>(*rendering, {scene_id, a});
        integration_test::call<RemoveView>(*rendering, {scene_id, b});
        integration_test::call<RemoveScene>(*rendering, {scene_id});
        CHECK(fixture::RoleStorage::live == 2);
    }

    const auto choices =
        integration_test::call<ListCapabilities>(*rendering, {std::string(kUiSceneCapability), "lux.render.scene.ui"});
    CHECK(choices.implementations.size() == 1);
    const auto upload_effect = create_effect(
        choices.implementations.front().implementation_id,
        integration_test::take(ConfigValue::make(UiConfiguration{}))
    );
    const CommandDestination routes{upload_effect, 0, GlobalRenderScope{}};
    const auto storage = integration_test::take(client.commandBinding<ImageCommands::Upload>(routes));
    const auto release_texture = integration_test::take(client.commandBinding<ImageCommands::Release>(routes));
    rdesc::TextureInfo description{};
    description.width = description.height = 1;
    description.channel = 4;
    description.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
    description.color_space = rdesc::ETextureColorSpace::DATA;
    description.mip_ranges[0] = {0, 4, 1, 1};
    const std::array<std::byte, 4> white{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    const std::array<std::byte, 4> green{std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}};
    const auto font = integration_test::call<ImageCommands::Upload>(
                          *rendering,
                          storage,
                          {storage.destination, {description, white}}
    ).texture;
    const auto picture = integration_test::call<ImageCommands::Upload>(
                             *rendering,
                             storage,
                             {storage.destination, {description, green}}
    ).texture;
    const TargetDesc target_description{64, 64, rdesc::ETextureFormat::RGBA8_UNORM};
    const auto scene = integration_test::call<CreateScene>(*rendering, {}).scene;
    const auto view = integration_test::call<CreateView>(*rendering, {scene}).view;
    const auto target = integration_test::call<CreateTarget>(*rendering, {target_description}).target;
    // A later-created producer Target is sampled in this UI scene. SRGB storage is decoded
    // by sampling; the UI output must encode once, not brighten an already encoded viewport.
    const TargetDesc viewport_description{64, 64, rdesc::ETextureFormat::RGBA8_SRGB, 1, ETargetKind::OFFSCREEN, true};
    const auto viewport_scene = integration_test::call<CreateScene>(*rendering, {}).scene;
    const auto viewport_view = integration_test::call<CreateView>(*rendering, {viewport_scene}).view;
    const auto viewport_target = integration_test::call<CreateTarget>(*rendering, {viewport_description}).target;
    UiRasterFeatureConfig viewport{viewport_description};
    viewport.clear = {0, 0.5f, 0, 1};
    GraphEffectRequest viewport_program{viewport_description};
    viewport_program.definition.features.push_back(
        {graph_features[0], "viewport", integration_test::take(ConfigValue::make(viewport))}
    );
    viewport_program.definition.outputs.push_back(
        {{0, "color"}, GraphOutputRequirements{viewport_description.format, 1}}
    );
    const auto viewport_effect =
        integration_test::call<CreateGraphEffect>(*rendering, std::move(viewport_program)).effect;
    integration_test::call<Attach>(*rendering, {viewport_scene, viewport_view, viewport_target, viewport_effect});
    const std::array images{ImguiImage{0, font, {}}, ImguiImage{17, picture, {}}, ImguiImage{19, {}, viewport_target}};
    const auto capture_choices = integration_test::call<ListCapabilities>(
        *rendering,
        {std::string(cxx::make_basic_type_info<CopyPixelsCapability>().name())}
    );
    CHECK(capture_choices.implementations.size() == 1);
    CapturedPixels pixels;
    CreateEffectRequest effect{target_description};
    UiConfiguration ui;
    ui.index_type = sizeof(ImDrawIdx) == 2 ? EIndexType::UINT16 : EIndexType::UINT32;
    effect.composition.contributions.push_back(
        {"ui", choices.implementations.front().implementation_id, integration_test::take(ConfigValue::make(ui))}
    );
    effect.composition.contributions.push_back(
        {"capture",
         capture_choices.implementations.front().implementation_id,
         integration_test::take(ConfigValue::make(CaptureConfig{&pixels}))}
    );
    effect.composition.outputs = {{"ui", "color"}, {"capture", "pixels"}};
    const auto program = integration_test::call<CreateEffect>(*rendering, std::move(effect)).effect;
    AttachmentRequest attachment{scene, view, target, program};
    const auto attached = integration_test::call<Attach>(*rendering, std::move(attachment));
    const auto update = frameDataBinding<ImguiCapability, ImguiFrameData>({program, 0, attached.attachment});
    std::uint64_t viewport_epoch = 1;
    for (unsigned frame_number = 0; frame_number < 9; ++frame_number)
    {
        if (frame_number == 5)
        {
            auto resized = viewport_description;
            resized.width = resized.height = 32;
            viewport_epoch = integration_test::call<ResizeTarget>(*rendering, {viewport_target, resized}).epoch;
        }
        if (frame_number == 5)
        {
            integration_test::call<SetTargetActive>(*rendering, {viewport_target, true, viewport_epoch});
        }
        auto* frame = integration_test::take(client.beginFrame());
        FrameWriter writer{*frame};
        ImDrawList list{nullptr};
        list.VtxBuffer.resize(vertex_base + 12);
        for (unsigned quad = 0; quad < 3; ++quad)
        {
            const auto color = quad == 0 ? 0x800000ffu : 0xffffffffu;
            list.VtxBuffer[vertex_base + quad * 4 + 0] = {{7, 11}, {0, 0}, color};
            list.VtxBuffer[vertex_base + quad * 4 + 1] = {{39, 11}, {1, 0}, color};
            const auto bottom = quad == 0 ? 27.0f : 43.0f;
            list.VtxBuffer[vertex_base + quad * 4 + 2] = {{39, bottom}, {1, 1}, color};
            list.VtxBuffer[vertex_base + quad * 4 + 3] = {{7, bottom}, {0, 1}, color};
        }
        constexpr std::array<ImDrawIdx, 21> triangles{0, 1, 2, 0, 2, 3, 0, 1, 2, 0, 2, 3, 0, 1, 2, 0, 2, 3, 0, 1, 2};
        list.IdxBuffer.resize(static_cast<int>(triangles.size()));
        std::copy(triangles.begin(), triangles.end(), list.IdxBuffer.Data);
        list.CmdBuffer.resize(3);
        const bool invalid_selection = frame_number == 3;
        const std::array<ImVec4, 3> clips{{{15, 19, 31, 35}, {7, 11, 15, 43}, {31, 35, 39, 43}}};
        for (unsigned i = 0; i < 3; ++i)
        {
            auto& command = list.CmdBuffer[i];
            command.ClipRect = clips[i];
            command.TextureId = i == 0 ? (invalid_selection ? 99 : 0) : i == 1 ? 17 : 19;
            command.IdxOffset = i * 6;
            command.ElemCount = 6;
            command.VtxOffset = vertex_base + i * 4;
        }
        ImDrawData source;
        source.Valid = true;
        source.CmdLists.push_back(&list);
        source.CmdListsCount = 1;
        source.TotalVtxCount = list.VtxBuffer.Size;
        source.TotalIdxCount = list.IdxBuffer.Size;
        source.DisplayPos = {7, 11};
        source.DisplaySize = {32, 32};
        source.FramebufferScale = {2, 2};
        CHECK(captureImguiFrame(writer, update, source, images));
        bool completed = false;
        auto task = integration_test::take(execution.submit(
            {},
            [&](execution::TaskReporter) noexcept
            {
                return stdexec::then(
                    client.submitFrame(*frame),
                    [&](FrameCompletion response) noexcept
                    {
                        if (response.error.type != 0)
                        {
                            reportRenderFailure(response.error, "UI frame");
                        }
                        CHECK(response.error.type == (invalid_selection ? kRuntimeIdentity : 0));
                        CHECK(response.requires_resynchronization == invalid_selection);
                        CHECK(client.releaseFrame(*frame));
                        completed = true;
                    }
                );
            }
        ));
        CHECK(execution.waitUntil(
            [&]() noexcept
            {
                static_cast<void>(rendering->consumeResponses());
                return completed;
            }
        ));
    }
    integration_test::call<RemoveScene>(*rendering, {scene});
    integration_test::call<RemoveEffect>(*rendering, {program});
    integration_test::call<RemoveTarget>(*rendering, target);
    integration_test::call<RemoveScene>(*rendering, {viewport_scene});
    integration_test::call<RemoveEffect>(*rendering, {viewport_effect});
    integration_test::call<RemoveTarget>(*rendering, viewport_target);
    integration_test::call<ImageCommands::Release>(*rendering, release_texture, {release_texture.destination, font});
    integration_test::call<ImageCommands::Release>(*rendering, release_texture, {release_texture.destination, picture});
    CHECK(rendering->shutdown());
    CHECK(rendering->stats().validation_errors == 0);
    CHECK(pixels.ready.load() != 0 && pixels.frames.size() == 3);
    for (const auto& frame : pixels.frames)
    {
        const auto channel = [&](unsigned x, unsigned y, unsigned c)
        { return std::to_integer<int>(frame[y * pixels.pitch + x * 4 + c]); };
        CHECK(channel(8, 32, 1) == 255 && channel(8, 32, 0) == 0);
        CHECK(std::abs(channel(32, 24, 0) - 188) <= 2 && channel(32, 24, 1) == 0);
        CHECK(std::abs(channel(56, 56, 1) - 188) <= 2 && channel(56, 56, 0) == 0);
        CHECK(channel(56, 32, 0) == 0 && channel(32, 8, 0) == 0 && channel(32, 40, 0) == 0);
    }
    rendering.reset();
    execution.requestStop();
    CHECK(execution.join());
    std::puts("UI: three FIF slices, clip/offset/base vertex/image selection and linear alpha pixels verified");
}
