#include "ObjectQueue.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <source_location>
#include <lux/engine/editor/desktop/EditorContext.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/workspace/LayoutPlan.hpp>
#include <lux/engine/editor/workspace/RecoveryManifest.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
namespace w = lux::editor::workspace;
namespace v = lux::editor::views;
namespace e = lux::editor;
template <class T> auto take(T result, std::source_location location = std::source_location::current())
{
    if (!result)
    {
        std::fprintf(stderr, "Unexpected failure at %s:%u\n", location.file_name(), location.line());
        if constexpr (requires { result.error().detail; })
            std::fprintf(stderr, "%s\n", result.error().detail.c_str());
    }
    assert(result);
    return std::move(*result);
}
namespace
{
    namespace d = e::desktop;
    namespace s = lux::services;
    using Session = e::material::MaterialSession;
    class MaterialWindow final : public lux::ui::Pane
    {
    public:
        MaterialWindow(const d::UiCreateInfo& input, std::shared_ptr<Session> model)
            : Pane(input.dispatcher, input.instance, lux::ui::PaneTypeId{"test.material"}, "Material"),
              model_(std::move(model)), state_(input.configuration.bytes)
        {
        }
        [[nodiscard]] const std::shared_ptr<Session>& model() const noexcept
        {
            return model_;
        }
        [[nodiscard]] const std::vector<std::byte>& state() const noexcept
        {
            return state_;
        }

    private:
        std::shared_ptr<Session> model_;
        std::vector<std::byte> state_;
    };
    constexpr std::array dependencies{s::ServiceDependency{
        s::ServiceNameView{"test.sessions"},
        1,
        lux::cxx::typeToken<e::sessions::SessionStore>(),
        s::EDependencyKind::BORROWED
    }};
    d::UiResult<std::unique_ptr<lux::ui::Pane>> materialWindow(
        s::ServiceResolver& resolver,
        const d::UiCreateInfo& input
    )
    {
        auto store = resolver.require<e::sessions::SessionStore>(0);
        assert(store);
        if (!input.content.primary)
        {
            return lux::cxx::unexpected(d::UiFailure{d::EUiError::INVALID_CONFIGURATION, "test.binding"});
        }
        auto key = store->get().key<Session>(*input.content.primary);
        if (!key)
        {
            return lux::cxx::unexpected(
                d::UiFailure{d::EUiError::DEPENDENCY, "session", static_cast<std::uint64_t>(key.error()), {}}
            );
        }
        auto model = store->get().share(*key);
        if (!model)
        {
            return lux::cxx::unexpected(
                d::UiFailure{d::EUiError::DEPENDENCY, "session", static_cast<std::uint64_t>(model.error()), {}}
            );
        }
        return std::make_unique<MaterialWindow>(input, std::move(*model));
    }
    constexpr d::UiDescriptor
        window_definition{v::ViewTypeIdView{"test.material"}, "Material", dependencies, 1, nullptr, materialWindow};

