#pragma once

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <limits>
#include <lux/engine/editor/EditorMenuComposition.hpp>
#include <lux/engine/ui/Root.hpp>

namespace menu_composition_checks
{
    inline std::string describe(const lux::ui::MenuDefinition& menu)
    {
        std::string result;
        for (const auto& action : menu.actions)
        {
            result += action.id.name();
            result += ':' + action.label + ';';
        }
        const auto append = [&](auto&& self, const lux::ui::MenuNode& node) -> void
        {
            result += '[';
            result += node.id.name();
            for (const auto& entry : node.children)
            {
                if (const auto* action = std::get_if<lux::ui::MenuAction>(&entry))
                {
                    result += '(';
                    result += action->action.name();
                    result += ')';
                }
                else if (const auto* child = std::get_if<lux::ui::MenuNode>(&entry))
                {
                    self(self, *child);
                }
                else
                {
                    result += '|';
                }
            }
            result += ']';
        };
        for (const auto& node : menu.menus)
        {
            append(append, node);
        }
        return result;
    }

    inline void run()
    {
        using namespace lux;
        const ui::MenuId file{"lux.menu.file"}, tools{"lux.menu.tools"}, nested{"external.submenu"};
        const editor::MenuGroupId first{"group.first"}, second{"group.second"}, external{"group.external"};
        const ui::CommandId a{"action.a"}, b{"action.b"}, c{"action.c"};
        editor::EditorMenuComposition input{
            {{c, "C"}, {b, "B"}, {a, "A"}},
            {{tools, "Tools", {}, {{}, file}}, {nested, "Nested", file}, {file, "File"}},
            {{file, second, {{}, first}}, {tools, external}, {file, first}},
            {{file, first, c, {{}, a}}, {tools, external, a}, {file, first, a}, {file, second, b, {a}}}
        };
        auto composed = editor::composeEditorMenu(input);
        assert(composed);
        const auto canonical = describe(*composed);
        assert(
            canonical == "action.a:A;action.b:B;action.c:C;"
                         "[lux.menu.file[external.submenu]|(action.b)|(action.a)(action.c)]"
                         "[lux.menu.tools(action.a)]"
        );
        auto root = ui::Root::create({.docking = false});
        assert(root && (*root)->setMenu(std::move(*composed)));
        // Actual extension declaration order is irrelevant, including reversed action and group inputs.
        std::vector<unsigned> permutation{0, 1, 2, 3};
        do
        {
            auto shuffled = input;
            for (std::size_t index{}; index != permutation.size(); ++index)
            {
                shuffled.contributions[index] = input.contributions[permutation[index]];
            }
            for (unsigned rotation{}; rotation != 3; ++rotation)
            {
                std::rotate(shuffled.menus.begin(), shuffled.menus.begin() + 1, shuffled.menus.end());
                std::reverse(shuffled.groups.begin(), shuffled.groups.end());
                std::reverse(shuffled.actions.begin(), shuffled.actions.end());
                auto result = editor::composeEditorMenu(shuffled);
                assert(result && describe(*result) == canonical);
            }
        } while (std::next_permutation(permutation.begin(), permutation.end()));
        const auto rejected = [&](editor::EditorMenuComposition candidate, editor::EMenuCompositionError code)
        {
            const auto failed = editor::composeEditorMenu(candidate);
            if (failed || failed.error().code != code)
            {
                std::fprintf(
                    stderr,
                    "Menu rejection expected %u, got %d (%s)\n",
                    static_cast<unsigned>(code),
                    failed ? -1 : static_cast<int>(failed.error().code),
                    failed ? "success" : failed.error().id.c_str()
                );
            }
            assert(!failed && failed.error().code == code);
            assert(describe((*root)->menu()) == canonical);
            assert(editor::composeEditorMenu(input));
        };
        auto invalid = input;
        invalid.actions[0].id = {};
        rejected(std::move(invalid), editor::EMenuCompositionError::INVALID_ID);
        invalid = input;
        invalid.menus.push_back(invalid.menus[0]);
        rejected(std::move(invalid), editor::EMenuCompositionError::DUPLICATE_ID);
        invalid = input;
        invalid.groups[0].menu = ui::MenuId{"missing"};
        rejected(std::move(invalid), editor::EMenuCompositionError::UNKNOWN_MENU);
        invalid = input;
        invalid.contributions[0].group = external;
        rejected(std::move(invalid), editor::EMenuCompositionError::UNKNOWN_GROUP);
        invalid = input;
        invalid.contributions[0].action = ui::CommandId{"missing"};
        rejected(std::move(invalid), editor::EMenuCompositionError::UNKNOWN_ACTION);
        invalid = input;
        invalid.contributions.push_back(invalid.contributions[0]);
        rejected(std::move(invalid), editor::EMenuCompositionError::DUPLICATE_PLACEMENT);
        invalid = input;
        invalid.contributions[0].order.after = ui::CommandId{"missing"};
        rejected(std::move(invalid), editor::EMenuCompositionError::UNKNOWN_ANCHOR);
        invalid = input;
        invalid.contributions[2].order.before = b;
        rejected(std::move(invalid), editor::EMenuCompositionError::CYCLIC_ORDER);
        invalid = input;
        invalid.groups[2].order.after = second;
        rejected(std::move(invalid), editor::EMenuCompositionError::CYCLIC_ORDER);
        invalid = input;
        invalid.menus[2].parent = nested;
        invalid.menus[0].order = {}; // Isolate the parent cycle from an unrelated sibling anchor.
        rejected(std::move(invalid), editor::EMenuCompositionError::CYCLIC_ORDER);
        auto priority = input;
        priority.contributions[0].order = {};
        priority.contributions[3].order = {};
        priority.contributions[2].order.priority = std::numeric_limits<int>::min();
        priority.contributions[0].order.priority = std::numeric_limits<int>::max();
        const auto sorted = editor::composeEditorMenu(priority);
        assert(sorted);
        const auto ordered = describe(*sorted);
        assert(ordered.find("(action.c)(action.a)|(action.b)") != std::string::npos);
    }
} // namespace menu_composition_checks
