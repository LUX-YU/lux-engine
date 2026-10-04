#include "../../test-support/ObjectQueue.hpp"
#include <consumer/Domain.hpp>
#include <consumer/Gui.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/editor/scene/InspectorView.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/Root.hpp>
#include "../common/ControlsTestAccess.hpp"
#include <cassert>
#include <cstdio>
#include <source_location>
#include <imgui.h>
#include <lux/engine/ui/Table.hpp>

namespace
{
    using namespace lux;
    using namespace lux::editor;
    namespace author = lux::editor::scene;
    template <class T> auto take(T result, std::source_location location = std::source_location::current())
    {
        if (!result)
            std::fprintf(stderr, "Inspector operation failed at %s:%u\n", location.file_name(), location.line());
        assert(result);
        return std::move(*result);
    }
    void checkCompletedGesture(author::InspectorFields& interaction, author::SceneSession& session)
    {
        using consumer::Component;
        using namespace lux::editor;
        // A separate ImGui context injects IO events; this is not physical desktop evidence.
        struct Context final
        {
            ImGuiContext* previous{ImGui::GetCurrentContext()};
            ImGuiContext* current{ImGui::CreateContext()};
            ~Context()
            {
                ImGui::DestroyContext(current);
                ImGui::SetCurrentContext(previous);
            }
        } context;
        ImGui::SetCurrentContext(context.current);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {640, 480};
        io.DeltaTime = 1.0F / 60;
        io.MouseDoubleClickTime = 0; // Each scripted press is a separate drag, not a text-entry double click.
        unsigned char* pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        assert(interaction.refresh());
        const auto object = interaction.target();
        const auto before = session.historyView()->snapshot;
        const auto access = [](auto& value) noexcept { return &value.settings.gain; };
        const auto read = [&]()
        {
            return static_cast<const Component*>(interaction.component(object, lux::cxx::typeToken<Component>()))
                ->settings.gain;
        };
        const auto original = read();
        ImVec2 center{};
        const auto draw = [&](bool visible)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({600, 400});
            ImGui::Begin("Gesture regression");
            {
                auto table = lux::ui::TableScope({lux::ui::ElementIdView{"fields"}, 2});
                assert(table.visible());
                if (visible)
                {
                    lux::ui::propertyRow("Gain");
                    double candidate = read();
                    const bool changed = ImGui::DragScalar("##value", ImGuiDataType_Double, &candidate, 0.1F);
                    const auto minimum = ImGui::GetItemRectMin();
                    const auto maximum = ImGui::GetItemRectMax();
                    center = {(minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2};
                    assert((interaction.apply<Component, double>(
                        object,
                        "settings.gain",
                        "Gain",
                        access,
                        candidate,
                        lux::ui::EditResult{changed, ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), false}
                    )));
                }
            }
            if (!visible)
                assert(interaction.finish());
            ImGui::End();
            ImGui::Render();
            assert(interaction.update());
        };
        draw(true);
        draw(true);
        io.AddMousePosEvent(center.x, center.y);
        draw(true);
        io.AddMouseButtonEvent(0, true);
        draw(true);
        assert(!interaction.active());
        io.AddMousePosEvent(center.x + 40, center.y);
        draw(true);
        assert(read() != original && session.historyView()->snapshot.current == before.current);
        // Release ends the edit even if the widget is omitted on that frame.
        io.AddMouseButtonEvent(0, false);
        draw(false);
        draw(false);
        draw(false);
        std::printf(
            "Completed gesture: active=%d cursor=%zu before=%zu original=%.3f value=%.3f\n",
            interaction.active(),
            session.historyView()->snapshot.cursor,
            before.cursor,
            original,
            read()
        );
        std::fflush(stdout);
        assert(!interaction.active());
        assert(session.historyView()->snapshot.cursor == before.cursor + 1);
        draw(true);
        assert(session.historyView()->snapshot.cursor == before.cursor + 1);
        assert(session.undo());
        assert(interaction.refresh());
        assert(read() == original);
        assert(session.historyView()->snapshot.current == before.current);

