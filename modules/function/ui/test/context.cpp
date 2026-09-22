#include <lux/engine/ui/Context.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <lux/engine/ui/Theme.hpp>

#include <cassert>
#include <imgui.h>
#include <iostream>

int main()
{
    using namespace lux::ui;
    auto *original = ImGui::CreateContext();
    auto first = Context::create({true});
    auto second = Context::create();
    assert(first && second && ImGui::GetCurrentContext() == original);
    const auto theme = Theme::luxDark();
    UiFrameSnapshot slot;

    {
        Frame frame(*first, theme, {{640, 480}, 1.0F / 60.0F, {1, 1}});
        assert(first->frameOpen() && frame.uses(*first));
        assert(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable);
        assert(ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset);
        frame.text("Discarded CPU frame");
        const auto open = first->capture(slot);
        assert(!open && open.error() == EUiCaptureError::FRAME_OPEN);
    }
    assert(!first->frameOpen() && ImGui::GetCurrentContext() == original);
    auto discarded = first->capture(slot);
    assert(!discarded && discarded.error() == EUiCaptureError::NO_FRAME);

    for (int index = 0; index < 100; ++index)
    {
        auto &cpu = index % 2 ? *second : *first;
        Frame frame(cpu, theme, {{640, 480}, 1.0F / 60.0F, {1, 1}});
        frame.text("Independent CPU context");
        frame.finish();
        assert(ImGui::GetCurrentContext() == original);
        assert(cpu.capture(slot) && slot.valid());
        auto duplicate = cpu.capture(slot);
        assert(!duplicate && duplicate.error() == EUiCaptureError::NO_FRAME);
        assert(slot.valid());
    }
    UiFontSource invalid;
    auto rejected = Context::create({}, &invalid);
    assert(!rejected && rejected.error() == EUiInitError::INVALID_FONT_DATA);
    assert(ImGui::GetCurrentContext() == original);
    ImGui::DestroyContext(original);
    std::cout << "PASS CPU Context, explicit finish, discarded frame, reusable capture and context restoration\n";
}
