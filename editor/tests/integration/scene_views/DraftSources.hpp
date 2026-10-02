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
        template <class T> static void send(widgets::GraphCanvas& canvas, object::TSignal<T>& signal, const T& value)
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
            send(
                canvas,
                canvas.edited,
                widgets::CanvasEdit{widgets::CanvasMove{{{id, {x, 50}}}}, begin, commit, false}
            );
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
            : Pane(f.desktop->root(), ui::PaneId{"r1-probe"}, ui::PaneTypeId{"test.probe"}, "Input probe"), fixture(f),
              probe(*this)
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
        ~UiInput() noexcept override
        {
            ImGui::RemoveContextHook(context, hook);
        }
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
        return std::visit(
            [](const auto& error) {
                using T = std::decay_t<decltype(error)>;
                if constexpr (std::same_as<T, ef::FlowEditError>)
                    return error.code == ef::EFlowEditError::STALE_CONTENT ||
                           (error.code == ef::EFlowEditError::SESSION &&
                            error.session == sessions::ESessionError::STALE_CONTENT);
                else if constexpr (std::same_as<T, em::MaterialEditError>)
                    return error.code == em::EMaterialEditError::STALE_CONTENT ||
                           (error.code == em::EMaterialEditError::SESSION &&
                            error.session == sessions::ESessionError::STALE_CONTENT);
                else
                    return false;
            },
            view.status().error()
        );
    }
#ifdef LUX_P10_R1_NATIVE
    template <class Session> const auto& data(Session& session)
    {
        if constexpr (std::same_as<Session, ef::FlowSession>)
            return ef::detail::FlowSessionAccess::data(session);
        else
            return em::detail::MaterialSessionAccess::data(session);
    }
    struct NativeFacts final
    {
        editing::HistorySnapshot history;
        sessions::BindingRevision binding;
        std::optional<sessions::PersistedState> persisted;
        template <class Session>
        explicit NativeFacts(Session& session)
            : history(take(data(session).history->view()).snapshot), binding(data(session).state.bindingRevision()),
              persisted(data(session).state.checkpoint().persisted())
        {}
        template <class Session> void check(Session& session) const
        {
            const auto h = take(data(session).history->view()).snapshot;
            assert(history.history == h.history && history.current == h.current && history.revision == h.revision);
            assert(
                history.event_sequence == h.event_sequence && history.entry_count == h.entry_count &&
                history.cursor == h.cursor
            );
            assert(history.charged_retained_bytes == h.charged_retained_bytes && history.closed == h.closed);
            assert(
                binding == data(session).state.bindingRevision() &&
                persisted == data(session).state.checkpoint().persisted()
            );
        }
    };
