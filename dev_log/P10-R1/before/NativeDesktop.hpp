// Native OS automation belongs to this non-installed harness, not the desktop library.
#if defined(_WIN32)
template <class T> T* findControl(object::LuxObject& object)
{
    if (auto* control = dynamic_cast<T*>(&object))
        return control;
    for (auto* child = object.firstChild(); child; child = child->nextSibling())
        if (auto* control = findControl<T>(*child))
            return control;
    return nullptr;
}
void nativeDesktop(Fixture& f, window::LuxWindow& window)
{
    const auto hwnd = static_cast<HWND>(window.win32Handle());
    const auto previous_window = GetForegroundWindow();
    POINT previous_pointer{};
    assert(GetCursorPos(&previous_pointer));
    ShowWindow(hwnd, SW_SHOW);
    assert(IsWindow(hwnd));
    SetForegroundWindow(hwnd);
    if (GetForegroundWindow() != hwnd)
    {
        const auto foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        const auto current_thread = GetCurrentThreadId();
        const bool attached =
            foreground_thread != current_thread && AttachThreadInput(current_thread, foreground_thread, TRUE);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        if (attached)
            AttachThreadInput(current_thread, foreground_thread, FALSE);
    }
    std::fprintf(
        stderr,
        "native activation: foreground=%d visible=%d\n",
        GetForegroundWindow() == hwnd,
        IsWindowVisible(hwnd)
    );
    assert(GetForegroundWindow() == hwnd);
    SetFocus(hwnd);
    const auto frames = [&] {
        const auto captured = f.desktop->presentation().capturedFrames();
        f.wait([&] { return f.desktop->presentation().capturedFrames() >= captured + 3; });
    };
    const auto pointer = [&](ui::Point point) {
        POINT screen{static_cast<LONG>(point.x), static_cast<LONG>(point.y)};
        assert(ClientToScreen(hwnd, &screen));
        assert(SetCursorPos(screen.x, screen.y));
        // A captured GPU frame does not prove the OS input queue has delivered this movement.
        // Wait for the actual platform position and for Root to consume that native event.
        std::uint64_t sequence{};
        f.wait([&] {
            for (const auto& event : f.input_.snapshot().events)
                if (const auto* cursor = std::get_if<input::CursorAction>(&event))
                    sequence = std::max(sequence, cursor->sequence);
            const auto& input = f.input_.snapshot();
            const bool reached = std::abs(input.cursor_x - point.x) <= 1 && std::abs(input.cursor_y - point.y) <= 1;
            return reached && f.desktop->root().inputSnapshot().sequence >= sequence;
        });
        frames();
    };
    const auto button = [&](DWORD flags) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        assert(SendInput(1, &input, sizeof input) == 1);
        const bool right = flags == MOUSEEVENTF_RIGHTDOWN || flags == MOUSEEVENTF_RIGHTUP;
        const bool down = flags == MOUSEEVENTF_LEFTDOWN || flags == MOUSEEVENTF_RIGHTDOWN;
        f.wait([&] { return f.desktop->root().inputSnapshot().buttons[right ? 2 : 0] == down; });
        frames();
    };
    const auto key = [&](WORD value) {
        INPUT inputs[2]{};
        for (auto& input : inputs)
        {
            input.type = INPUT_KEYBOARD;
            input.ki.wScan = value;
            input.ki.dwFlags = KEYEVENTF_UNICODE;
        }
        inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        assert(SendInput(2, inputs, sizeof(INPUT)) == 2);
        frames();
    };
    const auto center = [](const ui::Element& element) {
        const auto origin = element.contentOrigin();
        return ui::Point{origin.x + element.rect().size.width / 2, origin.y + element.rect().size.height / 2};
    };
    const auto before = f.session->describe();
    // Establish the input precondition even when a previous failed process ended while holding a button.
    button(MOUSEEVENTF_LEFTUP);
    button(MOUSEEVENTF_RIGHTUP);
    author::SceneInteractionGroup group(f.store.access<author::SceneSession>(), *f.key, {77});
    auto detached = take(author::makeInspectorView(
        f.messages.dispatcherRef(),
        ui::PaneId{"native-inspector"},
        f.store.access<author::SceneSession>(),
        {*f.key, &group},
        {f.key->id(), before.current.state.history, f.object},
        f.environment.components,
        author::sceneInspectorComponents()
    ));
    auto* inspector = static_cast<author::InspectorView*>(detached.pane());
    const auto id = take(f.desktop->views().adopt(detached, views::ViewRestoreKey{"native-inspector"})).id;
    assert(f.desktop->views().focus(id));
    frames();
    auto* number = findControl<ui::NumericEdit>(*inspector);
    assert(number && number->displayed());
    auto start = center(*number);
    pointer(start);
    button(MOUSEEVENTF_LEFTDOWN);
    std::fprintf(
        stderr,
        "native drag: point=(%.1f,%.1f) sampled=(%.1f,%.1f) editing=%d foreground=%d\n",
        start.x,
        start.y,
        f.input_.snapshot().cursor_x,
        f.input_.snapshot().cursor_y,
        number->editing(),
        GetForegroundWindow() == hwnd
    );
    f.wait([&] { return number->editing(); });
    pointer({start.x + 40, start.y});
    std::fprintf(
        stderr,
        "native moved: overlay=%d editing=%d left=%d\n",
        group.overlay() != nullptr,
        number->editing(),
        f.desktop->root().inputSnapshot().buttons[0]
    );
    f.wait([&] { return group.overlay() != nullptr; });
    assert(group.overlay() && f.session->describe().current == before.current);
    button(MOUSEEVENTF_LEFTUP);
    f.wait([&] { return !group.overlay(); });
    assert(f.session->describe().current != before.current);
    assert(f.session->undo());
    assert(f.session->describe().current == before.current);
    assert(f.session->redo());
    assert(f.session->undo());
    assert(f.desktop->root().focusedPane() == inspector);
    assert(f.desktop->views().close(id));
    frames();
    assert(!f.desktop->views().describe(id) && !f.desktop->root().focusedElement());

    auto scene_owner =
        take(author::makeSceneView(f.messages.dispatcherRef(), f.services(), f.info("native-scene", group)));
    auto* scene_view = static_cast<author::SceneView*>(scene_owner.pane());
    const auto scene_id = take(f.desktop->views().adopt(scene_owner, views::ViewRestoreKey{"native-scene"})).id;
    assert(f.desktop->views().focus(scene_id));
    f.wait([&] { return scene_view->image().isValid(); });
    frames();
    auto* viewport = findControl<author::SceneElement>(*scene_view);
    assert(viewport && viewport->displayed());
    pointer(center(*viewport));
    const auto rotation = scene_view->state().camera.transform.rotation;
    button(MOUSEEVENTF_RIGHTDOWN);
    // Move outside the viewport: continued navigation proves Root routes the captured pointer.
    pointer({975, 725});
    assert(scene_view->state().camera.transform.rotation != rotation);
    assert(f.session->describe().current == before.current);
    assert(f.desktop->views().close(scene_id));
    frames();
    assert(!f.desktop->views().describe(scene_id));
    button(MOUSEEVENTF_RIGHTUP); // Must not deliver to the retired SceneElement.

    // Real OS keyboard input through the same desktop, plus close during DIRECT delivery.
    struct TextWindow final : ui::Pane
    {
        ui::Layout layout{*this, ui::ElementId{"body"}, ui::ELayoutType::VERTICAL};
        ui::TextEdit text{layout, ui::ElementId{"text"}};
        ui::Button close{layout, ui::ElementId{"close"}, "Close from signal"};
        explicit TextWindow(object::ObjectDispatcherRef dispatcher)
            : Pane(dispatcher, ui::PaneId{"native-text"}, ui::PaneTypeId{"test.input"}, "Text input")
        {
            setContent(layout);
        }
    };
    auto text_owner = std::make_unique<TextWindow>(f.messages.dispatcherRef());
    auto* text = text_owner.get();
    views::DetachedView text_view(contracts::CodeLease::builtin(), std::move(text_owner));
    const auto text_id = take(f.desktop->views().adopt(text_view, views::ViewRestoreKey{"native-text"})).id;
    assert(f.desktop->views().focus(text_id));
    frames();
    pointer(center(text->text));
    button(MOUSEEVENTF_LEFTDOWN);
    button(MOUSEEVENTF_LEFTUP);
    key('a');
    key('b');
    std::fprintf(
        stderr,
        "native TextEdit value='%s' focus=%d composing=%d\n",
        text->text.value().c_str(),
        f.desktop->root().focusedElement() == &text->text,
        f.desktop->root().inputSnapshot().composing
    );
    assert(text->text.value() == "ab");
    assert(f.desktop->root().focusedElement() == &text->text);
    bool signalled{};
    auto connection = take(object::LuxObject::connect(&text->close, &ui::Button::activated, [&]() noexcept {
        assert(f.desktop->views().close(text_id));
        assert(text->attachedRoot() && !signalled);
        signalled = true;
    }));
    pointer(center(text->close));
    button(MOUSEEVENTF_LEFTDOWN);
    button(MOUSEEVENTF_LEFTUP);
    assert(signalled && !f.desktop->views().describe(text_id));
    assert(!f.desktop->root().focusedElement());
    assert(SetCursorPos(previous_pointer.x, previous_pointer.y));
    if (previous_window)
        SetForegroundWindow(previous_window);
    std::printf(
        "P10 native desktop: OS mouse drag -> generated Inspector preview/commit/undo; "
        "captured SceneElement navigates outside its rectangle and closes while held; "
        "OS keyboard -> TextEdit; DIRECT close releases focus after callback; "
        "system IME candidate/commit NOT tested; validation_errors=%llu\n",
        static_cast<unsigned long long>(f.renderer->statistics().validation_errors)
    );
}
#endif
