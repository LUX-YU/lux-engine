// Controlled input into the real installed ImGui widgets and GraphCanvas signal connections.
// This is not OS/IME qualification and never reaches a tool Impl or authors an expected stamp.

namespace draft_test
{
    namespace ef = editor::flowforge;
    namespace em = editor::material;

    template <class T> T* find(object::LuxObject& owner)
    {
        if (auto* value = dynamic_cast<T*>(&owner))
            return value;
        for (auto* child = owner.firstChild(); child; child = child->nextSibling())
            if (auto* value = find<T>(*child))
                return value;
        return nullptr;
    }
    struct CanvasInput : object::LuxObject
    {
        template <class T>
        static void send(widgets::GraphCanvas& canvas, object::TSignal<T>& signal, const T& value)
        {
            // A pointer to the inherited member keeps the actual sender as `this`.
            const auto emit_member = &CanvasInput::emit<T>;
            const auto delivery = (canvas.*emit_member)(signal, value);
            assert(delivery.direct > 0 && !delivery.full && !delivery.closed);
        }
        static void select(widgets::GraphCanvas& canvas, std::uint64_t id)
        {
            const std::span<const std::uint64_t> selection{&id, 1};
            send(canvas, canvas.selected, selection);
        }
        static void move(widgets::GraphCanvas& canvas, std::uint64_t id, float x, bool begin, bool commit)
        {
            send(canvas, canvas.edited, widgets::CanvasEdit{widgets::CanvasMove{{{id, {x, 50}}}}, begin, commit, false});
        }
    };
    struct UiInput final : ui::Pane
    {
        Fixture& fixture;
        ImGuiContext* context{};
        ImGuiID hook{}, activate{};
        ImGuiActivateFlags flags{};
        struct Probe final : ui::Element
        {
            UiInput& input;
            explicit Probe(UiInput& owner) : Element(owner, ui::ElementId{"probe"}), input(owner) {}
            void draw() noexcept override
            {
                input.context = ImGui::GetCurrentContext();
                ImGui::TextUnformatted("Controlled P10 R1 input");
            }
        } probe;
        explicit UiInput(Fixture& f)
            : Pane(f.desktop->root(), ui::PaneId{"r1-probe"}, ui::PaneTypeId{"test.probe"}, "Input probe"),
              fixture(f), probe(*this)
        {
            setContent(probe);
            fixture.wait([&] { return context != nullptr; });
            ImGuiContextHook value;
            value.Type = ImGuiContextHookType_NewFramePost;
            value.UserData = this;
            value.Callback = [](ImGuiContext* context, ImGuiContextHook* hook) {
                auto& input = *static_cast<UiInput*>(hook->UserData);
                if (input.activate)
                {
                    context->NavActivateId = input.activate;
                    context->NavActivateDownId = input.activate;
                    context->NavActivatePressedId = input.activate;
                    context->NavActivateFlags = input.flags;
                    input.activate = 0;
                }
            };
            hook = ImGui::AddContextHook(context, &value);
        }
        ~UiInput() noexcept override { ImGui::RemoveContextHook(context, hook); }
        void frame()
        {
            const auto previous = fixture.desktop->presentation().capturedFrames();
            fixture.wait([&] { return fixture.desktop->presentation().capturedFrames() > previous; });
        }
        ImGuiID property(const char* window, const char* label, std::optional<int> pin = {}) const
        {
            auto id = ImHashStr(window);
            id = ImHashStr("content", 0, id);
            id = ImHashStr("properties", 0, id);
            if (pin)
                id = ImHashData(&*pin, sizeof(int), id);
            return ImHashStr(label, 0, id);
        }
        void click(ImGuiID id, ImGuiActivateFlags activation = ImGuiActivateFlags_None)
        {
            activate = id;
            flags = activation;
            frame();
            assert(!activate);
        }
        void text(ImGuiID id, const char* value)
        {
            click(id, ImGuiActivateFlags_PreferInput);
            assert(context->ActiveId == id);
            auto* previous = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(context);
            auto& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiKey_A, true);
            frame();
            io.AddKeyEvent(ImGuiKey_A, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            io.AddInputCharactersUTF8(value);
            frame();
            io.AddKeyEvent(ImGuiKey_Enter, true);
            frame();
            io.AddKeyEvent(ImGuiKey_Enter, false);
            frame();
            ImGui::SetCurrentContext(previous);
        }
    };
    bool unchanged(const sessions::SessionInfo& before, const sessions::SessionInfo& after)
    {
        return before.id == after.id && before.current == after.current && before.observed == after.observed &&
               before.dirty == after.dirty && before.binding == after.binding;
    }
    template <class View> bool stale(const View& view)
    {
        if (view.status())
            return false;
        return std::visit([](const auto& error) {
            using T = std::decay_t<decltype(error)>;
            if constexpr (std::same_as<T, ef::FlowEditError>)
                return error.code == ef::EFlowEditError::STALE_CONTENT ||
                       (error.code == ef::EFlowEditError::SESSION && error.session == sessions::ESessionError::STALE_CONTENT);
            else if constexpr (std::same_as<T, em::MaterialEditError>)
                return error.code == em::EMaterialEditError::STALE_CONTENT ||
                       (error.code == em::EMaterialEditError::SESSION && error.session == sessions::ESessionError::STALE_CONTENT);
            else
                return false;
        }, view.status().error());
    }
    bool flow(Fixture& f, std::string_view mode)
    {
        const bool signature = mode == "r1-signature";
        const bool canvas_case = mode == "r1-flow-canvas";
        const asset::AssetId asset{uuid("r1-flow")};
        ef::FlowAuthoringSource source{asset, "S0", {}};
        std::unique_ptr<lux::flowforge::Node> node;
        if (signature)
            node = std::make_unique<lux::flowforge::FuncDefNode>("S0 function", std::vector<lux::flowforge::FuncArgInfo>{});
        else
            node = std::make_unique<lux::flowforge::BinaryOpNode>(0, lux::flowforge::ENodeOperation::ADD, meta::builtin_ref_type_ptr<double>());
        const auto handle = source.graph.addNodes(std::move(node));
        const auto id = source.graph.getNode(handle).node->id();
        auto reservation = take(f.store.reserve<ef::FlowSession>({"lux.editor.flowforge"}, contracts::CodeLease::builtin()));
        auto model = take(ef::FlowSession::create(reservation.id(), sessions::BoundSource{asset,"r1.flow"}, std::move(source)));
        auto* session = model.get();
        assert(f.store.prepare(reservation, model));
        const auto key = take(f.store.key<ef::FlowSession>(take(f.store.publish(reservation))));
        auto captured = take(session->capture());
        const auto pin = signature ? lux::flowforge::PinId{} : captured.source().nodes.front().inputs.front().id;
        if (!signature)
        {
            ef::FlowEditBatch batch{session->describe().current,"initial literal",{}};
            batch.edits.emplace_back(ef::FlowSetLiteral{pin,{lux::flowforge::EFlowLiteralKind::REAL,"10"}});
            assert(session->apply(std::move(batch)));
        }
        ef::FlowInteraction interaction(f.store.access<ef::FlowSession>(),key);
        ef::FlowCompilationService compilation(f.execution);
        auto detached = take(ef::makeFlowView(f.messages.dispatcherRef(), ui::PaneId{"r1-flow"},
            {f.store.access<ef::FlowSession>(), compilation}, ef::FlowViewBinding{key,&interaction}));
        auto* view = static_cast<ef::FlowView*>(detached.pane());
        const auto mounted = take(f.desktop->views().adopt(detached,views::ViewRestoreKey{"r1-flow"})).id;
        UiInput input(f);
        auto* graph = find<widgets::GraphCanvas>(*view);
        assert(graph);
        CanvasInput::select(*graph,id.value);
        f.frame(false);
        input.frame();
        const auto s0 = session->describe();
        const auto s0_bytes = take(take(session->read()).encode());
        if (canvas_case)
            CanvasInput::move(*graph,id.value,20,true,true);
        else if (signature)
            input.text(input.property("Flow###r1-flow","Function name"),"Draft function");
        else
            input.text(input.property("Flow###r1-flow","##value",static_cast<int>(pin.value)),"20");
        assert(unchanged(s0,session->describe()) && s0_bytes == take(take(session->read()).encode()));
        ef::FlowEditBatch external{session->describe().current,"S1 elsewhere",{}};
        if (canvas_case)
            external.edits.emplace_back(ef::FlowMoveNodes{{{id,{30,50,true}}}});
        else if (signature)
        {
            auto value = std::get<lux::flowforge::FlowSourceSignature>(captured.source().nodes.front().parameters);
            value.arguments.push_back({"external argument", std::string(meta::builtin_ref_type_ptr<double>()->name)});
            external.edits.emplace_back(ef::FlowSetSignature{id,"S1 function",value});
        }
        else
            external.edits.emplace_back(ef::FlowSetLiteral{pin,{lux::flowforge::EFlowLiteralKind::REAL,"30"}});
        assert(session->apply(std::move(external)));
        const auto s1 = session->describe();
        const auto bytes = take(take(session->read()).encode());
        if (canvas_case)
            f.frame(false);
        else
        {
            f.frame(false); // Normal display refresh happens BEFORE the old property Apply.
            assert(unchanged(s1,session->describe()));
            input.click(input.property("Flow###r1-flow",signature ? "Apply signature" : "Apply literal",
                signature ? std::optional<int>{} : std::optional<int>{static_cast<int>(pin.value)}));
        }
        const bool preserved = unchanged(s1,session->describe()) && bytes == take(take(session->read()).encode());
        const bool rejected = stale(*view);
        std::fprintf(stderr,"P10 R1 %.*s: real cached property/canvas input; S0!=S1=%d source_and_history_preserved=%d stale=%d\n",
            static_cast<int>(mode.size()),mode.data(),s0.current!=s1.current,preserved,rejected);
        assert(view->cancelEdit());
        assert(f.desktop->views().close(mounted));
        f.wait([&]{return !f.desktop->views().describe(mounted);});
        auto permit=take(f.store.prepareClose(session->describe().current));
        assert(f.store.close(permit));
        return preserved && rejected;
    }
    bool material(Fixture& f)
    {
        const asset::AssetId asset{uuid("r1-material")};
        lux::material::MaterialSource source{asset,"S0",{}};
        const auto id=source.graph.addNode(std::make_unique<lux::material::ConstantNode>());
        auto reservation=take(f.store.reserve<em::MaterialSession>({"lux.editor.material"},contracts::CodeLease::builtin()));
        auto model=take(em::MaterialSession::create(reservation.id(),sessions::BoundSource{asset,"r1.material"},std::move(source)));
        auto* session=model.get();
        assert(f.store.prepare(reservation,model));
        const auto key=take(f.store.key<em::MaterialSession>(take(f.store.publish(reservation))));
        em::MaterialInteraction interaction(f.store.access<em::MaterialSession>(),key);
        em::MaterialPreviewStore preview{*f.runtime,{f.environment,{}}};
        auto detached=take(em::makeMaterialView(f.messages.dispatcherRef(),ui::PaneId{"r1-material"},
            {f.store.access<em::MaterialSession>(),*f.runtime,*f.resources,*f.renderer,preview,{}, {},{3}},
            em::MaterialViewBinding{key,&interaction}));
        auto* view=static_cast<em::MaterialView*>(detached.pane());
        const auto mounted=take(f.desktop->views().adopt(detached,views::ViewRestoreKey{"r1-material"})).id;
        auto* graph=find<widgets::GraphCanvas>(*view);
        assert(graph);
        const auto s0=session->describe();
        CanvasInput::move(*graph,id.value,20,true,true);
        std::vector<em::VMaterialEdit> edits;
        edits.emplace_back(em::MaterialPlaceNode{id,{30,50,true}});
        assert(session->apply({s0.current,"S1 elsewhere",std::move(edits)}));
        const auto s1=session->describe();
        const auto bytes=take(take(session->read()).encode());
        f.frame(false);
        const bool preserved=unchanged(s1,session->describe()) && bytes==take(take(session->read()).encode());
        const bool rejected=stale(*view);
        std::fprintf(stderr,"P10 R1 material-canvas: actual GraphCanvas signal -> queued BEGIN; source_and_history_preserved=%d stale=%d\n",preserved,rejected);
        assert(view->cancelEdit());
        assert(f.desktop->views().close(mounted));
        f.wait([&]{return !f.desktop->views().describe(mounted);});
        auto permit=take(f.store.prepareClose(session->describe().current));
        assert(f.store.close(permit));
        return preserved && rejected;
    }
}
