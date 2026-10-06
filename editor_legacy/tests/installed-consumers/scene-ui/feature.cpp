#include "../common/RenderRegistration.hpp"
#include <lux/engine/render/RenderRuntime.hpp>
#include "../common/UiDrawPane.hpp"
#include <lux/engine/ui/Theme.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <imgui.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    using namespace lux;
    using namespace std::chrono_literals;
    auto frame = std::make_shared<ui::RenderFrame>();
    std::vector<std::byte> configuration;
    {
        auto messages_created = object::ObjectMessageQueue::create(64);
        assert(messages_created);
        auto messages = std::move(*messages_created);
        auto context = ui::Root::create(messages.dispatcherRef(), {.docking = false});
        assert(context);
        auto encoded = ui::makeRenderConfiguration(**context);
        assert(encoded);
        assert(render::UiRenderConfigurationCodec().materialize_attach(*encoded, configuration));
        TUiDrawPane pane(**context, [&] {
        ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, {128, 96}, IM_COL32(255, 0, 0, 255));
        });
        assert((*context)->update({{128, 96}, 1.0F / 60.0F}, &frame->draw_data));
        frame->sequence = 1;
    }
    assert(ImGui::GetCurrentContext() == nullptr);
    render::RendererConfig config;
    config.validation = true;
    std::vector<lux::render::RenderFeatureRegistration> initial_features = {render::kUiRenderRenderFeatureRegistration};
    auto made = render::RenderRuntime::create(std::move(config));
    assert(made);
    registerRenderFeatures(**made, std::move(initial_features));
    auto runtime = std::move(*made);
    const auto until = [&](auto condition) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (!condition())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::size_t controls = 4, programs = 1;
            assert(runtime->collectCompletions(16));
        assert(runtime->submitPending(controls, programs));
            std::this_thread::sleep_for(1ms);
        }
    };
    auto scene_request = runtime->control()->get().createScene({.name = "Installed UI feature"});
    until([&] { return scene_request.isReady(); });
    assert(scene_request.tryResult() && scene_request.tryResult()->get().error.ok());
    const auto scene = scene_request.tryResult()->get().scene_id;
    assert(scene.isValid());
    auto scene_attach = runtime->control()->get().addFeatureRaw(scene, runtime->features().typeId("UiRender"), configuration);
    until([&] { return scene_attach.isReady(); });
    assert(scene_attach.tryResult() && scene_attach.tryResult()->get().error.ok());
    const auto scene_feature = scene_attach.tryResult()->get().feature;
    auto view_request = runtime->control()->get().addView(scene, {128, 96}, "Installed UI");
    auto target_request = runtime->control()->get().createOffscreenRenderTarget({128, 96}, render::kTargetFlagSampled);
    until([&] { return view_request.isReady() && target_request.isReady(); });
    assert(view_request.tryResult() && view_request.tryResult()->get().error.ok());
    assert(target_request.tryResult() && target_request.tryResult()->get().status == 0);
    const auto view = view_request.tryResult()->get().view;
    const auto target = target_request.tryResult()->get().target;
    auto attached = runtime->control()->get().setLayer(target, 0, scene, view);
    until([&] { return attached.isReady(); });
    assert(attached.tryResult() && attached.tryResult()->get().error.ok());
    render::TRenderProgram<> program;
    render::RenderProgramSession::Builder builder(program);
    for (unsigned index = 1; index <= 3; ++index)
    {
        builder.begin({});
        program.kind = render::ERenderProgramKind::FRAME;
        assert(ui::appendFrame(builder, runtime->features().ops<render::UiRenderOperationIds>("UiRender"),
            scene, scene_feature, frame));
        until([&] {
            const auto result = runtime->submit(program);
            assert(result);
            return *result == render::EFrameSubmit::SUBMITTED;
        });
        until([&] { return runtime->statistics().frames >= index; });
    }
    frame.reset();
    runtime->control()->get().removeLayer(target, 0);
    auto view_removed = runtime->control()->get().removeView(scene, view);
    until([&] { return view_removed.isReady(); });
    assert(view_removed.tryResult() && view_removed.tryResult()->get().error.ok());
    auto target_removed = runtime->control()->get().destroyRenderTarget(target);
    until([&] { return target_removed.isReady(); });
    assert(target_removed.tryResult() && target_removed.tryResult()->get().status <= 1);
    auto scene_destroy = runtime->control()->get().destroyScene(scene);
    until([&] { return scene_destroy.isReady(); });
    assert(scene_destroy.tryResult() && scene_destroy.tryResult()->get().error.ok());
    assert(runtime->statistics().validation_errors == 0);
    assert(runtime->statistics().gpu_completed > 0);
    assert(runtime->beginClose());
    until([&] {
        std::size_t replies = 16, controls = 4, programs = 1;
        auto closed = runtime->advanceClose(replies, controls, programs);
        assert(closed);
        return *closed == render::ERenderClose::COMPLETE;
    });
    assert(runtime->joinStopped());
    std::cout << "PASS installed RenderFeature: three GPU frames, CPU Context exited before GPU creation, "
                 "all resources retired; no Editor/Scene includes and no pixel readback claim\n";
}
