#include "RenderTest.hpp"
#include "SceneCapture.hpp"
#include <lux/engine/render/capabilities/RasterAssets.hpp>
#include <lux/engine/render/capabilities/UiScene.hpp>
#include <lux/engine/render/extensions/GraphCommands.hpp>
#include <lux/engine/render/features/UiRasterFeature.hpp>
#include <lux/engine/rendering/UiDrawInput.hpp>

using namespace lux;
using namespace lux::render;
using namespace lux::rendering;

int main(int argc, char**)
{
    const bool wide_indices = argc > 1;
    const unsigned vertex_base = wide_indices ? 70000 : 0;
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
        auto registration = server.registerFeature<CaptureFeature>();
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
    const auto choices = integration_test::call<ListCapabilities>(
        *rendering,
        {std::string(kUiSceneCapability), 1, "lux.render.scene.ui"}
    );
    CHECK(choices.implementations.size() == 1);
    const auto installed = integration_test::call<InstallCapabilities>(
        *rendering,
        {choices.implementations.front().selection,
         GlobalRenderScope{},
         integration_test::take(ConfigValue::make(UiConfiguration{}))}
    );
    const auto input = std::ranges::find(installed.inputs, kSampledTextureInput, &CapabilitiesInstalled::Input::schema);
    CHECK(input != installed.inputs.end());
    const auto storage = input->destination;
    rdesc::TextureInfo description{};
    description.width = description.height = 1;
    description.channel = 4;
    description.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
    description.color_space = rdesc::ETextureColorSpace::DATA;
    description.mip_ranges[0] = {0, 4, 1, 1};
    const std::array<std::byte, 4> white{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    const std::array<std::byte, 4> green{std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}};
    const auto font = integration_test::call<CreateTexture>(*rendering, {storage, description, white}).texture;
    const auto picture = integration_test::call<CreateTexture>(*rendering, {storage, description, green}).texture;
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
    viewport.images = {UiTextureImage{font, GraphImageDesc{1, 1}, "ui"}};
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
    GraphImageDesc sampled_viewport{1, 1, 1, 1, viewport_description.format};
    sampled_viewport.extent.source = EImageExtentSource::IMPORTED;
    UiRasterFeatureConfig ui{target_description};
    ui.index_type = wide_indices ? EIndexType::UINT32 : EIndexType::UINT16;
    ui.clear = {0, 0, 0, 1};
    ui.images = {
        UiTextureImage{font, GraphImageDesc{1, 1}, "ui"},
        UiTextureImage{picture, GraphImageDesc{1, 1}, "ui"},
        UiTargetImage{sampled_viewport}
    };
    CapturedPixels pixels;
    GraphEffectRequest effect{target_description};
    effect.definition.features.push_back({graph_features[0], "ui", integration_test::take(ConfigValue::make(ui))});
    effect.definition.features.push_back(
        {graph_features[1], "capture", integration_test::take(ConfigValue::make(CaptureConfig{&pixels}))}
    );
    effect.definition.connections.push_back({{0, "color"}, {1, "color"}});
    effect.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{target_description.format, 1}});
    effect.definition.outputs.push_back({{1, "pixels"}});
    effect.definition.inputs.push_back(
        {{0, uiImagePort(2)},
         {"viewport", sampled_viewport, EGraphOrigin::IMPORTED, {GraphImageRange{}}, EGraphUse::SAMPLED}}
    );
    const auto program = integration_test::call<CreateGraphEffect>(*rendering, std::move(effect)).effect;
    AttachmentRequest attachment{scene, view, target, program};
    attachment.target_images.push_back({"viewport", viewport_target});
    const auto attached = integration_test::call<Attach>(*rendering, std::move(attachment));
    CHECK(attached.draw_lists.size() == 1);
    const auto update = integration_test::take(client.bind<UpdateDrawList>());
    const std::array<UiImageSelection, 3> images{{{{0}, 0}, {{17}, 1}, {{19}, 2}}};
    bool resynchronize = false;
    std::uint64_t viewport_epoch = 1;
    for (unsigned frame_number = 0; frame_number < 9; ++frame_number)
    {
        if (frame_number == 5)
        {
            auto resized = viewport_description;
            resized.width = resized.height = 32;
            viewport_epoch = integration_test::call<ResizeTarget>(*rendering, {viewport_target, resized}).epoch;
        }
        auto* frame = integration_test::take(client.beginFrame());
        frame->resynchronize = resynchronize;
        if (frame_number == 5)
        {
            integration_test::check(bool(client.append(
                *frame,
                integration_test::take(client.bind<SetTargetActive>()),
                SetTargetActive::Request{viewport_target, true, viewport_epoch}
            )));
        }
        auto vertices = frame->allocate<rdesc::UiVertex>(vertex_base + 12);
        std::ranges::fill(vertices, rdesc::UiVertex{});
        for (unsigned quad = 0; quad < 3; ++quad)
        {
            const auto color = quad == 0 ? 0x800000ffu : 0xffffffffu;
            vertices[vertex_base + quad * 4 + 0] = {{7, 11}, {0, 0}, color};
            vertices[vertex_base + quad * 4 + 1] = {{39, 11}, {1, 0}, color};
            const auto bottom = quad == 0 ? 27.0f : 43.0f;
            vertices[vertex_base + quad * 4 + 2] = {{39, bottom}, {1, 1}, color};
            vertices[vertex_base + quad * 4 + 3] = {{7, bottom}, {0, 1}, color};
        }
        // Odd 16-bit index count exercises native four-byte copy-tail padding without a whole-stream copy.
        constexpr std::array<std::uint16_t, 21> triangles{0, 1, 2, 0, 2, 3, 0, 1, 2, 0, 2,
                                                          3, 0, 1, 2, 0, 2, 3, 0, 1, 2};
        std::span<const std::byte> indices;
        if (wide_indices)
        {
            auto values = frame->allocate<std::uint32_t>(triangles.size());
            std::ranges::transform(triangles, values.begin(), [&](auto index) { return vertex_base + index; });
            indices = std::as_bytes(values);
        }
        else
        {
            indices = std::as_bytes(frame->freeze(std::span(triangles)));
        }
        constexpr std::array<rdesc::UiDrawCommand, 3> draws{
            {{0, 6, 0, {15, 19, 31, 35}, {0}}, {6, 6, 4, {7, 11, 15, 43}, {17}}, {12, 6, 8, {31, 35, 39, 43}, {19}}}
        };
        const rdesc::UiDrawData source{vertices, indices, draws, {7, 11}, {32, 32}, {2, 2}, wide_indices ? 4u : 2u};
        auto selections = images;
        const bool invalid_selection = frame_number == 3;
        if (invalid_selection)
        {
            selections[0].index = 3;
        }
        const auto data = integration_test::take(makeUiDrawInput(*frame, source, selections));
        CHECK(client.append(*frame, update, {attached.attachment, attached.draw_lists[0].second, data}));
        frame->group_ends.push_back(frame->commands.size());
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
                        CHECK(response.error.type == (invalid_selection ? kDeviceArgument : 0));
                        CHECK(response.requires_resynchronization == invalid_selection);
                        resynchronize = response.requires_resynchronization;
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
    integration_test::call<ReleaseTexture>(*rendering, font);
    integration_test::call<ReleaseTexture>(*rendering, picture);
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
