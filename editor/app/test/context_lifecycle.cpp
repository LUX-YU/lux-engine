#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/launcher/ProjectCreationPane.hpp>
#include <lux/engine/editor/ui/TaskPane.hpp>
#include <lux/engine/editor/CloseRequest.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace
{
    unsigned created_tools{}, destroyed_tools{};
    constexpr std::array tool_names{"test.tool.1", "test.tool.2", "test.tool.3", "test.tool.4"};
    template <unsigned Index> class ExtraPane final : public lux::ui::Pane
    {
    public:
        explicit ExtraPane(lux::ui::Root& root)
            : Pane(root, lux::ui::PaneId{tool_names[Index]}, lux::ui::PaneTypeId{tool_names[Index]}, "Extra tool")
        {
            ++created_tools;
        }
        ~ExtraPane() override
        {
            ++destroyed_tools;
        }

    private:
        void event(lux::object::EventView& event) noexcept override
        {
            using namespace lux::editor;
            if (const auto* request = event.getIf<CloseRequest>())
            {
                event.accept();
                if (request->action == ECloseAction::CANCEL)
                    return;
                CloseDecision reply{request->id, this, ECloseDecision::READY};
                static_cast<void>(lux::object::routeEvent(*this, root(), reply));
            }
        }
    };
    template <unsigned Index> lux::editor::PaneRegistration extraTool()
    {
        lux::editor::PaneRegistration registration;
        registration.type = lux::ui::PaneTypeId{tool_names[Index]};
        registration.name = tool_names[Index];
        registration.create = [](lux::editor::PaneManager& panes
                              ) noexcept -> lux::editor::PaneRegistration::CreateResult {
            if (auto* existing = panes.findFirst(lux::ui::PaneTypeIdView{tool_names[Index]}))
                return std::ref(*existing);
            return panes.adopt(std::make_unique<ExtraPane<Index>>(panes.root()));
        };
        return registration;
    }
}

int main(int argc, char** argv)
{
    using namespace lux::editor;
    assert(argc == 3);
    const auto root =
        std::filesystem::path(argv[2]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    for (unsigned pass = 0; pass != 2; ++pass)
    {
        ProjectManifest manifest;
        std::array<std::uint8_t, 16> identity{};
        identity.back() = static_cast<std::uint8_t>(pass + 1);
        manifest.id = lux::asset::AssetId{identity};
        manifest.name = "Context lifetime";
        if (pass == 0)
            manifest.plugins.push_back({"lux.builtin.scene_render", 1, {}});
        const auto encoded = encodeProjectManifest(manifest);
        assert(encoded);
        const auto project = root / (std::to_string(pass) + ".luxproject");
        {
            std::ofstream stream(project, std::ios::binary);
            stream << *encoded;
            assert(stream);
        }
        EditorConfig config;
        config.project_file = project;
        config.plugin_root = argv[1];
        config.window.visible = false;
        config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
        if (pass == 0)
        {
            auto invalid = config;
            invalid.window.font = WindowFontSpec{root / "missing-font.ttf"};
            const auto failed = Editor::create(std::move(invalid));
            assert(!failed);
        }
        auto assemble = +[](lux::ui::Root&, EditorContext& context) noexcept {
            return context.panes().setRegistrations({extraTool<0>(), extraTool<1>(), extraTool<2>(), extraTool<3>()});
        };
        auto editor = Editor::create(std::move(config), pass == 1 ? assemble : nullptr);
        if (!editor)
            std::fprintf(stderr, "%s: %s\n", editor.error().domain.c_str(), editor.error().message.c_str());
        assert(editor);
        {
            unsigned notices{}, delivered{};
            auto& context = (*editor)->context();
            auto connected = lux::object::LuxObject::connect(
                &context,
                &EditorContext::taskChanged,
                [&](lux::process::TaskId) noexcept { ++notices; }
            );
            assert(connected);
            auto task = context.execution().submit(
                {"Context monitor", "test"},
                [](lux::process::TaskReporter reporter) noexcept {
                    reporter.setProgress(1, 1);
                    return stdexec::just(lux::cxx::expected<void, lux::process::EExecutionError>{});
                },
                [&](auto&& result) noexcept {
                    assert(result);
                    ++delivered;
                }
            );
            assert(task);
            EditorResult<void> status;
            {
                ui::TaskPane tasks(**editor, context, status);
                assert(status);
            }
            assert(context.execution().collectCompletions());
            assert(delivered == 0 && notices == 0); // Collection and closing the list do not notify business.
            assert(context.execution().dispatchTaskEvents());
            assert(delivered == 1 && notices > 0 && context.taskRevision() > 0);
        }
        if (pass == 1)
        {
            for (const auto name : tool_names)
            {
                const auto first = (*editor)->context().panes().create(lux::ui::PaneTypeIdView{name});
                const auto second = (*editor)->context().panes().create(lux::ui::PaneTypeIdView{name});
                assert(first && second && &first->get() == &second->get());
            }
            assert(created_tools == 4);
            auto& panes = (*editor)->context().panes();
            auto registrations =
                std::vector<PaneRegistration>(panes.registrations().begin(), panes.registrations().end());
            registrations.push_back(registrations.front());
            assert(!panes.setRegistrations(std::move(registrations)));
            assert(panes.registrations().size() == 4); // Rejected candidate leaves the old factories intact.
            const auto first_id = panes.makeId();
            const auto second_id = panes.makeId();
            assert(first_id != second_id);
            auto first = panes.adopt(
                std::make_unique<lux::ui::Pane>(**editor, first_id, lux::ui::PaneTypeId{"test.multi"}, "One")
            );
            auto second = panes.adopt(
                std::make_unique<lux::ui::Pane>(**editor, second_id, lux::ui::PaneTypeId{"test.multi"}, "Two")
            );
            assert(first && second && &first->get() != &second->get());
            panes.setFrozen(true);
            assert(!panes.create(lux::ui::PaneTypeIdView{tool_names[0]}));
            assert(!panes.erase(first_id.view()));
            panes.setFrozen(false);
            assert(panes.erase(first_id.view()) && !panes.find(first_id.view()));
            assert(panes.find(second_id.view()));
            assert(panes.erase(second_id.view()));
            panes.panes().front()->setVisible(false); // Hidden participants are still reviewed on exit.
        }
        if (pass == 1)
        {
            EditorConfig direct_config;
            direct_config.project_file = project;
            direct_config.plugin_root = argv[1];
            direct_config.window.visible = false;
            direct_config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
            auto direct = Editor::create(std::move(direct_config), assemble);
            assert(direct);
            for (const auto name : tool_names)
                assert((*direct)->context().panes().create(lux::ui::PaneTypeIdView{name}));
            auto creation = ProjectCreationPane::create(**direct, (*direct)->context().execution(), argv[1]);
            assert(creation);
            creation->reset(); // Catalog read completes through Process; no Pane update or explicit close.
            direct->reset();   // No exec, no close pump, no renderer shutdown in the caller.
            assert(destroyed_tools == 4);
        }
        (*editor)->requestExit();
        assert((*editor)->exec() == 0);
        editor->reset();
    }
    assert(destroyed_tools == 8);
    std::puts("PASS creation cleanup, four dynamically registered tools, reuse, event close, project/plugin contexts");
}
