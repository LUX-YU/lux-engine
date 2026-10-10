#include <cstdio>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorMenuComposition.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/platform/Process.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <random>

namespace
{
    void contributeProjectMenu(lux::editor::EditorMenuComposition& composition)
    {
        using namespace lux;
        const ui::MenuId menu{"lux.menu.file"};
        const editor::MenuGroupId group{"lux.group.file.project"};
        composition.menus.push_back({menu, "Project"});
        composition.groups.push_back({menu, group});
        composition.actions.push_back(
            {ui::CommandId{"lux.project.close"}, "Close project", "Ctrl+W", {ui::EKey::W, true}}
        );
        composition.actions.push_back({ui::CommandId{"lux.project.cancel_open"}, "Cancel opening"});
        composition.contributions.push_back({menu, group, ui::CommandId{"lux.project.close"}});
        composition.contributions.push_back(
            {menu, group, ui::CommandId{"lux.project.cancel_open"}, {{}, ui::CommandId{"lux.project.close"}}}
        );
    }

    void contributeEditingMenu(lux::editor::EditorMenuComposition& composition)
    {
        using namespace lux;
        const ui::MenuId menu{"lux.menu.edit"};
        const editor::MenuGroupId group{"lux.group.edit.history"};
        composition.menus.push_back({menu, "Edit", {}, {{}, ui::MenuId{"lux.menu.file"}}});
        composition.groups.push_back({menu, group});
        composition.actions.push_back({ui::CommandId{"lux.edit.undo"}, "Undo", "Ctrl+Z", {ui::EKey::Z, true}});
        composition.actions.push_back({ui::CommandId{"lux.edit.redo"}, "Redo", "Ctrl+Y", {ui::EKey::Y, true}});
        composition.contributions.push_back({menu, group, ui::CommandId{"lux.edit.undo"}});
        composition.contributions.push_back(
            {menu, group, ui::CommandId{"lux.edit.redo"}, {{}, ui::CommandId{"lux.edit.undo"}}}
        );
    }

    class WelcomePane final : public lux::ui::Pane
    {
    public:
        WelcomePane(lux::editor::EditorContext& context, const lux::editor::PaneDescription& description)
            : Pane(description.title), layout_{}, label_("LuxEngine Editor Framework v2"),
              detail_("Project: " + context.project().name)
        {
            if (!layout_.addElement(label_) || !layout_.addElement(detail_) || !addElement(layout_))
            {
                std::terminate();
            }
            for (const auto& profile : context.sceneProfiles().profiles())
            {
                auto label = std::make_unique<lux::ui::Label>("Available scene profile: " + profile.display_name);
                if (!layout_.addElement(*label))
                {
                    std::terminate();
                }
                profiles_.push_back(std::move(label));
            }
        }

    private:
        lux::ui::Layout layout_;
        lux::ui::Label label_, detail_;
        std::vector<std::unique_ptr<lux::ui::Label>> profiles_;
    };

    class ProjectStatusPane final : public lux::ui::Pane
    {
    public:
        explicit ProjectStatusPane(lux::editor::LuxEngine& engine)
            : Pane("Project"), engine_(engine),
              label_("No project. Open a .luxproj file or use --create directory name.")
        {
            if (!addElement(label_))
            {
                std::terminate();
            }
            auto changed =
                connect(&engine_, &lux::editor::LuxEngine::projectChanged, this, [this]() noexcept { showProject(); });
            auto failed = connect(
                &engine_,
                &lux::editor::LuxEngine::projectOpenFailed,
                this,
                [this](const lux::editor::ProjectOpenFailure& failure) noexcept
                { label_.setText(lux::error::format(failure.error)); }
            );
            if (!changed || !failed)
            {
                std::terminate();
            }
            changed_ = std::move(*changed);
            failed_ = std::move(*failed);
            showProject();
        }

    private:
        void showProject() noexcept
        {
            auto* project = engine_.project();
            label_.setText(project ? "Project: " + project->project().name : "No project");
        }

        lux::editor::LuxEngine& engine_;
        lux::ui::Label label_;
        lux::object::Connection changed_, failed_;
    };
} // namespace

