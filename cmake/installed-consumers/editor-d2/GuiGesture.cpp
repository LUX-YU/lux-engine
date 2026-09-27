#include "../common/ControlsTestAccess.hpp"
#include "../common/UiDrawPane.hpp"
#include <imgui.h>
#include <lux/engine/ui/Table.hpp>
#include <InspectorControl.hpp>
#include <cassert>
#include <consumer/Component.hpp>
#include <consumer/Domain.hpp>
#include <consumer/Gui.hpp>
#include <cstdio>
#include <imgui_internal.h>
#include "ComponentFactoryPane.hpp"

#include <lux/engine/editor/ui/Presentation.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <thread>

namespace consumer
{
    namespace
    {
        std::weak_ptr<const void> control_code;
        bool control_destroyed{};
        class PluginControlProbe final : public lux::ui::Element
        {
        public:
            PluginControlProbe(lux::ui::Element& parent, lux::ui::ElementId id)
                : lux::ui::Element(parent, std::move(id))
            {}
            ~PluginControlProbe() noexcept
            {
                assert(!control_code.expired());
                control_destroyed = true;
            }

        private:
            void draw() noexcept override {}
        };
    }
    void checkUndrawnInspector(
        lux::object::LuxObject& owner,
        lux::editor::scene::SceneEditing& editing,
        lux::editor::editing::EditHistory& history,
        lux::simulation::ecs::Entity target,
        const std::function<void()>& restore_root
    )
    {
        using namespace lux::editor;
        auto root = lux::ui::Root::create(owner.dispatcherRef(), {.docking = false});
        assert(root);
        lux::ui::Pane host(**root, lux::ui::PaneId{"isolated-document"}, lux::ui::PaneTypeId{"test"}, "Host");
        ComponentFactoryPane pane(host, lux::ui::PaneId{"isolated-inspector"}, editing, binding());
        assert(pane.setTarget(editing, target));
        assert((*root)->update({}, nullptr));
        const auto findControl = [&](auto&& self, lux::object::LuxObject& node) -> lux::ui::NumericEdit* {
            if (auto* control = dynamic_cast<lux::ui::NumericEdit*>(&node))
                return control;
            for (auto* child = node.firstChild(); child; child = child->nextSibling())
                if (auto* found = self(self, *child))
                    return found;
            return nullptr;
        };
        auto* control = findControl(findControl, pane);
        assert(control);
        const auto read = [&] {
            return static_cast<const Component*>(editing.component(target, lux::cxx::typeToken<Component>()))
                ->settings.gain;
        };
        const auto original = read();
        assert(std::get<double>(control->value()) == original);
        const auto before = history.view()->snapshot;
        control->setValue(original + 1.0);
        assert(read() == original); // Programmatic setters never edit the model.
        static_cast<void>(lux::ui::ControlsTestAccess::edited(*control, lux::ui::EditResult{true, true, false, false}));
        assert(read() == original + 1.0 && editing.active());
        assert(history.view()->snapshot.current == before.current);
        assert(pane.finishEditing()); // Works without another ImGui draw/deactivation.
        assert(!editing.active() && history.view()->snapshot.cursor == before.cursor + 1);
        assert(history.undo() && read() == original);
        assert(history.redo() && read() == original + 1.0);
        assert(history.undo());
        assert((*root)->requestFocus(*control));
        control->setValue(original + 2.0);
        static_cast<void>(lux::ui::ControlsTestAccess::edited(*control, lux::ui::EditResult{true, true, false, false}));
        control->setValue(original);
        static_cast<void>(lux::ui::ControlsTestAccess::edited(*control, lux::ui::EditResult{true, false, false, true}));
        assert(!editing.active() && read() == original);
        assert(history.view()->snapshot.current == before.current);
        assert(pane.setTarget(editing, lux::simulation::ecs::NullEntity));
        assert(!(*root)->focusedElement() && !findControl(findControl, pane));
        assert((*root)->update({}, nullptr));
        assert(pane.setTarget(editing, target));
        assert((*root)->update({}, nullptr));
        assert(findControl(findControl, pane));
        const auto component = [&]() -> const Component& {
            return *static_cast<const Component*>(editing.component(target, lux::cxx::typeToken<Component>()));
        };
        const auto find = [&](auto&& self, lux::object::LuxObject& owner, std::string_view suffix
                          ) -> lux::ui::Element* {
            if (auto* element = dynamic_cast<lux::ui::Element*>(&owner);
                element && element->id().name().ends_with(suffix))
                return element;
            for (auto* child = owner.firstChild(); child; child = child->nextSibling())
                if (auto* result = self(self, *child, suffix))
                    return result;
            return nullptr;
        };
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
        assert(component().pairs[0][1] == 27 && history.undo());
        assert((*root)->update({}, nullptr));
        assert(component().pairs[0][1] == 12);
        auto* grouped_sequence = find(find, pane, "::grouped/1");
        assert(grouped_sequence && find(find, *grouped_sequence, "row/1"));
        auto* add = dynamic_cast<lux::ui::Button*>(find(find, *sequence, "add"));
        assert(add);
        const auto sequence_size = component().sequence.size();
        const auto* old_row = find(find, *sequence, "row/0");
        assert(old_row);
        assert((*root)->requestFocus(*const_cast<lux::ui::Element*>(old_row)));
        static_cast<void>(lux::ui::ControlsTestAccess::activate(*add));
        assert(component().sequence.size() == sequence_size); // Draw-time signals only record intent.
        assert((*root)->update({}, nullptr));
        assert(component().sequence.size() == sequence_size + 1 && !(*root)->focusedElement());
        assert(history.undo());
        assert((*root)->update({}, nullptr));
        assert(component().sequence.size() == sequence_size);
        assert(history.redo());
        assert((*root)->update({}, nullptr));
        assert(component().sequence.size() == sequence_size + 1);
        assert(history.undo());
        assert((*root)->update({}, nullptr));

        auto* alternative = find(find, pane, "::choice");
        auto* choice = dynamic_cast<lux::ui::Choice*>(find(find, *alternative->firstChild(), "choice"));
        assert(choice);
        const auto variant_index = component().choice.index();
        choice->setValue(2); // Two alternatives have the same C++ type: index must be retained.
        static_cast<void>(lux::ui::ControlsTestAccess::edited(*choice, lux::ui::EditResult{true, true, true, false}));
        assert(component().choice.index() == variant_index);
        assert((*root)->update({}, nullptr));
        assert(component().choice.index() == 2);
        assert((*root)->requestFocus(*find(find, *alternative, "/2")));
        assert(history.undo());
        assert((*root)->update({}, nullptr));
        assert(component().choice.index() == variant_index && !(*root)->focusedElement());

        auto* optional = find(find, pane, "::optional");
        auto* presence = dynamic_cast<lux::ui::Choice*>(find(find, *optional->firstChild(), "choice"));
        assert(presence && component().optional.has_value());
        presence->setValue(0);
        static_cast<void>(lux::ui::ControlsTestAccess::edited(*presence, lux::ui::EditResult{true, true, true, false}));
        assert(component().optional.has_value());
        assert((*root)->update({}, nullptr));
        assert(!component().optional.has_value());
        assert(history.undo());
        assert((*root)->update({}, nullptr));
        assert(component().optional == 7);

        auto* map = find(find, pane, "::map");
        auto* insertion = dynamic_cast<lux::ui::TextEdit*>(find(find, *map, "new-key"));
        auto* add_key = dynamic_cast<lux::ui::Button*>(find(find, *map, "add"));
        assert(insertion && add_key);
        insertion->setValue("new-key");
        static_cast<void>(lux::ui::ControlsTestAccess::activate(*add_key));
        assert(!component().map.contains("new-key"));
        assert((*root)->update({}, nullptr));
        assert(component().map.contains("new-key"));
        assert(history.undo());
        assert((*root)->update({}, nullptr));
        assert(!component().map.contains("new-key"));
        const auto history_before_duplicate = history.view()->snapshot;
        insertion->setValue("key");
        static_cast<void>(lux::ui::ControlsTestAccess::activate(*add_key));
        assert((*root)->update({}, nullptr));
        assert(history.view()->snapshot.current == history_before_duplicate.current);
        assert(component().map.contains("key"));
        // A rejected key stays in the interaction buffer, not in the authoritative component.
        assert(pane.finishEditing());

        const auto countObjects = [&](auto&& self, const lux::object::LuxObject& owner) -> std::size_t {
            std::size_t count{1};
            for (auto* child = owner.firstChild(); child; child = child->nextSibling())
                count += self(self, *child);
            return count;
        };
        const auto small_tree = countObjects(countObjects, *sequence);
        std::vector<Settings> large(10000);
        const auto writable = editing.writeTarget(target);
        assert(writable);
        assert(editing.setField<Component>(
            *writable,
            "sequence",
            "Large sequence",
            [](auto& value) noexcept { return &value.sequence; },
            large
        ));
        assert((*root)->update({}, nullptr));
        assert(countObjects(countObjects, *sequence) < small_tree * 20); // One page, independent of 10,000 elements.
        const auto stable_count = countObjects(countObjects, *sequence);
        for (int i{}; i != 8; ++i)
            assert((*root)->update({}, nullptr));
        assert(countObjects(countObjects, *sequence) == stable_count);
        assert(history.undo());
        assert((*root)->update({}, nullptr));
        std::puts("PASS persistent container Elements: deferred structure, variant index, optional, map rejection, "
                  "Undo/Redo, bounded rows");
        // Exercise pagination and a same-size replacement on the real list Element.
        auto* list = find(find, pane, "::list");
        assert(list);
        std::list<int> long_list(80, 42);
        const auto list_target = editing.writeTarget(target);
        assert(list_target);
        assert(editing.setField<Component>(
            *list_target,
            "list",
            "List page",
            [](auto& value) noexcept { return &value.list; },
            long_list
        ));
        assert((*root)->update({}, nullptr));
        auto* next_page = dynamic_cast<lux::ui::Button*>(find(find, *list, "next"));
        assert(next_page);
        for (unsigned page{}; page != 4; ++page)
        {
            static_cast<void>(lux::ui::ControlsTestAccess::activate(*next_page));
            assert((*root)->update({}, nullptr));
        }
        auto* last_row = find(find, *list, "row/64");
        auto* list_value = last_row ? findControl(findControl, *last_row) : nullptr;
        assert(list_value && std::get<std::int32_t>(list_value->value()) == 42);
        const auto list_objects = countObjects(countObjects, *list);
        for (int i{}; i != 8; ++i)
            assert((*root)->update({}, nullptr));
        assert(countObjects(countObjects, *list) == list_objects);
        std::fill(long_list.begin(), long_list.end(), 73);
        const auto replacement_target = editing.writeTarget(target);
        assert(replacement_target);
        assert(editing.setField<Component>(
            *replacement_target,
            "list",
            "Replace list",
            [](auto& value) noexcept { return &value.list; },
            long_list
        ));
        assert((*root)->update({}, nullptr));
        assert(std::get<std::int32_t>(list_value->value()) == 73);
        assert(history.undo());
        assert((*root)->update({}, nullptr));
        assert(std::get<std::int32_t>(list_value->value()) == 42);
        assert(history.undo());
        assert((*root)->update({}, nullptr));
        std::puts("PASS persistent list: last page, idle tree reuse, same-size replacement and Undo");
        pane.requestClose();
        assert((*root)->update({}, nullptr));
        assert(!pane.visible() && pane.content());
        // A replaced registration must keep old plugin destructors callable until all their objects are gone.
        auto code = std::shared_ptr<const void>(new int{1}, [](const void* value) {
            assert(control_destroyed);
            delete static_cast<const int*>(value);
        });
        control_code = code;
        control_destroyed = false;
        auto registration = binding();
        registration.code_lifetime = code;
        registration.create = [](lux::ui::Element& parent,
                                 lux::ui::ElementId id,
                                 scene::SceneEditing&,
                                 lux::simulation::ecs::Entity,
                                 ui::InspectorInteraction&) noexcept -> ComponentEditorRegistration::CreateResult {
            return std::make_unique<PluginControlProbe>(parent, std::move(id));
        };
        auto plugin_pane =
            std::make_unique<ComponentFactoryPane>(host, lux::ui::PaneId{"plugin-lifetime"}, editing, registration);
        assert(plugin_pane->setTarget(editing, target));
        assert((*root)->update({}, nullptr));
        registration = {};
        code.reset();
        assert(!control_code.expired() && !control_destroyed);
        plugin_pane.reset();
        assert(control_destroyed && control_code.expired());
        std::puts("PASS plugin code lifetime: registration replacement preserves controls; destruction returns before "
                  "code release");
        restore_root();
        std::puts("PASS installed Inspector factory: initial value, silent setters, explicit finish without frame, "
                  "Undo/Redo, cancel, stale child target, local close");
    }

