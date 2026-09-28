#include <lux/engine/editor/flowforge/FlowSessionAccess.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/meta/Meta.hpp>
#include "../src/PreparedFlowReload.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::flowforge;
    namespace flow = lux::flowforge;
    namespace access = lux::editor::flowforge::detail;
    template <class T> auto take(T result)
    {
        if (!result)
        {
            if constexpr (requires { result.error().code; })
                std::fprintf(stderr, "unexpected error %u\n", unsigned(result.error().code));
            if constexpr (requires { result.error().history; })
                std::fprintf(
                    stderr,
                    "history %u domain %llu\n",
                    unsigned(result.error().history.code),
                    result.error().history.domain_code
                );
            std::abort();
        }
        return std::move(*result);
    }
    asset::AssetId rootId()
    {
        return asset::AssetId{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    }
    FlowAuthoringSource source()
    {
        FlowAuthoringSource result{rootId(), "flow source", {}};
        auto event = result.graph.addNodes(std::make_unique<flow::OnEventNode>("tick"));
        assert(result.graph.addExport({flow::FlowForgeExportNodeId{1}, result.graph.getNode(event).node->id(), 1234}));
        (void)result.graph.addNodes(std::make_unique<flow::BranchNode>());
        (void)result.graph.addNodes(std::make_unique<flow::FuncDefNode>(
            "function",
            std::vector<flow::FuncArgInfo>{{&meta::ref_type_of_v<bool>, "condition"}},
            std::vector<flow::FuncArgInfo>{{&meta::ref_type_of_v<bool>, "result"}}
        ));
        return result;
    }
    struct Fixture final
    {
        sessions::SessionStore store{8};
        FlowSession* session{};
        sessions::SessionId id;
        Fixture(
            FlowAuthoringSource input = source(),
            flow::FlowSourceEnvironment environment = {},
            bool bound = true,
            FlowSessionLimits limits = {}
        )
        {
            auto reservation =
                take(store.reserve<FlowSession>({"lux.editor.flowforge"}, contracts::CodeLease::builtin()));
            id = reservation.id();
            auto candidate = take(FlowSession::create(
                id,
                bound ? sessions::SourceBinding{sessions::BoundSource{input.id, "flow.luxflow"}} : std::nullopt,
                std::move(input),
                std::move(environment),
                limits
            ));
            session = candidate.get();
            assert(store.prepare(reservation, candidate));
            assert(store.publish(reservation));
        }
        FlowEditBatch batch() const
        {
            return {session->describe().current, "flow edit", {}};
        }
        std::string encoded() const
        {
            return take(take(session->read()).encode());
        }
        editing::HistorySnapshot history() const
        {
            return take(access::FlowSessionAccess::data(*session).history->view()).snapshot;
        }
        flow::FlowSourceNode node(flow::ENodeOperation op) const
        {
            const auto snapshot = take(session->capture());
            for (const auto& node : snapshot.source().nodes)
                if (node.operation == op)
                    return node;
            std::abort();
        }
        FlowEditReceipt apply(VFlowEdit value)
        {
            auto edits = batch();
            edits.edits.push_back(std::move(value));
            return take(session->apply(std::move(edits)));
        }
    };
    struct Saved final
    {
        std::string bytes;
        sessions::SessionInfo info;
        editing::HistorySnapshot history;
        sessions::BindingRevision binding;
        std::optional<sessions::PersistedState> persisted;
        explicit Saved(const Fixture& f) : bytes(f.encoded()), info(f.session->describe()), history(f.history())
        {
            const auto& state = access::FlowSessionAccess::data(*f.session).state;
            binding = state.bindingRevision();
            persisted = state.checkpoint().persisted();
        }
        void unchanged(const Fixture& f) const
        {
            const auto now = f.session->describe();
            const auto h = f.history();
            assert(bytes == f.encoded());
            assert(info.current == now.current && info.observed == now.observed && info.dirty == now.dirty);
            assert(info.binding == now.binding && info.admission == now.admission);
            assert(history.history == h.history && history.current == h.current && history.revision == h.revision);
            assert(
                history.event_sequence == h.event_sequence && history.entry_count == h.entry_count &&
                history.cursor == h.cursor
            );
            assert(history.charged_retained_bytes == h.charged_retained_bytes && history.closed == h.closed);
            const auto& state = access::FlowSessionAccess::data(*f.session).state;
            assert(binding == state.bindingRevision() && persisted == state.checkpoint().persisted());
        }
    };
    flow::PinId dataInput(const flow::FlowSourceNode& node)
    {
        for (const auto& pin : node.inputs)
            if (pin.kind == flow::EPinKind::DATA_IN)
                return pin.id;
        std::abort();
    }
    flow::PinId dataOutput(const flow::FlowSourceNode& node)
    {
        for (const auto& pin : node.outputs)
            if (pin.kind == flow::EPinKind::DATA_OUT)
                return pin.id;
        std::abort();
    }
    flow::FlowSourceLiteral boolean(bool value)
    {
        return {flow::EFlowLiteralKind::BOOLEAN, value ? "true" : "false"};
    }
    void content()
    {
        Fixture f;
        Saved original(f);
        const auto branch = f.node(flow::ENodeOperation::BRANCH);
        const auto definition = f.node(flow::ENodeOperation::FUNC_DEF_START);
        auto batch = f.batch();
        batch.edits.push_back(FlowRename{"edited flow"});
        batch.edits.push_back(FlowSetLiteral{dataInput(branch), boolean(true)});
        batch.edits.push_back(FlowAddVariable{"enabled", "bool", boolean(true)});
        batch.edits.push_back(FlowMoveNodes{{{branch.id, {42, 24}}}});
        batch.edits.push_back(FlowInsertFunctionUse{definition.id, false, {4, 5}});
        batch.edits.push_back(FlowInsertFunctionUse{definition.id, true, {6, 7}});
        auto changed = take(f.session->apply(std::move(batch)));
        assert(changed.inserted.nodes.size() == 2 && changed.inserted.variables.size() == 1);
        assert(f.history().entry_count == 1 && f.session->describe().dirty);
        auto frozen = take(f.session->capture());
        assert(frozen.source().variables.front().name == "enabled");
        const auto call = f.node(flow::ENodeOperation::GRAPH_FUNC_CALL);
        f.apply(FlowConnect{dataOutput(definition), dataInput(call)});
        const auto connected = f.encoded();
        Saved no_change(f);
        assert(f.apply(FlowConnect{dataOutput(definition), dataInput(call)}).effect == editing::EEditEffect::NO_CHANGE);
        no_change.unchanged(f);
        f.apply(FlowDisconnect{dataOutput(definition), dataInput(call)});
        assert(f.session->undo());
        assert(f.encoded() == connected);
        assert(f.session->undo());
        assert(f.session->undo());
        assert(f.encoded() == original.bytes && !f.session->describe().dirty);
        assert(f.session->redo());
        assert(f.session->redo());
        assert(f.encoded() == connected);
        assert(frozen.source().links.empty() && frozen.source().name == "edited flow");
        std::vector<flow::ExportMethodNode> exports{
            {flow::FlowForgeExportNodeId{1}, f.node(flow::ENodeOperation::ON_EVENT).id, 5678}
        };
        f.apply(FlowSetExports{std::move(exports)});
        assert(take(f.session->capture()).source().exports.front().symbol == 5678);
        assert(f.session->undo());
        assert(f.encoded() == connected);
        // Branch after undo cannot recycle a variable identity issued by an atomic batch.
        assert(f.session->undo());
        assert(f.session->undo());
        auto next = f.apply(FlowAddVariable{"later", "bool", boolean(false)}).inserted.variables.front();
        assert(next > changed.inserted.variables.front());
        Saved before_failure(f);
        auto bad = f.batch();
        bad.edits.push_back(FlowRename{"not published"});
        bad.edits.push_back(FlowRemoveVariable{999});
        assert(!f.session->apply(std::move(bad)));
        before_failure.unchanged(f);
        assert(!f.session->capture({1}));
        before_failure.unchanged(f);
        std::puts("X04-01: actual Flow graph/variables/signatures/exports/links/layout/history without linker PASS");
    }
    void failure()
    {
        Fixture f;
        const auto variable = f.apply(FlowAddVariable{"condition", "bool", boolean(true)}).inserted.variables.front();
        f.apply(FlowInsertNode{
            contracts::CodeLease::builtin(),
            std::make_unique<flow::GetVariableNode>(
                variable,
                flow::DataPinInfo{"condition", &meta::ref_type_of_v<bool>}
            )
        });
        Saved referenced(f);
        auto batch = f.batch();
        batch.edits.push_back(FlowRemoveVariable{variable});
        assert(!f.session->apply(std::move(batch)));
        referenced.unchanged(f);
        batch = f.batch();
        batch.edits.push_back(FlowSetVariable{{variable, "condition", "double", {flow::EFlowLiteralKind::REAL, "2"}}});
        assert(!f.session->apply(std::move(batch)));
        referenced.unchanged(f);
        const auto def = f.node(flow::ENodeOperation::FUNC_DEF_START);
        f.apply(FlowInsertFunctionUse{def.id, false, {}});
        f.apply(FlowInsertFunctionUse{def.id, true, {}});
        const auto call = f.node(flow::ENodeOperation::GRAPH_FUNC_CALL);
        f.apply(FlowConnect{dataOutput(def), dataInput(call)});
        Saved signature(f);
        batch = f.batch();
        batch.edits.push_back(FlowSetSignature{def.id, "broken", {}});
        assert(!f.session->apply(std::move(batch)));
        signature.unchanged(f);
        auto layout = f.node(flow::ENodeOperation::ON_EVENT).id;
        batch = f.batch();
        batch.edits.push_back(FlowRemoveNodes{{layout}, {}});
        assert(!f.session->apply(std::move(batch)));
        signature.unchanged(f);
        auto value = std::get<flow::FlowSourceSignature>(def.parameters);
        value.arguments.front().name = "newName";
        f.apply(FlowSetSignature{def.id, "updated", value});
        assert(dataInput(f.node(flow::ENodeOperation::GRAPH_FUNC_CALL)) == dataInput(call));
        assert(f.session->undo());
        assert(f.encoded() == signature.bytes);
        std::puts("X04-02: referenced variable/signature/exports reject with entire state unchanged PASS");
    }
    void mixedSignature()
    {
        Fixture f;
        const auto def = f.node(flow::ENodeOperation::FUNC_DEF_START);
        f.apply(FlowInsertFunctionUse{def.id, false, {}});
        const auto call = f.node(flow::ENodeOperation::GRAPH_FUNC_CALL);
        const auto pin = dataInput(call);
        Saved prior(f);
        const auto before_entries = f.history().entry_count;
        auto batch = f.batch();
        batch.edits.push_back(FlowSetLiteral{pin, boolean(true)});
        auto signature = std::get<flow::FlowSourceSignature>(def.parameters);
        signature.arguments.front().name = "replaced";
        batch.edits.push_back(FlowSetSignature{def.id, "replacement", signature});
        batch.edits.push_back(FlowSetLiteral{pin, boolean(false)});
        assert(f.session->apply(std::move(batch)));
        assert(f.history().entry_count == before_entries + 1);
        auto now = f.node(flow::ENodeOperation::GRAPH_FUNC_CALL);
        for (const auto& input : now.inputs)
            if (input.id == pin)
                assert(input.literal == boolean(false));
        const auto after = f.encoded();
        assert(f.session->undo());
        assert(f.encoded() == prior.bytes);
        assert(f.session->redo());
        assert(f.encoded() == after);
        std::puts("ordered literal/signature replacement/literal and atomic replay PASS");
    }

    void mixedRecreate()
    {
        Fixture f;
        const auto branch = f.node(flow::ENodeOperation::BRANCH);
        const auto pin = dataInput(branch);
        Saved saved(f);
        auto replacement = std::make_unique<flow::BranchNode>(branch.id.value);
        for (std::size_t i = 0; i < replacement->inPins().size(); ++i)
            assert(flow::FlowGraph::assignDetachedPinId(*replacement->inPins()[i], branch.inputs[i].id));
        for (std::size_t i = 0; i < replacement->outPins().size(); ++i)
            assert(flow::FlowGraph::assignDetachedPinId(*replacement->outPins()[i], branch.outputs[i].id));
        auto* input = static_cast<flow::DataInPin*>(replacement->inPins().back());
        assert(input->kind() == flow::EPinKind::DATA_IN);
        assert(input->setConstantData(take(flow::materializeFlowLiteral(boolean(false), *input->info().type))));
        auto batch = f.batch();
        batch.edits.push_back(FlowSetLiteral{pin, boolean(true)});
        batch.edits.push_back(FlowRemoveNodes{{branch.id}, {}});
        batch.edits.push_back(FlowInsertNode{contracts::CodeLease::builtin(), std::move(replacement), {9, 10}, true});
        assert(f.session->apply(std::move(batch)));
        const auto node = f.node(flow::ENodeOperation::BRANCH);
        for (const auto& value : node.inputs)
            if (value.id == pin)
                assert(value.literal == boolean(false));
        assert(f.history().entry_count == 1);
        const auto after = f.encoded();
        assert(f.session->undo());
        assert(f.encoded() == saved.bytes);
        assert(f.session->redo());
        assert(f.encoded() == after);
        std::puts("ordered literal/delete/recreate same IDs: old scratch never overwrites new payload PASS");
    }
    struct Lifetime final
    {
        int alive{}, destroyed{}, callbacks{};
        bool released{}, premature{};
    };
    struct NativeRecord final
    {
        std::int32_t value{};
    };
    struct Metadata final
    {
        Lifetime& stats;
        meta::RefClass record;
        meta::RefFunction function;
        std::array<const meta::RefClass*, 1> classes;
        std::array<const meta::RefFunction*, 1> functions;
        explicit Metadata(Lifetime& value) : stats(value)
        {
            record.name = "NativeRecord";
            record.full_name = "NativeRecord";
            record.type = meta::ref_type_of_v<NativeRecord>;
            record.type.name = record.full_name;
            record.type.ptr = &record;
            record.fields.push_back({"value", meta::ref_type_of_v<std::int32_t>});
            function.invokable.name = "nativeProbe";
            function.invokable.full_name = "nativeProbe";
            function.invokable.type_signature = "int32_t()";
            function.invokable.return_type = meta::ref_type_of_v<std::int32_t>;
            classes = {&record};
            functions = {&function};
        }
        ~Metadata()
        {
            stats.premature = stats.alive != 0;
            stats.released = true;
        }
    };
    flow::FlowSourceEnvironment environment(const std::shared_ptr<Metadata>& owner)
    {
        flow::FlowSourceEnvironment result;
        result.classes = owner->classes;
        result.functions = owner->functions;
        result.code_lifetime = owner;
        return result;
    }
    class ProbeNode final : public flow::BranchNode
    {
    public:
        ProbeNode(Lifetime& stats, std::weak_ptr<Metadata> metadata, std::function<void()> callback = {})
            : stats_(stats), metadata_(std::move(metadata)), callback_(std::move(callback))
        {
            ++stats_.alive;
        }
        ~ProbeNode() override
        {
            assert(!metadata_.expired());
            auto owner = metadata_.lock();
            assert(owner->classes.front()->fields.front().name == "value");
            if (callback_)
            {
                ++stats_.callbacks;
                callback_();
            }
            --stats_.alive;
            ++stats_.destroyed;
        }

    private:
        Lifetime& stats_;
        std::weak_ptr<Metadata> metadata_;
        std::function<void()> callback_;
    };
    void lifetime()
    {
        Lifetime stats;
        auto owner = std::make_shared<Metadata>(stats);
        std::weak_ptr<Metadata> weak = owner;
        auto input = source();
        (void)input.graph.addNodes(std::make_unique<flow::GetFieldNode>(owner->record, owner->record.fields.front()));
        (void)input.graph.addNodes(std::make_unique<flow::NativeFuncCall>(owner->function));
        auto fixture = std::make_unique<Fixture>(std::move(input), environment(owner));
        const auto variable =
            fixture->apply(FlowAddVariable{"native", "NativeRecord", {flow::EFlowLiteralKind::ZERO, {}}}
            ).inserted.variables.front();
        fixture->apply(FlowSetVariable{{variable, "renamed native", "NativeRecord", {flow::EFlowLiteralKind::ZERO, {}}}}
        );
        fixture->apply(FlowInsertNode{contracts::CodeLease::plugin(owner), std::make_unique<ProbeNode>(stats, owner)});
        auto frozen = take(fixture->session->capture());
        owner.reset();
        assert(!weak.expired());
        assert(fixture->session->undo()); // custom node parked in history, metadata arrays have no external owner
        assert(stats.alive == 1);
        assert(fixture->session->redo());
        assert(fixture->session->undo());
        fixture.reset();
        assert(stats.alive == 0 && stats.destroyed == 1 && stats.released && !stats.premature);
        assert(weak.expired()); // frozen data contains only owned values, safely usable after code/metadata release
        assert(flow::encodeFlowSource(frozen.source()));
        std::puts("X04-03: owned metadata spans/descriptors and last code owner outlive live/parked nodes PASS");
    }
    void reading()
    {
        Fixture f;
        Saved saved(f);
        auto view = take(f.session->read());
        const auto test = [&] {
            auto batch = f.batch();
            batch.edits.push_back(FlowRename{"reentrant"});
            auto result = f.session->apply(std::move(batch));
            assert(!result && result.error().session == sessions::ESessionError::BUSY);
            assert(!f.session->undo());
            assert(!f.session->capture());
            assert(!f.store.prepareClose(saved.info.current));
        };
        assert(view.withRead([&](const auto& source) -> FlowEditResult<void> {
            test();
            assert(source.name == "flow source");
            struct Cleanup
            {
                const std::function<void()>& action;
                ~Cleanup()
                {
                    action();
                }
            };
            std::function<void()> callback = test;
            Cleanup cleanup{callback};
            auto graph = take(flow::materializeFlowSource(source));
            assert(!graph.nodes().empty());
            return {};
        }));
        saved.unchanged(f);
        try
        {
            (void)view.withRead([&](const auto&) -> FlowEditResult<void> {
                struct Cleanup
                {
                    const std::function<void()>& action;
                    ~Cleanup()
                    {
                        action();
                    }
                };
                std::function<void()> callback = test;
                Cleanup cleanup{callback};
                throw std::runtime_error("foreign codec failure");
            });
            assert(false);
        }
        catch (const std::runtime_error&)
        {}
        saved.unchanged(f);
        f.apply(FlowRename{"after read"});
        std::puts("READING covers callback, temporary destruction and unwind; normal edit resumes PASS");
    }
    void inputCleanup(std::string_view scenario)
    {
        const bool external_owner = scenario == "reload-unbound-external";
        const bool bound = scenario != "reload-unbound" && !external_owner;
        Fixture f(source(), {}, bound);
        Saved saved(f);
        Lifetime stats;
        auto owner = std::make_shared<Metadata>(stats);
        bool blocked{};
        auto input = source();
        (void)input.graph.addNodes(std::make_unique<ProbeNode>(stats, owner, [&] {
            auto edit = f.batch();
            edit.edits.push_back(FlowRename{"bad reentry"});
            const auto attempted = f.session->apply(std::move(edit));
            blocked = !attempted && attempted.error().session == sessions::ESessionError::BUSY;
        }));
        auto env = environment(owner);
        if (!external_owner)
            owner.reset();
        const auto reject = [&]() -> FlowEditResult<void> {
            auto prepared = access::PreparedFlowReload::prepare(*f.session, std::move(input), std::move(env));
            assert(!prepared);
            return {};
        };
        if (scenario == "reload-reading")
        {
            assert(take(f.session->read()).withRead([&](const auto&) -> FlowEditResult<void> {
                assert(reject());
                assert(f.session->describe().admission == sessions::EEditAdmission::READING);
                return {};
            }));
        }
        else if (scenario == "reload-closing")
        {
            auto permit = take(f.store.prepareClose(saved.info.current));
            assert(reject());
            assert(f.session->describe().admission == sessions::EEditAdmission::CLOSING);
        }
        else
        {
            if (scenario == "reload-identity")
                input.id = asset::AssetId{};
            assert(reject());
        }
        assert(blocked && stats.destroyed == 1 && stats.callbacks == 1 && stats.alive == 0);
        if (external_owner)
        {
            assert(!stats.released);
            owner.reset();
        }
        assert(stats.released && !stats.premature);
        saved.unchanged(f);
        f.apply(FlowRename{"normal after rejection"});
        std::puts("owned reload input: last lease, destruction admission, full unchanged state PASS");
    }

    void editInputCleanup(std::string_view scenario)
    {
        Fixture f;
        Saved saved(f);
        Lifetime stats;
        auto owner = std::make_shared<Metadata>(stats);
        bool blocked{};
        auto input = std::make_unique<ProbeNode>(stats, owner, [&] {
            auto nested = f.batch();
            nested.edits.push_back(FlowRename{"reentrant input"});
            const auto result = f.session->apply(std::move(nested));
            blocked = !result && result.error().session == sessions::ESessionError::BUSY;
        });
        auto batch = f.batch();
        batch.edits.push_back(FlowInsertNode{contracts::CodeLease::plugin(owner), std::move(input)});
        owner.reset();
        const auto reject = [&]() -> FlowEditResult<void> {
            const auto result = f.session->apply(std::move(batch));
            assert(!result);
            return {};
        };
        if (scenario == "input-reading")
            assert(take(f.session->read()).withRead([&](const auto&) -> FlowEditResult<void> { return reject(); }));
        else if (scenario == "input-closing")
        {
            auto permit = take(f.store.prepareClose(saved.info.current));
            assert(reject());
            assert(f.session->describe().admission == sessions::EEditAdmission::CLOSING);
        }
        else
        {
            batch.expected.state.serial += 100;
            assert(reject());
        }
        assert(blocked && stats.destroyed == 1 && stats.alive == 0 && stats.released && !stats.premature);
        saved.unchanged(f);
        f.apply(FlowRename{"after rejected input"});
        std::puts("consumed edit input cleanup stays gated, last code owner releases after node PASS");
    }
    void budgets()
    {
        Fixture f;
        Saved before(f);
        auto bad = f.batch();
        bad.edits.push_back(FlowSetLiteral{
            dataInput(f.node(flow::ENodeOperation::BRANCH)),
            {flow::EFlowLiteralKind::BOOLEAN, "not-a-bool"}
        });
        assert(!f.session->apply(std::move(bad)));
        before.unchanged(f);
        FlowSessionLimits limits;
        limits.history.max_staging_bytes = 1;
        Fixture tiny(source(), {}, true, limits);
        Saved saved(tiny);
        auto batch = tiny.batch();
        batch.edits.push_back(FlowRename{"cannot fit"});
        auto result = tiny.session->apply(std::move(batch));
        assert(!result && result.error().history.code == editing::EEditError::STAGING_LIMIT);
        saved.unchanged(tiny);
        batch = tiny.batch();
        batch.edits.push_back(FlowRename{"atomic rejection"});
        batch.edits.push_back(FlowAddVariable{"flag", "bool", boolean(true)});
        assert(!tiny.session->apply(std::move(batch)));
        saved.unchanged(tiny);
        std::puts("invalid literal/local and structural staging limits preserve full state PASS");
    }
    void reloadSuccess()
    {
        Fixture f;
        Saved before(f);
        auto replacement = source();
        replacement.name = "reloaded";
        auto prepared = take(access::PreparedFlowReload::prepare(*f.session, std::move(replacement)));
        before.unchanged(f);
        assert(prepared.adopt(*f.session));
        assert(!f.session->describe().dirty && f.history().entry_count == 0);
        assert(take(f.session->capture()).source().name == "reloaded");
        prepared = take(access::PreparedFlowReload::prepare(*f.session, source()));
        f.apply(FlowRename{"newer"});
        Saved newer(f);
        assert(!prepared.adopt(*f.session));
        newer.unchanged(f);
        std::puts("reload success and stale adoption PASS");
    }
}
int main(int argc, char** argv)
{
    const std::string_view name = argc > 1 ? argv[1] : "content";
    if (name == "content")
        content();
    else if (name == "failure")
        failure();
    else if (name == "mixed-signature")
        mixedSignature();
    else if (name == "mixed-recreate")
        mixedRecreate();
    else if (name == "lifetime")
        lifetime();
    else if (name == "reading")
        reading();
    else if (name == "reload-success")
        reloadSuccess();
    else if (name == "budgets")
        budgets();
    else if (name.starts_with("input-"))
        editInputCleanup(name);
    else if (name.starts_with("reload-"))
        inputCleanup(name);
    else
        return 2;
}
