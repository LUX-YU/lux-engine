#include "../../../../../cmake/installed-consumers/common/RenderRegistration.hpp"
#include <lux/engine/function/render/features/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/render/detail/ViewImageLifetime.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <lux/engine/ui/Theme.hpp>
#include <lux/engine/ui/rendering/UiRenderFeature.hpp>

#include <cassert>
#include <chrono>
#include <imgui.h>
#include <iostream>
#include <thread>
#include <vulkan/vulkan.h>

int main(int argc, char **)
{
    using namespace lux;
    using namespace std::chrono_literals;
    const bool camera_gate = argc > 1;
    std::vector<std::byte> configuration;
    auto captured = std::make_shared<ui::UiRenderFrame>();
    {
        auto context = ui::Context::create({true});
        assert(context);
        auto config = ui::makeUiRenderConfiguration(*context);
        assert(config);
        configuration = std::move(*config);
        ui::Frame frame(*context, ui::Theme::luxDark(), {{128, 96}, 1.0F / 60.0F});
        ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, {128, 96}, IM_COL32(255, 0, 0, 255));
        frame.finish();
        assert(context->capture(captured->snapshot));
        captured->sequence = 1;
    }
    assert(ImGui::GetCurrentContext() == nullptr);

    render::RendererConfig config;
    config.validation = true;
    std::vector<render::RenderFeatureRegistration> initial_features = {render::kUiRenderRenderFeatureRegistration};
    if (camera_gate)
    {
        initial_features.push_back(render::kViewCameraRenderFeatureRegistration);
    }
    auto diagnostics = [](auto severity, auto message) {
        if (severity == 2)
        {
            std::cerr << message << '\n';
        }
    };
    auto created = render::RenderRuntime::create(std::move(config), std::move(diagnostics));
    assert(created);
    registerRenderFeatures(**created, std::move(initial_features));
    auto runtime = std::move(*created);
    const auto pump = [&] {
        std::size_t controls = 4, programs = 1;
        assert(runtime->poll(16, controls, programs));
    };
    const auto until = [&](auto condition) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (condition())
            {
                return;
            }
            pump();
            std::this_thread::sleep_for(1ms);
        }
        assert(false && "The requested completion did not arrive");
    };
    // Admit the consumer first: target slot order is deliberately the reverse
    // of its later producer dependency.
    auto consumer = runtime->createScene(
        {.name = "UI sampling Scene output"},
        {{render::kUiRenderDescriptor.type, runtime->features().typeId("UiRender"), configuration, {}}});
    assert(consumer);
    until([&] { return consumer->status().state == render::ESceneResourceState::READY; });
    auto consumer_view = runtime->openView(*consumer, {.extent = {128, 96}});
    assert(consumer_view);
    until([&] { return (*consumer_view)->status().state == render::EViewState::READY; });

    auto scene = runtime->createScene(
        {.name = "UI only, no camera", .lit_color_format = rdesc::ETextureFormat::RGBA8_UNORM},
        {{render::kUiRenderDescriptor.type, runtime->features().typeId("UiRender"), std::move(configuration), {}}});
    assert(scene);
    until([&] { return scene->status().state == render::ESceneResourceState::READY || !scene->status().failure.ok(); });
    assert(scene->status().failure.ok());
    auto view = runtime->openView(*scene, {.extent = {128, 96}});
    assert(view);
    until([&] { return (*view)->status().state == render::EViewState::READY; });
    auto image = (*view)->acquireImage();
    assert(image);
    const auto target = render::detail::ViewImageAccess::record(*image)->version->target;
    render::RenderProgram<> program;
    render::RenderProgramSession::Builder builder(program);
    std::size_t retries{};
    const auto submit = [&] {
        until([&] {
            const auto accepted = runtime->submit(program);
            assert(accepted);
            if (*accepted == render::EFrameSubmit::BACKPRESSURED)
            {
                ++retries;
                assert(!program.attachments.empty());
            }
            return *accepted == render::EFrameSubmit::SUBMITTED;
        });
    };
    builder.begin({});
    program.kind = render::ERenderProgramKind::Frame;
    assert(ui::appendUiFrame(builder, runtime->features().ops<render::UiRenderOperationIds>("UiRender"), *scene,
                             scene->feature(render::kUiRenderDescriptor.type), captured));
    submit();
    until([&] { return runtime->statistics().frames >= 1; });
    // The existing readback contract selects FIF slot zero. Fill every slot
    // with identical content before reading; one submitted frame is insufficient.
    for (unsigned frame = 2; frame <= 3; ++frame)
    {
        builder.begin({});
        program.kind = render::ERenderProgramKind::Frame;
        assert(ui::appendUiFrame(builder, runtime->features().ops<render::UiRenderOperationIds>("UiRender"), *scene,
                                 scene->feature(render::kUiRenderDescriptor.type), captured));
        submit();
        until([&] { return runtime->statistics().frames >= frame; });
    }

    auto control = runtime->control();
    assert(control);
    std::vector<std::byte> pixels(128 * 96 * 4);
    auto readback = control->get().readbackTarget(target, pixels.data(), pixels.size());
    until([&] { return readback.isReady(); });
    const auto result = readback.tryResult();
    assert(result && result->get().status == 0);
    const auto pixel = (48 * 128 + 64) * 4;
    std::cout << "pixel=" << static_cast<unsigned>(pixels[pixel]) << ',' << static_cast<unsigned>(pixels[pixel + 1])
              << ',' << static_cast<unsigned>(pixels[pixel + 2]) << '\n';
    const auto format = static_cast<VkFormat>(result->get().format);
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    assert(static_cast<unsigned>(pixels[pixel + (bgra ? 2 : 0)]) == 255);
    assert(static_cast<unsigned>(pixels[pixel + 1]) == 0);
    assert(static_cast<unsigned>(pixels[pixel + (bgra ? 0 : 2)]) == 0);

    std::vector<char> graph(65536);
    auto dump = control->get().dumpRenderGraph(scene->id(), graph.data(), graph.size());
    until([&] { return dump.isReady(); });
    assert(dump.tryResult());
    assert(std::string_view(graph.data()).find("UiRender") != std::string_view::npos);

    auto sampled = std::make_shared<ui::UiRenderFrame>();
    {
        auto context = ui::Context::create();
        assert(context);
        ui::Frame frame(*context, ui::Theme::luxDark(), {{128, 96}, 1.0F / 60.0F});
        ImGui::GetBackgroundDrawList()->AddImage(image->texture, {0, 0}, {128, 96});
        frame.finish();
        assert(context->capture(sampled->snapshot));
        sampled->images.push_back(*image);
        sampled->sequence = 2;
    }
    builder.begin({});
    const auto cycle = ui::appendUiFrame(builder, runtime->features().ops<render::UiRenderOperationIds>("UiRender"),
                                         *scene, scene->feature(render::kUiRenderDescriptor.type), sampled);
    assert(!cycle && cycle.error().type == render::renderError<render::err::graph::DependencyCycle>().type);
    assert(program.commands.empty() && program.attachments.empty() && sampled->images.size() == 1);

    for (unsigned frame = 4; frame <= 6; ++frame)
    {
        builder.begin({});
        program.kind = render::ERenderProgramKind::Frame;
        assert(ui::appendUiFrame(builder, runtime->features().ops<render::UiRenderOperationIds>("UiRender"), *consumer,
                                 consumer->feature(render::kUiRenderDescriptor.type), sampled));
        submit();
        until([&] { return runtime->statistics().frames >= frame; });
    }
    auto consumer_image = (*consumer_view)->acquireImage();
    assert(consumer_image);
    const auto consumer_target = render::detail::ViewImageAccess::record(*consumer_image)->version->target;
    auto sampled_readback = control->get().readbackTarget(consumer_target, pixels.data(), pixels.size());
    until([&] { return sampled_readback.isReady(); });
    assert(sampled_readback.tryResult() && sampled_readback.tryResult()->get().status == 0);
    assert(static_cast<unsigned>(pixels[pixel + 2]) == 255 && static_cast<unsigned>(pixels[pixel + 1]) == 0);
    const auto evidence = runtime->imageEvidence(*image);
    assert(evidence && evidence->frame_serial != 0);

    // A -> B -> A is rejected before opening a GPU frame, while the backend
    // remains usable. This tests the generic target graph, not only the UI guard.
    auto feedback = std::make_shared<ui::UiRenderFrame>();
    {
        auto context = ui::Context::create();
        assert(context);
        ui::Frame frame(*context, ui::Theme::luxDark(), {{128, 96}, 1.0F / 60.0F});
        ImGui::GetBackgroundDrawList()->AddImage(consumer_image->texture, {0, 0}, {128, 96});
        frame.finish();
        assert(context->capture(feedback->snapshot));
        feedback->images.push_back(*consumer_image);
        feedback->sequence = 3;
    }
    const auto prior_frames = runtime->statistics().frames;
    builder.begin({});
    program.kind = render::ERenderProgramKind::Frame;
    assert(ui::appendUiFrame(builder, runtime->features().ops<render::UiRenderOperationIds>("UiRender"), *scene,
                             scene->feature(render::kUiRenderDescriptor.type), feedback));
    submit();
    bool rejected{};
    until([&] {
        auto diagnostic = runtime->takeDiagnostic();
        assert(diagnostic);
        rejected = rejected || (*diagnostic && (*diagnostic)->failure.render_error.type ==
                                                   render::renderError<render::err::graph::DependencyCycle>().type);
        return rejected;
    });
    assert(runtime->statistics().frames == prior_frames);
    assert(runtime->status().state == render::ERenderRuntimeState::ACTIVE);
    builder.begin({});
    program.kind = render::ERenderProgramKind::Frame;
    assert(ui::appendUiFrame(builder, runtime->features().ops<render::UiRenderOperationIds>("UiRender"), *scene,
                             scene->feature(render::kUiRenderDescriptor.type), captured));
    submit();
    until([&] { return runtime->statistics().frames > prior_frames; });
    feedback.reset();

    if (camera_gate)
    {
        std::cerr << "camera gate: attach\n";
        auto camera = control->get().addFeature(scene->id(), runtime->features().typeId("StandardViewCamera"),
                                                render::ViewCameraCommTag{});
        until([&] { return camera.isReady(); });
        assert(camera.tryResult() && camera.tryResult()->get().error.ok());
        const auto camera_ops = runtime->features().ops<render::ViewCameraOperationIds>("StandardViewCamera");
        const auto frames = [&](bool expected_red) {
            std::cerr << "camera gate: frames red=" << expected_red << '\n';
            for (unsigned index{}; index != 3; ++index)
            {
                const auto next_frame = runtime->statistics().frames + 1;
                builder.begin({});
                program.kind = render::ERenderProgramKind::Frame;
                assert(ui::appendUiFrame(builder, runtime->features().ops<render::UiRenderOperationIds>("UiRender"),
                                         *scene, scene->feature(render::kUiRenderDescriptor.type), captured));
                submit();
                until([&] { return runtime->statistics().frames >= next_frame; });
            }
            auto read = control->get().readbackTarget(target, pixels.data(), pixels.size());
            until([&] { return read.isReady(); });
            assert(read.tryResult() && read.tryResult()->get().status == 0);
            assert(static_cast<unsigned>(pixels[pixel + (bgra ? 2 : 0)]) == (expected_red ? 255U : 0U));
            assert(static_cast<unsigned>(pixels[pixel + 1]) == 0);
            assert(static_cast<unsigned>(pixels[pixel + (bgra ? 0 : 2)]) == 0);
        };
        frames(false); // Existing red output is actually cleared, not merely left untouched.
        builder.begin({});
        program.kind = render::ERenderProgramKind::StateUpdate;
        auto update =
            builder.appendBulk<render::ViewCameraUpdatePayload>(camera_ops.id<render::ViewCameraUpdateOp>(), 1);
        update[0].scene_id = scene->id();
        update[0].view = (*view)->handle();
        update[0].coordinate_page_size = 1024;
        for (unsigned i{}; i != 4; ++i)
        {
            update[0].view_matrix[i * 5] = update[0].proj_matrix[i * 5] = 1;
        }
        submit();
        frames(true);
        std::cerr << "camera gate: remove association\n";
        builder.begin({});
        program.kind = render::ERenderProgramKind::StateUpdate;
        auto removed =
            builder.appendBulk<render::ViewCameraRemovePayload>(camera_ops.id<render::ViewCameraRemoveOp>(), 1);
        removed[0] = {scene->id(), (*view)->handle()};
        submit();
        frames(false);
        std::cerr << "camera gate: detach\n";
        auto detach = control->get().removeFeature(scene->id(), camera.tryResult()->get().feature);
        until([&] { return detach.isReady(); });
        assert(detach.tryResult() && detach.tryResult()->get().error.ok());
        frames(true); // CPU/GPU UI itself has no Camera requirement.
        std::cout
            << "PASS camera capability: absent/valid/removed output pixel results, UI remains camera-independent\n";
    }

    // Resize cannot revoke an image already used by a captured UI frame.
    assert((*view)->requestExtent({144, 100}));
    for (unsigned i = 0; i < 8; ++i)
    {
        pump();
    }
    assert((*view)->status().ready_extent == (render::PixelExtent{128, 96}));
    const auto before_clear = runtime->statistics().frames;
    builder.begin({});
    program.kind = render::ERenderProgramKind::StateUpdate;
    assert(ui::appendUiClear(builder, runtime->features().ops<render::UiRenderOperationIds>("UiRender"), *consumer,
                             consumer->feature(render::kUiRenderDescriptor.type)));
    submit();
    sampled.reset();
    *image = {};
    until([&] {
        return (*view)->status().state == render::EViewState::READY &&
               (*view)->status().ready_extent == (render::PixelExtent{144, 100});
    });
    assert(runtime->statistics().frames == before_clear);
    assert(consumer->status().state == render::ESceneResourceState::READY);
    assert((*consumer_view)->status().state == render::EViewState::READY);
    std::cout << "PASS UI clear: sampled View released without another draw or destroying the UI Scene; GPU guards "
                 "retained\n";

    sampled.reset();
    *consumer_image = {};
    consumer_view->reset();
    auto consumer_receipt = consumer->receipt();
    *consumer = {};
    until([&] { return consumer_receipt.status().state == render::ESceneResourceState::RETIRED; });
    captured.reset();
    *image = {};
    view->reset();
    auto receipt = scene->receipt();
    *scene = {};
    until([&] { return receipt.status().state == render::ESceneResourceState::RETIRED; });
    assert(receipt.status().failure.ok());
    assert(runtime->statistics().validation_errors == 0);
    assert(runtime->beginClose());
    until([&] {
        std::size_t replies = 16, controls = 4, programs = 1;
        const auto result = runtime->advanceClose(replies, controls, programs);
        assert(result);
        return *result == render::ERenderClose::COMPLETE;
    });
    assert(runtime->joinStopped());
    std::cout << "PASS registered UI Feature, graph pass, GPU pixel readback, reverse target dependency, "
                 "cycle refusal/recovery, resize waits for old CPU/GPU use, CPU Context destroyed before GPU creation, "
                 "retirement\n";
}
