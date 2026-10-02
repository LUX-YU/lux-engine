#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::application;
    assert(argc == 3);
    const auto directory = std::filesystem::path{argv[2]} /
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(directory);
    const auto id = asset::AssetId{*uuids::uuid::from_string("57279371-1b9c-40d6-b55d-a1a12065d932")};
    const auto manifest = encodeProjectManifest(
        {id, "Command thread qualification", {}, {}, {{"lux.builtin.scene_render", 1, {}}}}
    );
    assert(manifest);
    const auto file = directory / "Project.luxproject";
    {
        std::ofstream output(file, std::ios::binary);
        output << *manifest;
        assert(output);
    }
    EditorApplicationConfig config{file, argv[1], "Command thread qualification", 320, 240, true};
    config.user_directory = directory;
    auto created = EditorApplication::create(config);
    if (!created)
    {
        std::cerr << created.error().domain << ": " << created.error().message << '\n';
        return 2;
    }
    auto app = std::move(*created);
    bool correctly_rejected = true;
    std::thread foreign([&] {
        for (const auto command : {"lux.editor.new.material", "lux.editor.exit", "test.missing"})
        {
            const auto result = app->execute(commands::CommandId{command});
            const bool is_wrong_thread = !result && result.error().code == commands::ECommandError::WRONG_THREAD;
            correctly_rejected = correctly_rejected && is_wrong_thread;
            std::cout << command << " code=" << (result ? -1 : int(result.error().code))
                      << " domain=" << (result ? "success" : result.error().domain) << std::endl;
        }
    });
    foreign.join();
    assert(app->phase() == EApplicationPhase::RUNNING);
    assert(app->execute(commands::CommandId{"lux.editor.assets"}));
    const auto about = app->execute(commands::CommandId{"lux.editor.about"});
    assert(!about && about.error().code == commands::ECommandError::DISABLED);
    const auto missing = app->execute(commands::CommandId{"test.missing"});
    assert(!missing && missing.error().code == commands::ECommandError::NOT_FOUND);
    std::cout << "owner calls recover; phase remains RUNNING; wrong-thread classification="
              << correctly_rejected << '\n';
    return correctly_rejected ? 0 : 1;
}