    void checkCompletedGesture(
        lux::editor::scene::SceneEditing& editing,
        lux::editor::editing::EditHistory& history,
        lux::simulation::ecs::Entity object
    )
    {
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
        ui::InspectorInteraction interaction(editing, "completed-gesture");
        const auto before = history.view()->snapshot;
        const auto access = [](auto& value) noexcept { return &value.settings.gain; };
        const auto read = [&]() {
            return static_cast<const Component*>(editing.component(object, lux::cxx::typeToken<Component>()))
                ->settings.gain;
        };
        const auto original = read();
        ImVec2 center{};
        const auto draw = [&](bool visible) {
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
                        ui::generated_support::edited(changed)
                    )));
                }
            }
            if (!visible)
                assert(interaction.finish());
            ImGui::End();
            ImGui::Render();
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
        assert(read() != original && history.view()->snapshot.current == before.current);
        // Release ends the edit even if the widget is omitted on that frame.
        io.AddMouseButtonEvent(0, false);
        draw(false);
        draw(false);
        draw(false);
        std::printf(
            "Completed gesture: active=%d cursor=%zu before=%zu original=%.3f value=%.3f\n",
            interaction.active(),
            history.view()->snapshot.cursor,
            before.cursor,
            original,
            read()
        );
        std::fflush(stdout);
        assert(!interaction.active());
        assert(history.view()->snapshot.cursor == before.cursor + 1);
        draw(true);
        assert(history.view()->snapshot.cursor == before.cursor + 1);
        assert(history.undo() && read() == original);
        assert(history.view()->snapshot.current == before.current);

        const auto no_change = history.view()->snapshot;
        io.AddMousePosEvent(center.x, center.y);
        draw(true);
        io.AddMouseButtonEvent(0, true);
        draw(true);
        assert(!interaction.active());
        io.AddMouseButtonEvent(0, false);
        draw(true);
        assert(!interaction.active() && read() == original);
        assert(history.view()->snapshot.current == no_change.current);
        assert(history.view()->snapshot.revision == no_change.revision);

        std::puts("PASS injected ImGui completed edit: release commits once; omitted widget cannot strand a "
                  "completed field edit; redraw does not duplicate history; unchanged click preserves revision; Undo "
                  "restores");
    }
} // namespace consumer
