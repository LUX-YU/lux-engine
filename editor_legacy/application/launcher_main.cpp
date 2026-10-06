#include <lux/engine/editor/application/Launcher.hpp>
#include <lux/engine/platform/Process.hpp>
#include <algorithm>
#include <cstdio>

int main(int argc, char** argv)
{
    auto arguments = lux::engine::platform::processArguments(argc, argv);
    auto executable = lux::engine::platform::executablePath();
    if (!arguments || !executable)
        return 2;
    if (std::ranges::find(*arguments, "--help") != arguments->end())
    {
        std::puts("lux_launcher: select/create a project and open it with the installed lux_editor.");
        return 0;
    }
    const auto smoke = std::ranges::find(*arguments, "--smoke") != arguments->end();
    return lux::editor::application::runLauncher(executable->parent_path().parent_path(), smoke ? 8 : 0);
}