    void configurationIdentity(lux::object::ObjectMessageQueue& messages)
    {
        const auto asset = lux::asset::AssetId{*uuids::uuid::from_string("a2345678-1234-1234-1234-123456789abc")};
        lux::material::MaterialSource initial{asset, "Persisted material", {}};
        assert(initial.graph.addNode(std::make_unique<lux::material::ConstantNode>()).valid());
        const auto source_bytes = take(lux::material::encodeMaterialSource(initial));
        w::DockLayout layout;
        layout.id = {"a2345678123412341234123456789abc"};
        layout.label = "Shared material, independent windows";
        layout.slots = {
            {{1}, v::ViewRestoreKey{"left"}, v::ViewTypeId{"test.material"}, true, {1, {std::byte{11}}}},
            {{2}, v::ViewRestoreKey{"right"}, v::ViewTypeId{"test.material"}, true, {1, {std::byte{29}}}},
            {{3},
             v::ViewRestoreKey{"absent"},
             v::ViewTypeId{"absent.plugin"},
             false,
             {7, {std::byte{0}, std::byte{255}, std::byte{3}}}},
            {{4}, v::ViewRestoreKey{"future"}, v::ViewTypeId{"test.material"}, true, {99, {std::byte{8}, std::byte{0}}}}
        };
        layout.dock.nodes = {{1, w::EDockSplit::LEAF, 0, 0, .5, {{1}, {2}, {3}, {4}}}};
        layout.dock.roots = {{1}};
        layout.opaque.push_back({"absent.layout", 3, {std::byte{2}, std::byte{0}}});
        w::RecoveryManifest recovery;
        for (std::size_t i{}; i < 2; ++i)
        {
            recovery.entries.push_back(
                {layout.slots[i].restore_key,
                 layout.slots[i].type,
                 {{"asset://a2345678-1234-1234-1234-123456789abc", false}},
                 0}
            );
        }
        recovery.opaque.push_back({"absent.recovery", 5, {std::byte{9}, std::byte{0}}});
        w::UserPreferences preferences;
        preferences.opaque.push_back({"absent.settings", 17, {std::byte{5}, std::byte{0}, std::byte{255}}});
        const auto layout_bytes = take(w::encodeLayout(layout));
        const auto recovery_bytes = take(w::encodeRecovery(recovery));
        const auto preference_bytes = take(w::encodePreferences(preferences));
        const std::array providers{w::ViewProviderInfo{v::ViewTypeId{"test.material"}, 1, 1}};
        e::sessions::SessionId old_session;
        std::optional<d::UiHandle> old_factory;
        lux::object::Connection old_connection;
        unsigned old_calls{};

        // Recreate every live owner from the same existing persistent codecs. No runtime identity
        // is serialized and no layout opaque field authorizes opening a source.
        for (unsigned launch{}; launch < 2; ++launch)
        {
            auto restored_layout = take(w::decodeLayout(layout_bytes));
            auto restored_recovery = take(w::decodeRecovery(recovery_bytes));
            auto restored_preferences = take(w::decodePreferences(preference_bytes));
            auto plan =
                take(w::LayoutPlanner::resolve(take(w::ValidatedLayout::validate(restored_layout)), {}, providers));
            assert(plan.views[2].resolution == w::ELayoutResolution::MISSING_PROVIDER);
            assert(plan.views[3].resolution == w::ELayoutResolution::UNSUPPORTED_STATE);
            assert(plan.layout.slots[2].state == layout.slots[2].state);
            assert(plan.layout.slots[3].state == layout.slots[3].state);
            assert(plan.layout.opaque == layout.opaque);
            assert(restored_recovery.entries.size() == 2 && restored_recovery.opaque == recovery.opaque);
            for (std::size_t i{}; i < 2; ++i)
            {
                const auto& binding = restored_recovery.entries[i];
                assert(binding.restore_key == restored_layout.slots[i].restore_key);
                assert(binding.type == restored_layout.slots[i].type && binding.primary == 0);
                assert(binding.contents[0].locator == recovery.entries[0].contents[0].locator);
            }

            e::sessions::SessionStore store{messages.dispatcherRef(), 1};
            auto reservation = take(store.reserve<Session>({"lux.editor.material"}, lux::object::CodeLease::builtin()));
            auto candidate = take(Session::create(
                reservation.id(),
                e::sessions::BoundSource{asset, restored_recovery.entries[0].contents[0].locator},
                take(lux::material::decodeMaterialSource(source_bytes))
            ));
            assert(store.prepare(reservation, candidate));
            const auto id = take(store.publish(reservation));
            auto shared = take(store.share(take(store.key<Session>(id))));
            const auto before = shared->describe();
            const auto encoded_before = take(take(shared->read()).encode());
            assert(id != old_session);
            d::EditorContext context{messages.dispatcherRef()};
            auto scope = take(context.services().createScope());
            assert(scope.provide(s::ServiceNameView{"test.sessions"}, store));
            auto catalog =
                take(d::UiCatalog::prepare({d::UiEntry::bind<window_definition>(lux::object::CodeLease::builtin())}));
            assert(context.ui().publish(catalog));
            auto factory = take(catalog.find(window_definition.type));
            auto root = take(lux::ui::Root::create(messages.dispatcherRef()));
            const v::ViewContent binding{{id}, id};
            std::vector<d::UiMountRequest> requests;
            for (std::size_t i{}; i < 2; ++i)
            {
                const auto& slot = restored_layout.slots[i];
                requests.push_back(
                    {factory,
                     {messages.dispatcherRef(), lux::ui::PaneId{slot.restore_key.name()}, binding, slot.state},
                     slot.visible}
                );
            }
            if (launch)
            {
                assert(!old_connection.connected());
                auto stale_factory = context.ui().create(*old_factory, scope, requests[0].input);
                assert(!stale_factory && stale_factory.error().code == d::EUiError::STALE_REGISTRATION);
                auto stale_input = requests[0].input;
                stale_input.content = {{old_session}, old_session};
                auto stale_binding = context.ui().create(factory, scope, stale_input);
                assert(!stale_binding && stale_binding.error().domain == "session");
                assert(
                    stale_binding.error().domain_code ==
                    static_cast<std::uint64_t>(e::sessions::ESessionError::WRONG_STORE)
                );
                assert(root->panes().empty() && store.size() == 1);
            }
            auto unknown = catalog.find(v::ViewTypeIdView{"absent.plugin"});
            assert(!unknown && unknown.error().code == d::EUiError::NOT_FOUND);
            auto future = requests[0].input;
            future.configuration = restored_layout.slots[3].state;
            auto unsupported = context.ui().create(factory, scope, future);
            assert(!unsupported && unsupported.error().code == d::EUiError::INVALID_CONFIGURATION);
            assert(root->panes().empty());
            assert(context.ui().mount(*root, scope, std::move(requests)));
            auto& left = static_cast<MaterialWindow&>(*root->findPane(lux::ui::PaneIdView{"left"}));
            auto& right = static_cast<MaterialWindow&>(*root->findPane(lux::ui::PaneIdView{"right"}));
            assert(left.model().get() == shared.get() && right.model().get() == shared.get());
            assert(!left.model().owner_before(right.model()) && !right.model().owner_before(left.model()));
            assert(left.state() == layout.slots[0].state.bytes && right.state() == layout.slots[1].state.bytes);
            const auto after = shared->describe();
            assert(after.current == before.current && after.observed == before.observed && after.dirty == before.dirty);
            assert(after.binding == before.binding && take(take(shared->read()).encode()) == encoded_before);
            auto edited_layout = plan.layout;
            edited_layout.label = "Renamed local layout";
            const auto saved = take(w::decodeLayout(take(w::encodeLayout(edited_layout))));
            assert(saved.id == layout.id && saved.slots[2].state == layout.slots[2].state);
            assert(saved.slots[3].state == layout.slots[3].state && saved.opaque == layout.opaque);
            restored_preferences.selected_layout = layout.id;
            assert(
                take(w::decodePreferences(take(w::encodePreferences(restored_preferences)))).opaque ==
                preferences.opaque
            );
            assert(take(w::encodeRecovery(restored_recovery)) == recovery_bytes);
            if (!launch)
            {
                old_connection = take(lux::object::LuxObject::connect(
                    &left,
                    &lux::ui::Pane::closeRequested,
                    [&old_calls]() noexcept { ++old_calls; }
                ));
            }
            else
            {
                left.requestClose();
            }
            assert(old_calls == 0);
            old_session = id;
            old_factory = factory;
            root.reset();
            assert(!old_connection.connected() && store.describe(id));
            auto permit = take(store.prepareClose(shared->describe().current));
            assert(store.close(permit));
            assert(shared->read().error().session == e::sessions::ESessionError::STALE_SESSION);
            shared.reset();
            assert(scope.release() && scope.drained() && context.services().drained());
            static_cast<void>(messages.collectRetired());
        }
        std::puts("EC4 persistent codecs + real shared MaterialSession + UiRegistry: new identities, separate local "
                  "state, unknown payload and no implicit open PASS");
    }
} // namespace
int main()
{
    auto messages = take(lux::object::ObjectMessageQueue::create(32));
    auto root = take(lux::ui::Root::create(messages.dispatcherRef(), {.docking = false}));
    lux::ui::Pane pane(messages.dispatcherRef(), lux::ui::PaneId{"material"}, lux::ui::PaneTypeId{"material"}, "dirty");
    auto mount = take(root->prepareMount(pane));
    assert(root->commit(mount));
    const auto revision = root->windowRevision();
    lux::test::ObjectQueue store_messages;
    e::sessions::SessionStore store{store_messages.dispatcherRef(), 4};
    auto reserved =
        take(store.reserve<e::material::MaterialSession>({"lux.editor.material"}, lux::object::CodeLease::builtin()));
    const auto session_id = reserved.id();
    auto asset = lux::asset::AssetId{*uuids::uuid::from_string("12345678-1234-1234-1234-123456789abc")};
    lux::material::MaterialSource source{asset, "original", {}};
    assert(source.graph.addNode(std::make_unique<lux::material::ConstantNode>()).valid());
    auto session = take(e::material::MaterialSession::create(
        session_id,
        e::sessions::BoundSource{asset, "test.material"},
        std::move(source)
    ));
    auto* model = session.get();
    assert(store.prepare(reserved, session));
    assert(store.publish(reserved));
    e::material::MaterialEditBatch batch{model->describe().current, "edit", {}};
    batch.edits.emplace_back(e::material::MaterialRename{"dirty source"});
    assert(model->apply(std::move(batch)));
    const auto before = model->describe();
    assert(before.dirty);
    const auto encoded = take(take(model->read()).encode());
    w::DockLayout layout;
    layout.id = {"1234567890abcdef1234567890abcdef"};
    layout.label = "layout";
    layout.slots = {
        {{1}, v::ViewRestoreKey{"exact"}, v::ViewTypeId{"material"}},
        {{2}, v::ViewRestoreKey{"new"}, v::ViewTypeId{"material"}},
        {{3}, v::ViewRestoreKey{"same-key"}, v::ViewTypeId{"scene"}}
    };
    layout.dock.nodes = {{1, w::EDockSplit::LEAF, 0, 0, .5, {{1}, {2}, {3}}}};
    layout.dock.roots = {{1}};
    const std::array existing{
        w::LayoutTarget{v::ViewRestoreKey{"other"}, v::ViewTypeId{"material"}},
        w::LayoutTarget{v::ViewRestoreKey{"exact"}, v::ViewTypeId{"material"}},
        w::LayoutTarget{v::ViewRestoreKey{"same-key"}, v::ViewTypeId{"material"}}
    };
    const std::array providers{
        w::ViewProviderInfo{v::ViewTypeId{"material"}},
        w::ViewProviderInfo{v::ViewTypeId{"scene"}}
    };
    auto bad = layout;
    bad.dock.nodes[0].first = 123;
    assert(!w::ValidatedLayout::validate(bad));
    std::vector<w::LayoutTarget> targets;
    for (const auto& view : existing)
        targets.push_back({view.restore_key, view.type});
    auto ambiguous = targets;
    ambiguous.push_back(targets.front());
    assert(!w::LayoutPlanner::resolve(take(w::ValidatedLayout::validate(layout)), ambiguous, providers));
    auto plan = take(w::LayoutPlanner::resolve(take(w::ValidatedLayout::validate(layout)), targets, providers));
    assert(plan.views[0].existing && *plan.views[0].existing == 1);
    assert(plan.views[1].resolution == w::ELayoutResolution::CREATE_UNBOUND);
    assert(plan.views[2].resolution == w::ELayoutResolution::CREATE_UNBOUND);
    assert(plan.retained.size() == 2);
    assert(plan.retained[0].restore_key == existing[0].restore_key && plan.retained[0].type == existing[0].type);
    assert(plan.retained[1].restore_key == existing[2].restore_key && plan.retained[1].type == existing[2].type);
    layout.slots.clear(); // Output has its own full value lifetime.
    assert(plan.layout.slots.size() == 3 && plan.views[0].slot.restore_key.name() == "exact");
    const auto after = model->describe();
    assert(after.current == before.current && after.observed == before.observed && after.dirty == before.dirty);
    assert(after.binding == before.binding && after.id == before.id);
    assert(take(take(model->read()).encode()) == encoded);
    assert(root->windowRevision() == revision && root->panes().size() == 1 && pane.visible());
    assert(model->undo() && model->redo()); // Same live History remains usable, not replaced.
    auto detach = take(root->prepareDetach(pane));
    assert(root->commit(detach));
    std::puts(
        "X09-01/X09-06 REAL Root + dirty MaterialSession unchanged; exact key/type, extras retained, zero open/rebind"
    );
    configurationIdentity(messages);
}