#endif
    template <class Session> void closeSession(Fixture& f, Session& session)
    {
        auto permit = take(f.store.prepareClose(session.describe().current));
        assert(f.store.close(permit));
    }
    template <class Session, class Fn> void underRead(Session& session, Fn&& fn)
    {
        auto read = take(session.read());
        if constexpr (std::same_as<Session, ef::FlowSession>)
            assert(read.withRead([&]() -> ef::FlowEditResult<void> {
                fn();
                return {};
            }));
        else
            assert(read.withRead([&](const lux::material::MaterialSource&) -> em::MaterialEditResult<void> {
                fn();
                return {};
            }));
    }
    template <class View> bool busy(const View& view)
    {
        if (view.status())
            return false;
        return std::visit(
            [](const auto& error) {
                if constexpr (requires { error.session; })
                    return error.session == sessions::ESessionError::BUSY;
                else
                    return false;
            },
            view.status().error()
        );
    }
    template <class Session> auto bytes(Session& session)
    {
        return take(take(session.read()).encode());
    }
    template <class Session, class View> void roundTrip(Session& session, View& view, const auto& before)
    {
        const auto after = bytes(session);
        assert(after != before);
        const auto state = session.describe().current.state;
        assert(view.undo());
        assert(bytes(session) == before);
        assert(view.redo());
        assert(bytes(session) == after && session.describe().current.state == state);
    }
    template <class Interaction> float previewX(const Interaction& interaction)
    {
        assert(interaction.overlay() && interaction.overlay()->edits.size() == 1);
        if constexpr (std::same_as<Interaction, ef::FlowInteraction>)
        {
            const auto& move = std::get<ef::FlowMoveNodes>(interaction.overlay()->edits.front());
            assert(move.value.size() == 1 && move.value.front().layout.y == 50);
            return move.value.front().layout.x;
        }
        else
        {
            const auto& move = std::get<em::MaterialPlaceNode>(interaction.overlay()->edits.front());
            assert(move.value.y == 50);
            return move.value.x;
        }
    }
    template <class Session, class View, class Interaction>
    void queueCases(
        Fixture& f,
        Session& session,
        View& view,
        Interaction& interaction,
        widgets::GraphCanvas& graph,
        std::uint64_t id
    )
    {
        const auto before = bytes(session);
        const auto original = session.describe();
        CanvasInput::move(graph, id, 20, true, false);
        underRead(session, [&] {
            f.frame(false);
            assert(busy(view) && !interaction.overlay());
        });
        assert(unchanged(original, session.describe()) && bytes(session) == before);
        f.frame(false);
        const auto* overlay = interaction.overlay();
        assert(overlay && overlay->expected == original.current && previewX(interaction) == 20);
        CanvasInput::move(graph, id, 25, false, false);
        underRead(session, [&] {
            f.frame(false);
            assert(busy(view) && interaction.overlay() == overlay && previewX(interaction) == 20);
        });
        f.frame(false);
        assert(interaction.overlay() == overlay && interaction.overlay()->expected == original.current);
        assert(previewX(interaction) == 25);
        assert(unchanged(original, session.describe()) && bytes(session) == before);
        CanvasInput::move(graph, id, 40, false, true);
        f.frame(false);
        assert(!interaction.overlay() && view.status());
        const auto done = session.describe();
        f.frame(false);
        assert(unchanged(done, session.describe()));
        roundTrip(session, view, before); // One undo removes ALL preview moves.
        f.frame(false);
        const auto held = session.describe();
        const auto held_bytes = bytes(session);
        // Capacity stays bounded while an accepted request cannot BEGIN. Cancel restores it.
        for (int i = 0; i < 65; ++i)
            CanvasInput::move(graph, id, 70, true, true);
        assert(!view.status() && !graph.enabled());
        underRead(session, [&] {
            f.frame(false);
            assert(busy(view));
        });
        assert(unchanged(held, session.describe()));
        assert(view.cancelEdit());
        assert(graph.enabled() && bytes(session) == held_bytes);
        f.frame(false);
        CanvasInput::move(graph, id, 90, true, true);
        f.frame(false);
        assert(view.status() && !interaction.overlay());
        roundTrip(session, view, held_bytes);
        std::fputs("R10-R1-05/06 BUSY stable pointer/source/stage, one history, capacity/cancel/retry PASS\n", stderr);
    }
    bool flow(Fixture& f, std::string_view mode)
    {
        const bool signature = mode == "r1-signature";
        const bool canvas_case = mode == "r1-flow-canvas";
        const bool queue_case = mode == "r1-flow-queue";
        const bool positive = mode == "r1-positive";
        const asset::AssetId asset{uuid("r1-flow")};
        ef::FlowAuthoringSource source{asset, "S0", {}};
        std::unique_ptr<lux::flowforge::Node> node;
        if (signature)
            node = std::make_unique<lux::flowforge::FuncDefNode>(
                "S0 function",
                std::vector<lux::flowforge::FuncArgInfo>{}
            );
        else
            node = std::make_unique<lux::flowforge::BinaryOpNode>(
                0,
                lux::flowforge::ENodeOperation::ADD,
                meta::builtin_ref_type_ptr<double>()
            );
        const auto handle = source.graph.addNodes(std::move(node));
        const auto id = source.graph.getNode(handle).node->id();
        auto reservation =
            take(f.store.reserve<ef::FlowSession>({"lux.editor.flowforge"}, contracts::CodeLease::builtin()));
        auto model =
            take(ef::FlowSession::create(reservation.id(), sessions::BoundSource{asset, "r1.flow"}, std::move(source)));
        auto* session = model.get();
        assert(f.store.prepare(reservation, model));
        const auto key = take(f.store.key<ef::FlowSession>(take(f.store.publish(reservation))));
        if (signature)
        {
            ef::FlowEditBatch calls{session->describe().current, "associated call", {}};
            calls.edits.emplace_back(ef::FlowInsertFunctionUse{id, false});
            assert(session->apply(std::move(calls)));
        }
        auto captured = take(session->capture());
        const auto pin = signature ? lux::flowforge::PinId{} : captured.source().nodes.front().inputs.front().id;
        if (!signature)
        {
            ef::FlowEditBatch batch{session->describe().current, "initial literal", {}};
            batch.edits.emplace_back(ef::FlowSetLiteral{pin, {lux::flowforge::EFlowLiteralKind::REAL, "10"}});
            assert(session->apply(std::move(batch)));
        }
        ef::FlowInteraction interaction(f.store.access<ef::FlowSession>(), key);
        ef::FlowCompilationService compilation(f.execution);
        auto detached = take(ef::makeFlowView(
            f.messages.dispatcherRef(),
            ui::PaneId{"r1-flow"},
            {f.store.access<ef::FlowSession>(), compilation, {}},
            ef::FlowViewBinding{key, &interaction}
        ));
        auto* view = static_cast<ef::FlowView*>(detached.pane());
        const auto mounted = take(f.desktop->views().adopt(detached, views::ViewRestoreKey{"r1-flow"})).id;
        UiInput input(f);
        auto* graph = find<widgets::GraphCanvas>(*view);
        assert(graph);
        CanvasInput::select(*graph, id.value);
        f.frame(false);
        input.frame();
        if (queue_case)
        {
            queueCases(f, *session, *view, interaction, *graph, id.value);
            assert(view->cancelEdit());
            assert(f.desktop->views().close(mounted));
            f.wait([&] { return !f.desktop->views().describe(mounted); });
            closeSession(f, *session);
            return true;
        }
        const auto s0 = session->describe();
        const auto s0_bytes = take(take(session->read()).encode());
        if (canvas_case)
            CanvasInput::move(*graph, id.value, 20, true, true);
        else if (signature)
            input.text(input.property("Flow###r1-flow", "Function name"), "Draft function");
        else
            input.text(input.property("Flow###r1-flow", "##value", static_cast<int>(pin.value)), "20");
        assert(unchanged(s0, session->describe()) && s0_bytes == take(take(session->read()).encode()));
        if (canvas_case)
        {
            underRead(*session, [&] {
                f.frame(false);
                assert(busy(*view) && !interaction.overlay());
            });
            assert(unchanged(s0, session->describe()));
        }
        if (mode == "r1-lifecycle")
        {
            const auto binding = view->binding();
            underRead(*session, [&] {
                auto rejected = view->rebind(std::nullopt);
                assert(!rejected && view->binding() == binding);
                f.frame(false);
                assert(busy(*view));
            });
            assert(unchanged(s0, session->describe()) && bytes(*session) == s0_bytes);
            closeSession(f, *session);
            auto next_reservation =
                take(f.store.reserve<ef::FlowSession>({"lux.editor.flowforge"}, contracts::CodeLease::builtin()));
            ef::FlowAuthoringSource next_source{asset, "new generation", {}};
            auto next_handle = next_source.graph.addNodes(std::make_unique<lux::flowforge::BinaryOpNode>(
                0,
                lux::flowforge::ENodeOperation::ADD,
                meta::builtin_ref_type_ptr<double>()
            ));
            assert(next_source.graph.getNode(next_handle).node->id() == id);
            auto next = take(ef::FlowSession::create(
                next_reservation.id(),
                sessions::BoundSource{asset, "r1.flow"},
                std::move(next_source)
            ));
            auto* replacement = next.get();
            assert(f.store.prepare(next_reservation, next));
            const auto next_key = take(f.store.key<ef::FlowSession>(take(f.store.publish(next_reservation))));
            assert(next_key != key);
            ef::FlowInteraction next_interaction(f.store.access<ef::FlowSession>(), next_key);
            const auto new_info = replacement->describe();
            const auto new_bytes = bytes(*replacement);
            input.click(input.property("Flow###r1-flow", "Apply literal", static_cast<int>(pin.value)));
            assert(!view->status() && view->binding() == binding);
            assert(unchanged(new_info, replacement->describe()) && bytes(*replacement) == new_bytes);
            assert(view->rebind(ef::FlowViewBinding{next_key, &next_interaction}));
            CanvasInput::select(*graph, id.value);
            f.frame(false);
            input.text(input.property("Flow###r1-flow", "##value", static_cast<int>(pin.value)), "40");
            input.click(input.property("Flow###r1-flow", "Apply literal", static_cast<int>(pin.value)));
            assert(view->status());
            roundTrip(*replacement, *view, new_bytes);
            assert(view->cancelEdit());
            assert(f.desktop->views().close(mounted));
            f.wait([&] { return !f.desktop->views().describe(mounted); });
            closeSession(f, *replacement);
            std::fputs(
                "R10-R1-03 failed BUSY rebind preserves; old generation rejected; new binding same IDs recaptured "
                "PASS\n",
                stderr
            );
            return true;
        }
        ef::FlowEditBatch external{session->describe().current, "S1 elsewhere", {}};
        if (canvas_case)
            external.edits.emplace_back(ef::FlowMoveNodes{{{id, {30, 50, true}}}});
        else if (signature)
        {
            auto value = std::get<lux::flowforge::FlowSourceSignature>(captured.source().nodes.front().parameters);
            value.arguments.push_back({"external argument", std::string(meta::builtin_ref_type_ptr<double>()->name)});
            value.results.push_back({"external result", std::string(meta::builtin_ref_type_ptr<double>()->name)});
            external.edits.emplace_back(ef::FlowSetSignature{id, "S1 function", value});
        }
        else
            external.edits.emplace_back(ef::FlowSetLiteral{pin, {lux::flowforge::EFlowLiteralKind::REAL, "30"}});
        if (!positive)
        {
#ifdef LUX_P10_R1_NATIVE
            if (mode == "r1-history")
            {
                ef::FlowAuthoringSource replacement{asset, "reloaded", {}};
                (void)replacement.graph.addNodes(std::make_unique<lux::flowforge::BinaryOpNode>(
                    0,
                    lux::flowforge::ENodeOperation::ADD,
                    meta::builtin_ref_type_ptr<double>()
                ));
                auto candidate = take(ef::PreparedFlowReload::prepare(*session, std::move(replacement)));
                assert(candidate.adopt(*session));
                assert(session->describe().current.state.history != s0.current.state.history);
                assert(take(session->capture()).source().nodes.front().id == id);
            }
            else
#endif
                assert(session->apply(std::move(external)));
        }
        const auto s1 = session->describe();
#ifdef LUX_P10_R1_NATIVE
        const NativeFacts facts(*session);
#endif
        const auto bytes = take(take(session->read()).encode());
        if (canvas_case)
            f.frame(false);
        else
        {
            f.frame(false); // Normal display refresh happens BEFORE the old property Apply.
            assert(unchanged(s1, session->describe()));
            input.click(input.property(
                "Flow###r1-flow",
                signature ? "Apply signature" : "Apply literal",
                signature ? std::optional<int>{} : std::optional<int>{static_cast<int>(pin.value)}
            ));
        }
        if (positive)
        {
            assert(view->status() && !interaction.overlay());
            roundTrip(*session, *view, s0_bytes);
            assert(view->cancelEdit());
            assert(f.desktop->views().close(mounted));
            f.wait([&] { return !f.desktop->views().describe(mounted); });
            closeSession(f, *session);
            std::fputs("R10-R1-01 positive actual property Apply/Undo/Redo PASS\n", stderr);
            return true;
        }
#ifdef LUX_P10_R1_NATIVE
        facts.check(*session);
#endif
        const bool preserved = unchanged(s1, session->describe()) && bytes == take(take(session->read()).encode());
        const bool rejected = stale(*view);
        std::fprintf(
            stderr,
            "P10 R1 %.*s: real cached property/canvas input; S0!=S1=%d source_and_history_preserved=%d stale=%d\n",
            static_cast<int>(mode.size()),
            mode.data(),
            s0.current != s1.current,
            preserved,
            rejected
        );
        assert(preserved && rejected);
        f.frame(false); // Rejected head cannot retry indefinitely or silently start again.
        assert(unchanged(s1, session->describe()) && stale(*view));
        if (!canvas_case)
        {
            // Explicit recapture through the actual Revert button, then a new local edit and Apply.
            input.click(input.property("Flow###r1-flow", "Revert properties"));
            assert(view->status() && unchanged(s1, session->describe()));
            if (signature)
                input.text(input.property("Flow###r1-flow", "Function name"), "Recovered function");
            else
                input.text(input.property("Flow###r1-flow", "##value", static_cast<int>(pin.value)), "40");
            assert(unchanged(s1, session->describe()) && bytes == take(take(session->read()).encode()));
            input.click(input.property(
                "Flow###r1-flow",
                signature ? "Apply signature" : "Apply literal",
                signature ? std::optional<int>{} : std::optional<int>{static_cast<int>(pin.value)}
            ));
            assert(view->status());
            roundTrip(*session, *view, bytes);
            std::fputs("R10-R1-06 actual Revert/edit/Apply one-history Undo/Redo PASS\n", stderr);
        }
        assert(view->cancelEdit());
        assert(f.desktop->views().close(mounted));
        f.wait([&] { return !f.desktop->views().describe(mounted); });
        auto permit = take(f.store.prepareClose(session->describe().current));
        assert(f.store.close(permit));
        return preserved && rejected;
    }
    struct CleanupProbe final
    {
        em::MaterialSession* session{};
        bool armed{};
        std::size_t released{};
    };
    class ProbeConstant final : public lux::material::ConstantNode
    {
    public:
        ProbeConstant(std::weak_ptr<const void> code, CleanupProbe& probe) : code_(std::move(code)), probe_(&probe) {}
        ~ProbeConstant() override
        {
            assert(!code_.expired());
            if (probe_->armed)
            {
                const auto state = probe_->session->describe();
                auto result = probe_->session->apply({state.current, "cleanup callback", {}});
                assert(!result && result.error().session == sessions::ESessionError::BUSY);
                ++probe_->released;
            }
        }
        std::unique_ptr<lux::material::Node> clone() const override
        {
            return std::make_unique<ProbeConstant>(*this);
        }

    private:
        std::weak_ptr<const void> code_;
        CleanupProbe* probe_;
    };
    bool material(Fixture& f, std::string_view mode)
    {
        const asset::AssetId asset{uuid("r1-material")};
        lux::material::MaterialSource source{asset, "S0", {}};
        CleanupProbe probe;
        auto code = std::make_shared<int>(1);
        const bool lifetime = mode == "r1-material-lifetime";
        std::unique_ptr<lux::material::Node> node =
            lifetime ? std::unique_ptr<lux::material::Node>{std::make_unique<ProbeConstant>(code, probe)}
                     : std::make_unique<lux::material::ConstantNode>();
        const auto id = source.graph.addNode(std::move(node));
        auto reservation =
            take(f.store.reserve<em::MaterialSession>({"lux.editor.material"}, contracts::CodeLease::builtin()));
        auto model = take(em::MaterialSession::create(
            reservation.id(),
            sessions::BoundSource{asset, "r1.material"},
            std::move(source),
            contracts::CodeLease::plugin(code)
        ));
        auto* session = model.get();
        probe.session = session;
        code.reset();
        assert(f.store.prepare(reservation, model));
        const auto key = take(f.store.key<em::MaterialSession>(take(f.store.publish(reservation))));
        em::MaterialInteraction interaction(f.store.access<em::MaterialSession>(), key);
        em::MaterialPreviewStore preview{*f.runtime, {f.environment, {}}};
        em::MaterialCompilationService compilation(f.execution);
        auto detached = take(em::makeMaterialView(
            f.messages.dispatcherRef(),
            ui::PaneId{"r1-material"},
            {f.store.access<em::MaterialSession>(),
             *f.runtime,
             *f.resources,
             *f.renderer,
             preview,
             compilation,
             f.environment,
             {},
             {3}},
            em::MaterialViewBinding{key, &interaction}
        ));
        auto* view = static_cast<em::MaterialView*>(detached.pane());
        const auto mounted = take(f.desktop->views().adopt(detached, views::ViewRestoreKey{"r1-material"})).id;
        auto* graph = find<widgets::GraphCanvas>(*view);
        assert(graph);
        if (lifetime)
        {
            CanvasInput::select(*graph, id.value);
            f.frame(false);
            const auto original = session->describe();
            const auto encoded = bytes(*session);
            probe.armed = true;
            underRead(*session, [&] {
                auto cancel = view->cancelEdit();
                assert(!cancel && probe.released == 0);
                auto rebound = view->rebind(std::nullopt);
                assert(!rebound && view->binding()->session == key && probe.released == 0);
            });
            assert(probe.released == 0);
            assert(view->cancelEdit());
            assert(probe.released == 1);
            probe.armed = false;
            assert(unchanged(original, session->describe()) && bytes(*session) == encoded);
            assert(f.desktop->views().close(mounted));
            f.wait([&] { return !f.desktop->views().describe(mounted); });
            closeSession(f, *session);
            std::fputs(
                "R10-R1-05 dynamic node draft: BUSY retains/code alive/disposal under original gate PASS\n",
                stderr
            );
            return true;
        }
        if (mode == "r1-material-queue")
        {
            queueCases(f, *session, *view, interaction, *graph, id.value);
            assert(view->cancelEdit());
            assert(f.desktop->views().close(mounted));
            f.wait([&] { return !f.desktop->views().describe(mounted); });
            closeSession(f, *session);
            return true;
        }
        const auto s0 = session->describe();
        CanvasInput::move(*graph, id.value, 20, true, true);
        underRead(*session, [&] {
            f.frame(false);
            assert(busy(*view) && !interaction.overlay());
        });
        assert(unchanged(s0, session->describe()));
        std::vector<em::VMaterialEdit> edits;
        edits.emplace_back(em::MaterialPlaceNode{id, {30, 50, true}});
        assert(session->apply({s0.current, "S1 elsewhere", std::move(edits)}));
        const auto s1 = session->describe();
#ifdef LUX_P10_R1_NATIVE
        const NativeFacts facts(*session);
#endif
        const auto bytes = take(take(session->read()).encode());
        f.frame(false);
#ifdef LUX_P10_R1_NATIVE
        facts.check(*session);
#endif
        const bool preserved = unchanged(s1, session->describe()) && bytes == take(take(session->read()).encode());
        const bool rejected = stale(*view);
        std::fprintf(
            stderr,
            "P10 R1 material-canvas: actual GraphCanvas signal -> queued BEGIN; source_and_history_preserved=%d "
            "stale=%d\n",
            preserved,
            rejected
        );
        assert(view->cancelEdit());
        assert(f.desktop->views().close(mounted));
        f.wait([&] { return !f.desktop->views().describe(mounted); });
        auto permit = take(f.store.prepareClose(session->describe().current));
        assert(f.store.close(permit));
        return preserved && rejected;
    }
}
