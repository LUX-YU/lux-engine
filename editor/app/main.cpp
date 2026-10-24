#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/AssetBrowserPane.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorMenuComposition.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/LuxEngine.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/editor/ProjectCreationPane.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/SceneCreationPane.hpp>
#include <lux/engine/editor/SceneHierarchyPane.hpp>
#include <lux/engine/editor/SceneInspectorPane.hpp>
#include <lux/engine/editor/SceneProfile3D.hpp>
#include <lux/engine/editor/SceneViewportPane.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/platform/Process.hpp>
#include <lux/engine/project/PluginCatalog.hpp>
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
        for (auto action : std::array{
                 ui::ActionDescriptor{ui::CommandId{"lux.scene.save"}, "Save Scene", "Ctrl+S", {ui::EKey::S, true}},
                 ui::ActionDescriptor{ui::CommandId{"lux.scene.play"}, "Play Scene"},
                 ui::ActionDescriptor{ui::CommandId{"lux.scene.play_headless"}, "Play Headless"},
                 ui::ActionDescriptor{ui::CommandId{"lux.scene.stop"}, "Stop Play"}
             })
        {
            composition.contributions.push_back({menu, group, action.id});
            composition.actions.push_back(std::move(action));
        }
    }

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
        if (auto registered = context.registerServiceFactory<lux::editor::ProjectAssets>(
                [](lux::editor::EditorContext& value
                ) noexcept -> lux::editor::FrameworkResult<std::unique_ptr<lux::editor::ProjectAssets>>
                {
                    auto assets = std::make_unique<lux::editor::ProjectAssets>(value);
                    static_cast<void>(assets->open()
                    ); // The valid service exposes readiness and any IO/admission failure.
                    return assets;
                }
            );
            !registered)
        {
            return registered;
        }
        if (auto registered = context.registerServiceFactory<lux::editor::ProjectScenes>(
                [](lux::editor::EditorContext& value
                ) noexcept -> lux::editor::FrameworkResult<std::unique_ptr<lux::editor::ProjectScenes>>
                {
                    auto assets = value.service<lux::editor::ProjectAssets>();
                    if (!assets)
                    {
                        return lux::cxx::unexpected(assets.error());
                    }
                    return std::make_unique<lux::editor::ProjectScenes>(value, assets->get());
                }
            );
            !registered)
        {
            return registered;
        }
        if (auto registered = context.registerUiFactory(
                "scene.catalog",
                [](lux::editor::EditorContext& value, const lux::editor::PaneDescription& description
                ) noexcept -> lux::editor::FrameworkResult<std::unique_ptr<lux::ui::Pane>>
                {
                    auto scenes = value.service<lux::editor::ProjectScenes>();
                    if (!scenes)
                    {
                        return lux::cxx::unexpected(scenes.error());
                    }
                    return std::make_unique<lux::editor::SceneCreationPane>(value, scenes->get(), description.title);
                }
            );
            !registered)
        {
            return registered;
        }
        if (auto registered = context.registerUiFactory(
                "assets.browser",
                [](lux::editor::EditorContext& value, const lux::editor::PaneDescription& description
                ) noexcept -> lux::editor::FrameworkResult<std::unique_ptr<lux::ui::Pane>>
                {
                    auto assets = value.service<lux::editor::ProjectAssets>();
                    if (!assets)
                    {
                        return lux::cxx::unexpected(assets.error());
                    }
                    return std::make_unique<lux::editor::AssetBrowserPane>(assets->get(), description.title);
                }
            );
            !registered)
        {
            return registered;
        }
        if (auto registered = context.registerUiFactory(
                "scene.hierarchy",
                [](lux::editor::EditorContext& value, const lux::editor::PaneDescription& description
                ) noexcept -> lux::editor::FrameworkResult<std::unique_ptr<lux::ui::Pane>>
                {
                    auto scenes = value.service<lux::editor::ProjectScenes>();
                    if (!scenes)
                    {
                        return lux::cxx::unexpected(scenes.error());
                    }
                    return std::make_unique<lux::editor::SceneHierarchyPane>(scenes->get(), description.title);
                }
            );
            !registered)
        {
            return registered;
        }
        if (auto registered = context.registerUiFactory(
                "scene.inspector",
                [](lux::editor::EditorContext& value, const lux::editor::PaneDescription& description
                ) noexcept -> lux::editor::FrameworkResult<std::unique_ptr<lux::ui::Pane>>
                {
                    auto scenes = value.service<lux::editor::ProjectScenes>();
                    if (!scenes)
                    {
                        return lux::cxx::unexpected(scenes.error());
                    }
                    return std::make_unique<lux::editor::SceneInspectorPane>(scenes->get(), description.title);
                }
            );
            !registered)
        {
            return registered;
        }
        return context.registerUiFactory(
            "scene.viewport",
            [](lux::editor::EditorContext& value, const lux::editor::PaneDescription& description
            ) noexcept -> lux::editor::FrameworkResult<std::unique_ptr<lux::ui::Pane>>
            {
                auto scenes = value.service<lux::editor::ProjectScenes>();
                if (!scenes)
                {
                    return lux::cxx::unexpected(scenes.error());
                }
                return std::make_unique<lux::editor::SceneViewportPane>(
                    scenes->get(),
                    value.service<lux::editor::ProjectAssets>()->get(),
                    value.engine().sceneRuntime(),
                    description.title
                );
            }
        );
    };
    lux::editor::EditorConfig config;
    config.layout = {
        {"scene.catalog", "scenes", "Scenes", lux::editor::EPaneRegion::LEFT},
        {"assets.browser", "assets", "Assets", lux::editor::EPaneRegion::BOTTOM},
        {"scene.viewport", "viewport", "Scene View"},
        {"scene.viewport", "second_viewport", "Second View"},
        {"scene.hierarchy", "hierarchy", "Hierarchy", lux::editor::EPaneRegion::LEFT},
        {"scene.inspector", "inspector", "Inspector", lux::editor::EPaneRegion::RIGHT}
    };
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
    lux::project::PluginCatalog catalog;
    if (auto read = catalog.read(config.plugin_locations.front().catalog, product_root); !read)
    {
        std::fprintf(stderr, "Cannot read plugin catalog: %s\n", read.error().detail.c_str());
        return 1;
    }
    std::vector<lux::editor::ProjectManifest::PluginRecord> available_plugins;
    for (const auto& plugin : catalog.plugins())
    {
        available_plugins.push_back({plugin.identity.id, plugin.identity.version});
    }
    auto engine = lux::editor::LuxEngine::create(std::move(config), std::move(assemble));
    if (!engine)
    {
        std::fprintf(stderr, "%s\n", lux::error::format(engine.error()).c_str());
        return 1;
    }
    lux::error::Error rejection;
    if (!(*engine)->window().uiRoot().addPane(
            std::make_unique<lux::editor::ProjectCreationPane>(**engine, std::move(available_plugins))
        ))
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
        lux::editor::ProjectManifest manifest{1, generate(), args[3], {{"lux.builtin.scene_render", 2}}};
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
