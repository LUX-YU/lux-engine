#include <cstdio>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/platform/Process.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <random>

namespace
{
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
        }
        void update() noexcept override
        {
            const auto status = engine_.projectStatus();
            if (status.state == last_)
            {
                return;
            }
            last_ = status.state;
            using State = lux::editor::EProjectTransition;
            if (status.failure.type)
            {
                label_.setText(lux::error::format(status.failure));
            }
            else if (last_ == State::PREPARING)
            {
                label_.setText("Preparing project...");
            }
            else if (last_ == State::CLOSING_CURRENT)
            {
                label_.setText("Waiting for accepted project tasks to complete...");
            }
        }

    private:
        lux::editor::LuxEngine& engine_;
        lux::ui::Label label_;
        lux::editor::EProjectTransition last_{lux::editor::EProjectTransition::IDLE};
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
    lux::editor::FrameworkResult<void> requested;
    if (!(*engine)->window().uiRoot().addPane(std::make_unique<ProjectStatusPane>(**engine)))
    {
        return 1;
    }
    if (opening)
    {
        requested = (*engine)->openProject(requested_path);
    }
    else if (creating)
    {
        std::random_device entropy;
        std::seed_seq seed{entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy()};
        std::mt19937 random{seed};
        uuids::uuid_random_generator generate(random);
        lux::editor::ProjectManifest manifest{1, generate(), args[3], {{"lux.builtin.scene_render", 1}}};
        requested = (*engine)->createProject({requested_path, std::move(manifest)});
    }
    if (!requested)
    {
        std::fprintf(stderr, "%s\n", lux::error::format(requested.error()).c_str());
        return 2;
    }
    // Product key bindings, transported by the same menu/shortcut path as external commands.
    (*engine)->window().uiRoot().setMenu(
        {{{},
          "Edit",
          {},
          {},
          {{lux::ui::CommandId{"lux.edit.undo"}, "Undo", "Ctrl+Z", {lux::ui::EKey::Z, true}},
           {lux::ui::CommandId{"lux.edit.redo"}, "Redo", "Ctrl+Y", {lux::ui::EKey::Y, true}}}}}
    );
    auto result = (*engine)->exec();
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lux::error::format(result.error()).c_str());
    }
    return result ? 0 : 3;
}
