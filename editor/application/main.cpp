#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/editor/application/Launcher.hpp>
#include <lux/engine/platform/Process.hpp>
#include <lux/cxx/arguments/Arguments.hpp>
#include <algorithm>
#include <cstdio>

int main(int argc, char** argv)
{
    auto utf8 = lux::engine::platform::processArguments(argc, argv);
    if (!utf8)
        return 2;
    std::vector<char*> arguments;
    for (auto& value : *utf8)
        arguments.push_back(value.data());
    lux::cxx::Parser parser("lux_editor");
    parser.add<std::string>("project", "p").desc("Open an existing .luxproject");
    parser.add<std::string>("font").desc("Explicit cold UI font file");
    auto parsed = parser.parse(static_cast<int>(arguments.size()), arguments.data());
    if (!parsed)
    {
        std::fprintf(stderr, "%s\n%s", lux::cxx::to_string(parsed.error()).data(), parser.usage().c_str());
        return 2;
    }
    if (std::ranges::any_of(*utf8, [](std::string_view value) { return value == "--help" || value == "-h"; }))
    {
        std::printf("%s", parser.usage().c_str());
        return 0;
    }
    auto executable = lux::engine::platform::executablePath();
    if (!executable)
        return 2;
    const auto installation = executable->parent_path().parent_path();
    if (!parsed->contains("project"))
        return lux::editor::application::runLauncher(installation);
    lux::editor::application::EditorApplicationConfig config;
    config.installation = installation;
    config.project_file = std::filesystem::u8path(*parsed->get("project").as<std::string>());
    if (parsed->contains("font"))
        config.font = std::filesystem::u8path(*parsed->get("font").as<std::string>());
    auto application = lux::editor::application::EditorApplication::create(std::move(config));
    if (!application)
    {
        std::fprintf(stderr, "%s: %s\n", application.error().domain.c_str(), application.error().message.c_str());
        return 3;
    }
    auto result = (*application)->exec();
    if (!result)
    {
        std::fprintf(stderr, "%s: %s\n", result.error().domain.c_str(), result.error().message.c_str());
        return 3;
    }
    return 0;
}
