#pragma once

namespace
{
    // Feed the real UI session, draw the actual SceneViewport, and observe camera effects by GPU readback.
    // This is an input-routing regression, not Windows mouse-capture or native IME acceptance.
    bool exerciseViewportInput(
        rendering::EditorRenderer& renderer, ui::EditorWindow& window, lux::process::ExecutionRuntime& execution,
        sessions::SceneSession& session, sessions::SceneView& view, ui::SceneWorkspace& workspace,
        lux::scene::RenderRuntimeLease& runtime, std::uint64_t& cycle, const std::filesystem::path& output)
    {
        const auto deadline = Clock::now() + std::chrono::seconds{45};
        auto& input = window.uiSession();
        const auto selection = session.selection().current;
        const auto history = session.historyId();
        const auto history_before = session.historyView();
        require(history_before, "viewport input reads original history state");
        unsigned checks{}, failures{};
        const auto step = [&] {
            require(Clock::now() < deadline && execution.drainMain(64) && renderer.poll(64),
                    "viewport input finite owner progress");
            const sessions::SceneOwnerUpdate update{++cycle, 1.0 / 60.0};
            require(session.updateAtOwnerSafePoint(update) && workspace.updateBeforeFrame(),
                    "viewport input updates the owning Session/View");
            require(window.beginFrame({{1600, 900}, 1.0F / 60, {1, 1}}) && window.drawPanes() &&
                        workspace.afterDraw(1.0 / 60.0, {1, 1}), "viewport input real Pane draw and routing");
            auto snapshot = window.finishFrame();
            require(snapshot, "viewport input owning UI snapshot");
            auto packet = renderer.sealFrame(*snapshot, workspace.frameImages());
            require(packet && !snapshot->valid(), "viewport input seals actual images");
            workspace.releaseFrameImages();
            while (packet->valid())
            {
                require(Clock::now() < deadline && renderer.trySubmitFrame(*packet) && renderer.poll(64),
                        "viewport input retries the same backpressured packet");
                std::this_thread::yield();
            }
            require(session.advanceScene(update), "viewport input advances exactly once");
        };
        const auto move = [&](float x, float y) { input.feedInput(lux::ui::UiPointerMove{{x, y}}); step(); };
        const auto button = [&](lux::ui::EPointerButton key, bool down) {
            input.feedInput(lux::ui::UiPointerButton{key, down}); step();
        };
        const auto key = [&](lux::ui::EKey value, bool down) {
            input.feedInput(lux::ui::UiKey{value, down}); step();
        };
        const auto capture = [&](const char* name) {
            for (unsigned i = 0; i != 12; ++i) step();
            auto image = view.image();
            require(image, "viewport input retains current image for readback");
            Readback result;
            result.start(runtime, *image);
            while (!result.request.isReady()) step();
            return result.finish(output / name);
        };
        const auto check = [&](bool passed, const char* name) {
            ++checks;
            failures += !passed;
            std::printf("viewport input %s result=%u\n", name, unsigned(passed));
        };

        input.feedInput(lux::ui::UiWindowFocus{true});
        require(workspace.activate() && view.resetCamera(), "viewport input starts from the owning default camera");
        move(1200, 350);
        const auto initial = capture("input-initial.ppm");
        button(lux::ui::EPointerButton::RIGHT, true);
        move(1210, 354);
        const auto looking = capture("input-looking.ppm");
        check(looking != initial, "RMB moves camera");
        // Drag across the boundary into the Inspector while still held.
        move(1270, 358);
        const auto outside = capture("input-outside.ppm");
        check(outside != looking, "captured drag survives Pane boundary");
        button(lux::ui::EPointerButton::RIGHT, false);
        move(1200, 350);
        check(capture("input-released.ppm") == outside, "release cancels capture");

        button(lux::ui::EPointerButton::RIGHT, true);
        key(lux::ui::EKey::ESCAPE, true);
        key(lux::ui::EKey::ESCAPE, false);
        move(1210, 354);
        check(capture("input-escape.ppm") == outside, "Escape cancels held capture");
        button(lux::ui::EPointerButton::RIGHT, false);

        button(lux::ui::EPointerButton::MIDDLE, true);
        move(1220, 364);
        const auto panning = capture("input-panning.ppm");
        check(panning != outside, "MMB pans camera");
        input.feedInput(lux::ui::UiWindowFocus{false}); step();
        button(lux::ui::EPointerButton::MIDDLE, false);
        input.feedInput(lux::ui::UiWindowFocus{true}); step();
        move(1230, 374);
        check(capture("input-focus-restored.ppm") == panning, "focus loss cancels capture before return");

        // The actual Outliner Filter owns keyboard input; Home/F/W must not move the Scene camera.
        move(70, 70);
        button(lux::ui::EPointerButton::LEFT, true);
        button(lux::ui::EPointerButton::LEFT, false);
        input.feedInput(lux::ui::UiText{U'a'}); step();
        check(input.inputSnapshot().keyboard_blocked, "real text field owns keyboard");
        const auto before_text = capture("input-text-before.ppm");
        for (const auto value : {lux::ui::EKey::HOME, lux::ui::EKey::F, lux::ui::EKey::W})
        {
            key(value, true); key(value, false);
        }
        check(capture("input-text-after.ppm") == before_text, "text input blocks camera shortcuts");
        key(lux::ui::EKey::ESCAPE, true); key(lux::ui::EKey::ESCAPE, false);
        const auto history_after = session.historyView();
        check(session.selection().current == selection && session.historyId() == history && history_after &&
                  history_after->history.current == history_before->history.current &&
                  history_after->history.revision == history_before->history.revision,
              "camera and local text retain Scene selection/history state");
        std::printf("viewport input checks=%u failures=%u; UiInputEvent route, not native desktop evidence\n",
                    checks, failures);
        return failures == 0;
    }
}