        const auto no_change = session.historyView()->snapshot;
        io.AddMousePosEvent(center.x, center.y);
        draw(true);
        io.AddMouseButtonEvent(0, true);
        draw(true);
        assert(!interaction.active());
        io.AddMouseButtonEvent(0, false);
        draw(true);
        assert(!interaction.active() && read() == original);
        assert(session.historyView()->snapshot.current == no_change.current);
        assert(session.historyView()->snapshot.revision == no_change.revision);

        std::puts("PASS injected ImGui completed edit: release commits once; omitted widget cannot strand a "
                  "completed field edit; redraw does not duplicate history; unchanged click preserves revision; Undo "
                  "restores");
    }
} // namespace
void consumer::checkInspector()
{
    auto queue = take(object::ObjectMessageQueue::create(128));
    auto root = take(ui::Root::create(queue.dispatcherRef(), {.docking = false}));
    lux::test::ObjectQueue store_messages;
    sessions::SessionStore store{store_messages.dispatcherRef(), 4};
    desktop::ViewHost host(*root);
    const auto generated = consumer::schemas();
    std::vector<simulation::ecs::ComponentSchema> copies;
    for (const auto& schema : generated)
        if (schema.snapshot == simulation::ecs::EComponentSnapshotPolicy::COPY)
            copies.push_back(schema);
    auto schemas = take(simulation::ecs::ComponentSchemaSet::build(std::move(copies)));
    const auto uuid = *uuids::uuid::from_string("98765432-1234-5678-1234-123456789abc");
    const std::array types{world::worldDataSchemaId("consumer.Component")};
    auto package = take(lux::scene::createScenePackage(
        asset::AssetId{uuid},
        "SDK Inspector",
        types,
        std::make_shared<const simulation::SimulationDescription>(
            take(std::move(simulation::SimulationDescriptionBuilder{}).build())
        ),
        take(std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved())
    ));
    auto reservation = take(store.reserve<author::SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin()));
    auto model = take(author::SceneSession::create(
        reservation.id(),
        std::nullopt,
        take(author::SceneSource::create(package, schemas))
    ));
    auto* session = model.get();
    assert(store.prepare(reservation, model));
    const auto key = take(store.key<author::SceneSession>(take(store.publish(reservation))));
    simulation::ecs::WorldEntityMap identities;
    author::SceneEditBatch seed{session->describe().current, "SDK seed", {}};
    seed.edits.emplace_back(author::SceneCreateObject{
        {world::WorldObjectId{uuid},
         {0},
         {take(author::encodeSceneValue(consumer::Component{}, schemas, identities, 65536))}}
    });
    assert(session->apply(std::move(seed)));
    const auto initial = session->describe();
    author::SceneInteractionGroup interaction(store.access<author::SceneSession>(), key, {1});
    const author::SceneObjectRef target{key.id(), initial.current.state.history, world::WorldObjectId{uuid}};
    const auto bindings = std::array{consumer::binding()};
    auto detached = take(author::makeInspectorView(
        queue.dispatcherRef(),
        ui::PaneId{"sdk-fields"},
        store.access<author::SceneSession>(),
        {key, &interaction},
        target,
        schemas,
        {bindings.begin(), bindings.end()}
    ));
    auto* inspector = static_cast<author::InspectorView*>(detached.pane());
    auto adopted = host.adopt(detached, views::ViewRestoreKey{"sdk-fields"});
    if (!adopted)
        std::fprintf(stderr, "Inspector mount error: %u\n", unsigned(adopted.error()));
    const auto id = take(std::move(adopted)).id;
    const auto frame = [&] { assert(root->update({{900, 800}, .016F}, nullptr)); };
    frame();
    const auto& schema = schemas.all().front();
    const auto component = [&]()
    {
        auto source = take(session->read());
        auto encoded = take(source.component(target, schema.id));
        return take(source.withRead(
            [&](const auto&) -> author::SceneEditResult<consumer::Component>
            {
                auto decoded = schema.decode_value(encoded.version, encoded.bytes, {}, {});
                assert(decoded);
                simulation::ecs::Registry registry;
                const auto entity = registry.create();
                std::move(*decoded).installInto(registry, entity);
                return registry.get<consumer::Component>(entity);
            }
        ));
    };
    const auto setField = [&](std::string path, auto value)
    {
        author::SceneEditBatch batch{session->describe().current, "SDK field", {}};
        batch.edits.push_back(
            author::SceneSetField::make<consumer::Component>({target, schema.id, std::move(path)}, std::move(value))
        );
        return session->apply(std::move(batch));
    };
    const auto findControl = [&](auto&& self, object::LuxObject& node) -> ui::NumericEdit*
    {
        if (auto* control = dynamic_cast<ui::NumericEdit*>(&node))
            return control;
        for (auto* child = node.firstChild(); child; child = child->nextSibling())
            if (auto* found = self(self, *child))
                return found;
        return nullptr;
    };
    auto& pane = *inspector;
    auto* control = findControl(findControl, pane);
    assert(control);
    const auto read = [&] { return component().settings.gain; };
    const auto original = read();
    assert(std::get<double>(control->value()) == original);
    const auto before = session->historyView()->snapshot;
    control->setValue(original + 1.0);
    assert(read() == original);
    static_cast<void>(ui::ControlsTestAccess::edited(*control, {true, true, false, false}));
    frame();
    assert(read() == original && interaction.overlay());
    assert(session->historyView()->snapshot.current == before.current);
    assert(pane.finishEditing()); // No ImGui deactivation is required.
    assert(!interaction.overlay() && read() == original + 1.0);
    assert(session->historyView()->snapshot.cursor == before.cursor + 1);
    assert(session->undo() && read() == original);
    frame();
    assert(session->redo() && read() == original + 1.0);
    assert(session->undo());
    frame();
    assert(root->requestFocus(*control));
    control->setValue(original + 2.0);
    static_cast<void>(ui::ControlsTestAccess::edited(*control, {true, true, false, false}));
    frame();
    control->setValue(original);
    static_cast<void>(ui::ControlsTestAccess::edited(*control, {true, false, false, true}));
    frame();
    assert(!interaction.overlay() && read() == original);
    assert(session->historyView()->snapshot.current == before.current);
    assert(pane.clearTarget());
    assert(!root->focusedElement() && !findControl(findControl, pane));
    frame();
    assert(pane.rebind({key, &interaction}, target));
    frame();
    assert(findControl(findControl, pane));
    const auto find = [&](auto&& self, lux::object::LuxObject& owner, std::string_view suffix) -> lux::ui::Element*
    {
        if (auto* element = dynamic_cast<lux::ui::Element*>(&owner); element && element->id().name().ends_with(suffix))
            return element;
        for (auto* child = owner.firstChild(); child; child = child->nextSibling())
            if (auto* result = self(self, *child, suffix))
                return result;
        return nullptr;
    };
    auto* array_field = find(find, pane, "::grid[1][2]");
    auto* array_control = array_field ? findControl(findControl, *array_field) : nullptr;
    assert(array_control && std::get<std::int32_t>(array_control->value()) == 6);
    const auto array_before = session->describe();
    array_control->setValue(std::int32_t{19});
    static_cast<void>(ui::ControlsTestAccess::edited(*array_control, {true, true, false, false}));
    frame();
    assert(interaction.overlay() && component().grid[1][2] == 6);
    assert(session->describe().current == array_before.current);
    assert(pane.finishEditing() && component().grid[1][2] == 19);
    assert(session->undo() && component().grid[1][2] == 6);
    frame();
    assert(session->redo() && component().grid[1][2] == 19);
    assert(session->undo());
    frame();
    assert(std::get<std::int32_t>(array_control->value()) == 6);
    std::puts("PASS canonical typedef C array: real generated preview/commit/Undo/Redo through SceneSession");
    auto* sequence = find(find, pane, "::sequence");
    assert(sequence);
    // Fixed arrays inside a dynamic row and tuples containing a dynamic field
    // must use actual child controls, not a generic draw/scratch fallback.
    auto* pairs = find(find, pane, "::pairs");
    auto* pair_field = pairs ? find(find, *pairs, "::pairs[0][1]") : nullptr;
    auto* pair_value = pair_field ? findControl(findControl, *pair_field) : nullptr;
    assert(pair_value && std::get<std::int32_t>(pair_value->value()) == 12);
    pair_value->setValue(std::int32_t{27});
    static_cast<void>(lux::ui::ControlsTestAccess::edited(*pair_value, {true, true, true, false}));
    frame();
    assert(component().pairs[0][1] == 27 && session->undo());
    frame();
    assert(component().pairs[0][1] == 12);
    auto* grouped_sequence = find(find, pane, "::grouped/1");
    assert(grouped_sequence && find(find, *grouped_sequence, "row/1"));
    auto* add = dynamic_cast<lux::ui::Button*>(find(find, *sequence, "add"));
    assert(add);
    const auto sequence_size = component().sequence.size();
    const auto* old_row = find(find, *sequence, "row/0");
    assert(old_row);
    assert(root->requestFocus(*const_cast<lux::ui::Element*>(old_row)));
    static_cast<void>(lux::ui::ControlsTestAccess::activate(*add));
    assert(component().sequence.size() == sequence_size); // Draw-time signals only record intent.
    frame();
    assert(component().sequence.size() == sequence_size + 1 && !root->focusedElement());
    assert(session->undo());
    frame();
    assert(component().sequence.size() == sequence_size);
    assert(session->redo());
    frame();
    assert(component().sequence.size() == sequence_size + 1);
    assert(session->undo());
    frame();

    auto* alternative = find(find, pane, "::choice");
    auto* choice = dynamic_cast<lux::ui::Choice*>(find(find, *alternative->firstChild(), "choice"));
    assert(choice);
    const auto variant_index = component().choice.index();
    choice->setValue(2); // Two alternatives have the same C++ type: index must be retained.
    static_cast<void>(lux::ui::ControlsTestAccess::edited(*choice, lux::ui::EditResult{true, true, true, false}));
    assert(component().choice.index() == variant_index);
    frame();
    assert(component().choice.index() == 2);
    assert(root->requestFocus(*find(find, *alternative, "/2")));
    assert(session->undo());
    frame();
    assert(component().choice.index() == variant_index && !root->focusedElement());

    auto* optional = find(find, pane, "::optional");
    auto* presence = dynamic_cast<lux::ui::Choice*>(find(find, *optional->firstChild(), "choice"));
    assert(presence && component().optional.has_value());
    presence->setValue(0);
    static_cast<void>(lux::ui::ControlsTestAccess::edited(*presence, lux::ui::EditResult{true, true, true, false}));
    assert(component().optional.has_value());
    frame();
    assert(!component().optional.has_value());
    assert(session->undo());
    frame();
    assert(component().optional == 7);

    auto* map = find(find, pane, "::map");
    auto* insertion = dynamic_cast<lux::ui::TextEdit*>(find(find, *map, "new-key"));
    auto* add_key = dynamic_cast<lux::ui::Button*>(find(find, *map, "add"));
    assert(insertion && add_key);
    insertion->setValue("new-key");
    static_cast<void>(lux::ui::ControlsTestAccess::activate(*add_key));
    assert(!component().map.contains("new-key"));
    frame();
    assert(component().map.contains("new-key"));
    assert(session->undo());
    frame();
    assert(!component().map.contains("new-key"));
    const auto history_before_duplicate = session->historyView()->snapshot;
    insertion->setValue("key");
    static_cast<void>(lux::ui::ControlsTestAccess::activate(*add_key));
    frame();
    assert(session->historyView()->snapshot.current == history_before_duplicate.current);
    assert(component().map.contains("key"));
    // A rejected key stays in the interaction buffer until explicit cancellation.
    assert(!pane.finishEditing());
    auto* cancel_draft = dynamic_cast<lux::ui::Button*>(find(find, pane, "cancel"));
    assert(cancel_draft);
    static_cast<void>(lux::ui::ControlsTestAccess::activate(*cancel_draft));
    frame();
    assert(pane.finishEditing());
    assert(session->historyView()->snapshot.current == history_before_duplicate.current);

    const auto flags_before = component().flags;
    assert(setField("flags", std::vector<bool>{false, true, false}));
    assert(component().flags == (std::vector<bool>{false, true, false}));
    assert(session->undo() && component().flags == flags_before);
    frame();
    const auto countObjects = [&](auto&& self, const lux::object::LuxObject& owner) -> std::size_t
    {
        std::size_t count{1};
        for (auto* child = owner.firstChild(); child; child = child->nextSibling())
            count += self(self, *child);
        return count;
    };
    const auto small_tree = countObjects(countObjects, *sequence);
    std::vector<Settings> large(10000);
    assert(setField("sequence", large));
    frame();
    assert(countObjects(countObjects, *sequence) < small_tree * 20); // One page, independent of 10,000 elements.
    const auto stable_count = countObjects(countObjects, *sequence);
    for (int i{}; i != 8; ++i)
        frame();
    assert(countObjects(countObjects, *sequence) == stable_count);
    assert(session->undo());
    frame();
    std::puts("PASS persistent container Elements: deferred structure, variant index, optional, map rejection, "
              "Undo/Redo, bounded rows");
    // Exercise pagination and a same-size replacement on the real list Element.
    auto* list = find(find, pane, "::list");
    assert(list);
    std::list<int> long_list(80, 42);
    assert(setField("list", long_list));
    frame();
    auto* next_page = dynamic_cast<lux::ui::Button*>(find(find, *list, "next"));
    assert(next_page);
    for (unsigned page{}; page != 4; ++page)
    {
        static_cast<void>(lux::ui::ControlsTestAccess::activate(*next_page));
        frame();
    }
    auto* last_row = find(find, *list, "row/64");
    auto* list_value = last_row ? findControl(findControl, *last_row) : nullptr;
    assert(list_value && std::get<std::int32_t>(list_value->value()) == 42);
    const auto list_objects = countObjects(countObjects, *list);
    for (int i{}; i != 8; ++i)
        frame();
    assert(countObjects(countObjects, *list) == list_objects);
    std::fill(long_list.begin(), long_list.end(), 73);
    assert(setField("list", long_list));
    frame();
    assert(std::get<std::int32_t>(list_value->value()) == 73);
    assert(session->undo());
    frame();
    assert(std::get<std::int32_t>(list_value->value()) == 42);
    assert(session->undo());
    frame();
    std::puts("PASS persistent list: last page, idle tree reuse, same-size replacement and Undo");
    {
        author::InspectorFields fields(store.access<author::SceneSession>(), interaction, target, schema);
        checkCompletedGesture(fields, *session);
    }
    assert(host.close(id) && host.drain());
    assert(!host.describe(id) && store.describe(key.id()));
    // A binding record retains code until the generated object destructor has returned.
    bool destroyed{}, released{};
    struct Probe final : ui::Element
    {
        bool& destroyed;
        bool& released;
        Probe(ui::Element& parent, ui::ElementId id, bool& d, bool& r)
            : Element(parent.dispatcherRef(), std::move(id)), destroyed(d), released(r)
        {
        }
        ~Probe() noexcept override
        {
            assert(!released);
            destroyed = true;
        }
        void draw() noexcept override {}
    };
    struct State
    {
        bool* destroyed;
        bool* released;
    };
    static State state;
    state = {&destroyed, &released};
    auto code = std::shared_ptr<const void>(
        new int{1},
        [&](const void* value)
        {
            assert(destroyed);
            released = true;
            delete static_cast<const int*>(value);
        }
    );
    auto registration = consumer::binding();
    registration.code = code;
    registration.create = +[](ui::Element& parent,
                              ui::ElementId id,
                              author::InspectorFields&) noexcept -> author::InspectorComponent::CreateResult
    { return std::make_unique<Probe>(parent, std::move(id), *state.destroyed, *state.released); };
    auto candidate = take(author::makeInspectorView(
        queue.dispatcherRef(),
        ui::PaneId{"code-owner"},
        store.access<author::SceneSession>(),
        {key, &interaction},
        target,
        schemas,
        {registration}
    ));
    const auto probe = take(host.adopt(candidate, views::ViewRestoreKey{"code-owner"})).id;
    frame();
    registration = {};
    code.reset();
    assert(!released && !destroyed);
    assert(host.close(probe) && host.drain());
    assert(destroyed && released);
    std::puts("PASS generated Inspector: scalar preview/commit/cancel; nested/sequence/variant/optional/map/list; "
              "bounded rows; explicit finish; code lifetime");
}
