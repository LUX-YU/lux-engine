#pragma once
namespace
{
    // Uses the actual property widgets and toolbar. Synthetic UI events are not native desktop evidence.
    void exerciseEditingInput(rendering::EditorRenderer &renderer, ui::EditorWindow &window,
        lux::process::ExecutionRuntime &execution, sessions::SceneSession &session,
        ui::SceneWorkspace &workspace, std::uint64_t &cycle)
    {
        auto &input = window.uiSession();
        const auto selected = session.selection().current;
        require(selected, "editing input selected object");
        const auto outline = session.readOutline();
        require(outline, "editing input outline");
        const auto data = std::find_if((*outline)->rows.begin(), (*outline)->rows.end(),
            [&](const auto &row) { return row.target == *selected; });
        require(data != (*outline)->rows.end() && data->authored, "editing input authored target");
        const auto target = *data->authored;
        const auto original = session.readAuthor(target);
        require(original && original->transform, "editing input transform");
        const auto deadline = Clock::now() + std::chrono::seconds{30};
        const auto step = [&] {
            require(Clock::now() < deadline && execution.drainMain(64) && renderer.poll(64), "editing input progress");
            const sessions::SceneOwnerUpdate update{++cycle, 1.0 / 60};
            require(session.updateAtOwnerSafePoint(update) && workspace.updateBeforeFrame(), "editing input owner");
            require(window.beginFrame({{1600, 900}, 1.0F / 60, {1, 1}}) && window.drawPanes(), "editing input panes");
            require(workspace.afterDraw(1.0 / 60, {1, 1}) && session.advanceScene(update), "editing input apply");
            auto snapshot = window.finishFrame();
            require(snapshot, "editing input snapshot");
            auto packet = renderer.sealFrame(*snapshot, workspace.frameImages());
            require(packet, "editing input packet");
            workspace.releaseFrameImages();
            while (packet->valid())
            {
                require(Clock::now() < deadline && renderer.trySubmitFrame(*packet) && renderer.poll(64),
                    "editing input retry retained packet");
                std::this_thread::yield();
            }
        };
        const auto move = [&](float x, float y) { input.feedInput(lux::ui::UiPointerMove{{x, y}}); step(); };
        const auto button = [&](bool down) {
            input.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, down}); step();
        };
        input.feedInput(lux::ui::UiWindowFocus{true});
        require(workspace.activate(), "editing input workspace focus");
        step();
        move(1490, 117);
        step();
        button(true);
        step();
        move(1510, 117);
        move(1530, 117);
        step();
        const auto preview = session.readAuthor(target, true);
        require(preview, "editing input preview read");
        std::printf("ER2 Inspector drag preview=(%f,%f,%f) original=(%f,%f,%f) revision=%llu\n",
            preview->transform->translation.x(), preview->transform->translation.y(),
            preview->transform->translation.z(),
            original->transform->translation.x(), original->transform->translation.y(),
            original->transform->translation.z(), session.stamp().preview_revision);
        button(false);
        step();
        require(session.readAuthor(target)->transform->translation != original->transform->translation,
            "actual Inspector drag commits changed translation");
        const auto edited = session.readAuthor(target);
        move(700, 14);
        button(true); button(false);
        const auto reset = session.readAuthor(target);
        require(reset->transform->translation.isZero() && reset->transform->scale.isOnes(),
            "actual toolbar reset changes the same author object");
        require(session.undo(), "toolbar undo shares Inspector history");
        require(session.readAuthor(target)->transform->translation == edited->transform->translation,
            "first undo restores Inspector result");
        require(session.undo(), "Inspector undo after toolbar undo");
        require(session.readAuthor(target)->transform->translation == original->transform->translation,
            "second undo restores pre-Inspector author value");
        const auto before_cancel = session.historyView()->history;
        const auto drag = [&] {
            for (unsigned i = 0; i < 24; ++i) step(); // Separate gestures from double-click text entry.
            move(1490, 117); step(); button(true); step(); move(1510, 117); move(1530, 117); step();
            require(session.readAuthor(target, true)->transform->translation !=
                session.readAuthor(target)->transform->translation, "actual drag has uncommitted preview");
        };
        drag();
        input.feedInput(lux::ui::UiKey{lux::ui::EKey::ESCAPE, true}); step();
        input.feedInput(lux::ui::UiKey{lux::ui::EKey::ESCAPE, false}); step(); button(false);
        require(session.readAuthor(target, true)->transform->translation == original->transform->translation,
            "Escape clears actual Inspector preview");
        drag();
        input.feedInput(lux::ui::UiWindowFocus{false}); step(); button(false);
        require(session.readAuthor(target, true)->transform->translation == original->transform->translation,
            "window focus loss clears actual Inspector preview");
        input.feedInput(lux::ui::UiWindowFocus{true}); step();
        drag();
        auto *inspector = input.focusedPane();
        require(inspector && inspector->type().name() == "lux.scene.inspector", "actual Inspector owns input");
        inspector->setVisible(false); step(); button(false);
        require(session.readAuthor(target, true)->transform->translation == original->transform->translation,
            "hiding Inspector clears its preview");
        const auto after_cancel = session.historyView()->history;
        require(after_cancel.current == before_cancel.current && after_cancel.revision == before_cancel.revision &&
            after_cancel.entry_count == before_cancel.entry_count, "cancel paths leave history and redo intact");
        require(session.redo(), "redo remains available with Inspector hidden");
        inspector->setVisible(true); step();
        require(session.readAuthor(target)->transform->translation == edited->transform->translation,
            "reopened Inspector reads the replayed committed value");
        require(window.requestClose() && window.closeRequested() && window.cancelCloseRequest() &&
            !window.closeRequested(), "close request can be cancelled before owner detachment");
        std::puts("ER2 Inspector/toolbar shared history, Escape/focus/hide cancellation and replay PASS; UI events");
    }
}
