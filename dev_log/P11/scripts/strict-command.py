from pathlib import Path
p=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11/editor/tests/integration/session_factories/installation.cpp')
s=p.read_text();pos=s.index('    void installationStages(')
s=s[:pos]+'''    void fixedCommandPayload(SessionStore& store, em::MaterialSession& model, lux::material::NodeId node)
    {
        using namespace commands;
        struct Selection final { std::vector<lux::material::NodeId> nodes; };
        CommandRegistry registry;
        CommandDispatcher dispatcher{registry};
        auto action = std::make_shared<CommandEntry>(
            contracts::CodeLease::builtin(),
            CommandDescriptor{CommandId{"test.delete"}, "Delete", "Edit", "", ECommandScope::SESSION,
                              1, cxx::typeToken<Selection>()},
            [&store](const CommandQuery& input) -> CommandResult<CommandState> {
                const auto& target = std::get<SessionTarget>(input.target);
                auto state = store.describe(target.id);
                if (!state)
                    return cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET, "session"});
                if (state->admission != EEditAdmission::AVAILABLE)
                    return cxx::unexpected(CommandFailure{ECommandError::BUSY, "session"});
                if (target.based_on && *target.based_on != state->current)
                    return cxx::unexpected(CommandFailure{ECommandError::STALE_CONTENT, "material"});
                return CommandState{true};
            },
            [&model](const CommandInvocation& input) -> CommandResult<DispatchReceipt> {
                const auto& target = std::get<SessionTarget>(input.target());
                em::MaterialEditBatch batch{target.based_on.value_or(model.describe().current), "delete", {}};
                for (auto id : static_cast<const Selection*>(input.arguments().data())->nodes)
                    batch.edits.emplace_back(em::MaterialEraseNode{id});
                auto result = model.apply(std::move(batch));
                if (!result)
                    return cxx::unexpected(CommandFailure{ECommandError::DOMAIN_FAILURE, "material",
                                                         static_cast<std::uint64_t>(result.error().code)});
                return DispatchReceipt{ImmediateCompletion{}};
            });
        auto catalog = take(CommandRegistrySnapshot::create({action}));
        assert(registry.publish(catalog));
        auto handle = take(catalog.find(CommandIdView{"test.delete"}));
        std::vector<lux::material::NodeId> selection{node};
        const auto original = model.describe().current;
        auto owned = std::make_shared<const Selection>(Selection{selection});
        CommandArguments args{contracts::CodeLease::builtin(), cxx::typeToken<Selection>(), owned};
        CommandInvocation strict{SessionTarget{model.describe().id, original}, args};
        assert(dispatcher.enqueue(handle, strict));
        owned.reset(); args = {}; strict = CommandInvocation{}; selection.clear();
        em::MaterialEditBatch later{original, "later author edit", {}};
        later.edits.emplace_back(em::MaterialRename{"later"});
        assert(model.apply(std::move(later)));
        const auto before = model.describe();
        const auto encoded = take(take(model.read()).encode());
        const auto history = take(model.historyView()).snapshot;
        auto refused = take(dispatcher.drain());
        assert(refused.size() == 1 && !refused[0].result);
        assert(refused[0].result.error().code == ECommandError::STALE_CONTENT);
        assert(encoded == take(take(model.read()).encode()));
        const auto after = model.describe();
        assert(after.current == before.current && after.observed == before.observed && after.dirty == before.dirty);
        assert(after.binding == before.binding && take(model.historyView()).snapshot.revision == history.revision);
        // An identity-only delete uses the captured set, never the current selection.
        owned = std::make_shared<const Selection>(Selection{{node}});
        CommandInvocation erase{SessionTarget{model.describe().id},
            CommandArguments{contracts::CodeLease::builtin(), cxx::typeToken<Selection>(), owned}};
        assert(dispatcher.enqueue(handle, erase));
        owned.reset(); erase = CommandInvocation{}; selection.push_back(lux::material::NodeId{});
        auto completed = take(dispatcher.drain());
        assert(completed.size() == 1 && completed[0].result);
        assert(take(model.capture()).source().graph.nodes().empty());
        assert(model.undo() && model.undo() && model.describe().current == original);
        std::cout << "PASS real model strict source conflict and owned original selection\\n";
    }
''' + s[pos:]
s=s.replace('assert(material.graph.addNode(std::make_unique<lux::material::ConstantNode>()).valid());','const auto original_node = material.graph.addNode(std::make_unique<lux::material::ConstantNode>());\n    assert(original_node.valid());')
s=s.replace('assert(mat.apply(std::move(material_edit)));','assert(mat.apply(std::move(material_edit)));\n    fixedCommandPayload(store, mat, original_node);')
p.write_text(s)
