#include <cstdio>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
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
              detail_("Framework ready. Authoring tools are not loaded.")
        {
            if (!layout_.addElement(label_) || !layout_.addElement(detail_) || !addElement(layout_))
            {
                std::terminate();
            }
        }

    private:
        lux::ui::Layout layout_;
        lux::ui::Label label_, detail_;
    };
} // namespace
int main(int argc, char** argv)
{
    auto assemble = [](lux::editor::EditorContext& context) noexcept -> lux::editor::FrameworkResult<void>
    {
        return context.ui().registerFactory(
            "framework.welcome",
            [](lux::editor::EditorContext& value, const lux::editor::PaneDescription& description
            ) noexcept -> lux::editor::FrameworkResult<std::unique_ptr<lux::ui::Pane>>
            { return std::unique_ptr<lux::ui::Pane>{new WelcomePane(value, description)}; }
        );
    };
    lux::editor::EditorConfig config;
    config.layout = {{"framework.welcome", "welcome", "Welcome"}};
    auto engine = lux::editor::LuxEngine::create(std::move(config), std::move(assemble));
    if (!engine)
    {
        std::fprintf(stderr, "%s\n", lux::error::format(engine.error()).c_str());
        return 1;
    }
    lux::editor::FrameworkResult<void> requested;
    if (argc == 2)
    {
        requested = (*engine)->openProject(std::filesystem::absolute(argv[1]));
    }
    else if (argc == 4 && std::string_view(argv[1]) == "--create")
    {
        std::random_device entropy;
        std::seed_seq seed{entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy()};
        std::mt19937 random{seed};
        uuids::uuid_random_generator generate(random);
        requested = (*engine)->createProject({std::filesystem::absolute(argv[2]), {1, generate(), argv[3]}});
    }
    else if (argc != 1)
    {
        std::fprintf(stderr, "Usage: lux_editor [project.luxproj | --create directory name]\n");
        return 2;
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