int main(int argc, char** argv)
{
    auto arguments = lux::engine::platform::processArguments(argc, argv);
    if (!arguments)
    {
        return 2;
    }
    const auto& args = *arguments;
    const bool opening = args.size() == 2;
    const bool creating = args.size() == 4 && args[1] == "--create";
    if (!opening && !creating && args.size() != 1)
    {
        std::fprintf(stderr, "Usage: lux_editor [project.luxproj | --create directory name]\n");
        return 2;
    }
    std::filesystem::path requested_path;
    if (opening || creating)
    {
        std::error_code error;
        requested_path = std::filesystem::absolute(std::filesystem::u8path(args[creating ? 2 : 1]), error);
        if (error)
        {
            std::fprintf(stderr, "Invalid project path: %s\n", error.message().c_str());
            return 2;
        }
    }
    auto assemble = [](lux::editor::EditorComposition& context) noexcept -> lux::editor::FrameworkResult<void>
    {
        if (auto registered = context.registerSceneProfile(lux::editor::sceneProfile3D()); !registered)
        {
            return registered;
        }
        return context.registerUiFactory(
            "framework.welcome",
            [](lux::editor::EditorContext& value, const lux::editor::PaneDescription& description
            ) noexcept -> lux::editor::FrameworkResult<std::unique_ptr<lux::ui::Pane>>
            { return std::unique_ptr<lux::ui::Pane>{new WelcomePane(value, description)}; }
        );
    };
    lux::editor::EditorConfig config;
    config.layout = {{"framework.welcome", "welcome", "Welcome"}};
    auto executable = lux::engine::platform::executablePath();
    if (!executable)
    {
        std::fprintf(
            stderr,
            "Cannot locate the installed plugin catalog: %llu\n",
            static_cast<unsigned long long>(executable.error().native_code)
        );
        return 1;
    }
    const auto product_root = executable->parent_path().parent_path();
    config.plugin_locations = {{product_root / "share/lux-engine/plugins/catalog.json", product_root}};
    auto engine = lux::editor::LuxEngine::create(std::move(config), std::move(assemble));
    if (!engine)
    {
        std::fprintf(stderr, "%s\n", lux::error::format(engine.error()).c_str());
        return 1;
    }
    lux::error::Error rejection;
    if (!(*engine)->window().uiRoot().addPane(std::make_unique<ProjectStatusPane>(**engine)))
    {
        return 1;
    }
    if (opening)
    {
        lux::editor::OpenProjectRequest request{requested_path};
        static_cast<void>(lux::object::sendEvent(**engine, request));
        rejection = request.rejection;
    }
    else if (creating)
    {
        std::random_device entropy;
        std::seed_seq seed{entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy()};
        std::mt19937 random{seed};
        uuids::uuid_random_generator generate(random);
        lux::editor::ProjectManifest manifest{1, generate(), args[3], {{"lux.builtin.scene_render", 1}}};
        lux::editor::CreateProjectRequest request{requested_path, std::move(manifest)};
        static_cast<void>(lux::object::sendEvent(**engine, request));
        rejection = request.rejection;
    }
    if (rejection.type)
    {
        std::fprintf(stderr, "%s\n", lux::error::format(rejection).c_str());
        return 2;
    }
    // Product key bindings, transported by the same menu/shortcut path as external commands.
    lux::editor::EditorMenuComposition contributions;
    contributeProjectMenu(contributions);
    contributeEditingMenu(contributions);
    auto menu = lux::editor::composeEditorMenu(contributions);
    if (!menu)
    {
        std::fprintf(
            stderr,
            "Invalid menu contribution: %s (%u)\n",
            menu.error().id.c_str(),
            static_cast<unsigned>(menu.error().code)
        );
        return 2;
    }
    if (auto installed = (*engine)->window().uiRoot().setMenu(std::move(*menu)); !installed)
    {
        std::fprintf(stderr, "Invalid product menu: %u\n", static_cast<unsigned>(installed.error()));
        return 2;
    }
    auto result = (*engine)->run();
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lux::error::format(result.error()).c_str());
    }
    return result ? 0 : 3;
}
