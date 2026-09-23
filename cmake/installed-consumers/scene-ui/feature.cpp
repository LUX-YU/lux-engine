#include "../common/RenderRegistration.hpp"
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <lux/engine/ui/Theme.hpp>
#include <lux/engine/ui/rendering/UiRenderFeature.hpp>
#include <imgui.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    using namespace lux;
    using namespace std::chrono_literals;
    auto frame = std::make_shared<ui::UiRenderFrame>();
    std::vector<std::byte> configuration;
    {
        auto context = ui::Context::create({true});
        assert(context);
        auto encoded = ui::makeUiRenderConfiguration(*context);
        assert(encoded);
        configuration = std::move(*encoded);
        ui::Frame cpu(*context, ui::Theme::luxDark(), {{128, 96}, 1.F / 60.F});
        ImGui::GetBackgroundDrawList()->AddRectFilled({0, 0}, {128, 96}, IM_COL32(255, 0, 0, 255));
        cpu.finish();
        assert(context->capture(frame->snapshot));
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
            assert(runtime->poll(16, controls, programs));
            std::this_thread::sleep_for(1ms);
        }
    };
    auto scene = runtime->createScene({.name = "Installed UI feature"},
        {{render::kUiRenderDescriptor.type, runtime->features().typeId("UiRender"), std::move(configuration), {}}});
    assert(scene);
    until([&] { return scene->status().state == render::ESceneResourceState::READY; });
    auto view = runtime->openView(*scene, {.extent = {128, 96}});
    assert(view);
    until([&] { return (*view)->status().state == render::EViewState::READY; });
    render::RenderProgram<> program;
    render::RenderProgramSession::Builder builder(program);
    for (unsigned index = 1; index <= 3; ++index)
    {
        builder.begin({});
        program.kind = render::ERenderProgramKind::Frame;
        assert(ui::appendUiFrame(builder, runtime->features().ops<render::UiRenderOperationIds>("UiRender"),
            *scene, scene->feature(render::kUiRenderDescriptor.type), frame));
        until([&] {
            const auto result = runtime->submit(program);
            assert(result);
            return *result == render::EFrameSubmit::SUBMITTED;
        });
        until([&] { return runtime->statistics().frames >= index; });
    }
    const auto receipt = scene->receipt();
    frame.reset();
    view->reset();
    *scene = {};
    until([&] { return receipt.status().state == render::ESceneResourceState::RETIRED; });
    assert(receipt.status().failure.ok());
    assert(runtime->statistics().runtime_leases == 0 && runtime->statistics().validation_errors == 0);
    assert(runtime->statistics().gpu_completed > 0);
    assert(runtime->beginClose());
    until([&] {
        std::size_t replies = 16, controls = 4, programs = 1;
        auto closed = runtime->advanceClose(replies, controls, programs);
        assert(closed);
        return *closed == render::ERenderClose::COMPLETE;
    });
    assert(runtime->joinStopped());
    std::cout << "PASS installed UiRenderFeature: three GPU frames, CPU Context exited before GPU creation, "
                 "all resources retired; no Editor/Scene includes and no pixel readback claim\n";
}
