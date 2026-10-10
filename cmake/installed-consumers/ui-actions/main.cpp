#include "../../../editor/tests/MenuCompositionChecks.hpp"
#include <cstdio>
#include <filesystem>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/object/ObjectEvent.hpp>

namespace
{
    void host(lux::editor::EditorMenuComposition& composition)
    {
        using namespace lux;
        const ui::MenuId menu{"menu.product"};
        const editor::MenuGroupId group{"group.host"};
        const ui::CommandId action{"host.action"};
        composition.menus.push_back({menu, "Product"});
        composition.groups.push_back({menu, group});
        composition.actions.push_back({action, "Host action"});
        composition.contributions.push_back({menu, group, action});
    }

    class Receiver final : public lux::object::LuxObject
    {
    public:
        unsigned queries{}, executions{};

    private:
        void event(lux::object::EventView& event) noexcept override
        {
            if (auto* command = event.getIf<lux::ui::Command>())
            {
                assert(command->id == lux::ui::CommandIdView{"external.action"});
                event.accept();
                if (command->phase == lux::ui::ECommandPhase::QUERY)
                {
                    ++queries;
                    command->enabled = true;
                }
                else
                {
                    ++executions;
                }
            }
        }
    };
} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    using namespace lux;
    menu_composition_checks::run();
    std::string expected;
    for (bool external_first : {false, true})
    {
        editor::EditorMenuComposition composition;
        {
            engine::platform::DynamicLibrary library{std::filesystem::path{argv[1]}};
            assert(library.is_loaded());
            using Entry = void(editor::EditorMenuComposition&) noexcept;
            auto entry = library.get_symbol<Entry>("contributeEditorMenu");
            assert(entry);
            if (!external_first)
            {
                host(composition);
            }
            entry(composition);
            if (external_first)
            {
                host(composition);
            }
        } // No borrowed descriptor, formatter, handler or code owner remains in the declarations.
        auto menu = editor::composeEditorMenu(composition);
        assert(menu);
        const auto serialized = menu_composition_checks::describe(*menu);
        if (expected.empty())
        {
            expected = serialized;
        }
        assert(serialized == expected);
        assert(serialized.find("(external.action)|(host.action)") != std::string::npos);
        auto root = ui::Root::create({.docking = false});
        assert(root && (*root)->setMenu(std::move(*menu)));
        composition = {}; // The installed Root owns every presentation value.
        Receiver receiver;
        (*root)->setCommandFallback(&receiver);
        ui::DrawData draw;
        for (unsigned index{}; index != 3; ++index)
        {
            assert((*root)->update({{640, 480}, 0.016f}, draw));
        }
        assert((*root)->feedInput(ui::Key{ui::EKey::LEFT_CONTROL, true}));
        assert((*root)->feedInput(ui::Key{ui::EKey::P, true}));
        assert((*root)->update({{640, 480}, 0.016f}, draw));
        assert(receiver.queries && receiver.executions == 0);
        assert((*root)->update());
        assert(receiver.executions == 1);
    }
    std::puts("PASS: installed menu contributions, 72 order permutations, actual DLL unload and routed shortcut");
}
