// lux_player consumes one cooked deployment manifest. It never opens a
// .luxproject, a cooked .luxworld, or a source Content directory.

#include <lux/engine/hosts/player/GameHost.hpp>
#include <lux/game/LaunchManifest.hpp>

#include <lux/cxx/arguments/Arguments.hpp>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace
{
    lux::cxx::Parser buildParser()
    {
        lux::cxx::Parser parser("lux_player");
        parser.add<std::string>("manifest")
            .desc("Cooked .luxruntime.toml manifest; default: <exe stem>.luxruntime.toml");
        parser.add<bool>("vk-validation")
            .desc("Enable the Vulkan validation layer")
            .def(false);
        parser.add<bool>("dump-graph")
            .desc("Dump the compiled render graph after bring-up")
            .def(false);
        parser.add<std::string>("size")
            .desc("Developer override for window size as WxH");
        parser.add<std::string>("title")
            .desc("Developer override for the cooked window title");
        return parser;
    }

    bool wantsHelp(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view argument = argv[i];
            if (argument == "--help" || argument == "-h")
                return true;
        }
        return false;
    }

    void printUsage(const lux::cxx::Parser& parser, std::FILE* stream)
    {
        std::fprintf(
            stream,
            "usage: lux_player [--manifest <game.luxruntime.toml>]\n"
            "                  [--vk-validation] [--dump-graph]\n"
            "                  [--size WxH] [--title <text>]\n"
            "  Without --manifest, the player reads <exe stem>.luxruntime.toml.\n\n");
        std::fputs(parser.usage().c_str(), stream);
    }

    bool readFlag(
        const lux::cxx::ParsedOptions& options,
        const char*                    name,
        bool&                          value)
    {
        const auto parsed = options.get(name).as<bool>();
        if (!parsed)
        {
            const auto error = lux::cxx::to_string(parsed.error());
            std::fprintf(
                stderr,
                "--%s: %.*s\n",
                name,
                static_cast<int>(error.size()),
                error.data());
            return false;
        }
        value = *parsed;
        return true;
    }

    std::optional<std::filesystem::path> deploymentPath(
        const std::filesystem::path& root,
        const std::filesystem::path& relative)
    {
        if (relative.empty() || relative.is_absolute() || relative.has_root_path())
            return std::nullopt;
        const auto normalized = relative.lexically_normal();
        for (const auto& part : normalized)
        {
            if (part == "..")
                return std::nullopt;
        }
        return (root / normalized).lexically_normal();
    }
} // namespace

int main(int argc, char** argv)
{
    const auto parser = buildParser();
    if (wantsHelp(argc, argv))
    {
        printUsage(parser, stdout);
        return 0;
    }

    const auto parsed = parser.parse(argc, argv);
    if (!parsed)
    {
        const auto error = lux::cxx::to_string(parsed.error());
        std::fprintf(
            stderr,
            "%.*s\n",
            static_cast<int>(error.size()),
            error.data());
        printUsage(parser, stderr);
        return 2;
    }

    std::filesystem::path manifest_path;
    if (const auto explicit_path =
            parsed->get("manifest").as<std::string>(); explicit_path)
    {
        manifest_path = *explicit_path;
    }
    else
    {
        std::error_code error;
        manifest_path = std::filesystem::absolute(argv[0], error);
        if (error)
        {
            std::fprintf(stderr, "cannot resolve executable path: %s\n",
                         error.message().c_str());
            return 2;
        }
        manifest_path.replace_extension(".luxruntime.toml");
    }

    auto manifest = lux::game::LaunchManifest::loadFromFile(
        manifest_path);
    if (!manifest)
    {
        std::fprintf(stderr, "%s\n", manifest.error().c_str());
        return 2;
    }

    const auto deployment_root = manifest_path.parent_path();
    const auto game_pak = deploymentPath(
        deployment_root,
        manifest->game_pak);
    if (!game_pak)
    {
        std::fprintf(stderr, "runtime.game_pak must be a deployment-relative path\n");
        return 2;
    }

    lux::game::GameHostConfig config;
    config.title       = manifest->title;
    config.pak_file    = *game_pak;
    config.scene_vpath = manifest->boot_scene;
    config.save_root   = deployment_root / "Saves";
    config.capacity_request = manifest->render_capacity;

    if (!manifest->base_pak.empty())
    {
        const auto base_pak = deploymentPath(
            deployment_root,
            manifest->base_pak);
        if (!base_pak)
        {
            std::fprintf(stderr,
                         "runtime.base_pak must be a deployment-relative path\n");
            return 2;
        }
        config.base_pak_file = *base_pak;
    }

    config.extensions.reserve(manifest->extensions.size());
    for (const auto& extension : manifest->extensions)
    {
        const auto module_path = deploymentPath(
            deployment_root,
            extension.path);
        if (!module_path)
        {
            std::fprintf(
                stderr,
                "extension '%.*s' path must be deployment-relative\n",
                static_cast<int>(extension.id.name().size()),
                extension.id.name().data());
            return 2;
        }
        config.extensions.push_back(
            lux::extensions::ExtensionModuleRequirement::fromPath(
                extension.id,
                *module_path,
                lux::extensions::EExtensionModuleTarget::RUNTIME,
                extension.required_major,
                extension.minimum_minor));
    }

    if (!readFlag(*parsed, "vk-validation", config.enable_validation) ||
        !readFlag(*parsed, "dump-graph", config.dump_graph))
    {
        return 2;
    }

    if (const auto title = parsed->get("title").as<std::string>(); title)
        config.title = *title;
    if (const auto size = parsed->get("size").as<std::string>(); size)
    {
        unsigned width = 0u;
        unsigned height = 0u;
        if (std::sscanf(size->c_str(), "%ux%u", &width, &height) != 2 ||
            width == 0u || height == 0u)
        {
            std::fprintf(stderr, "--size expects WxH, got '%s'\n", size->c_str());
            return 2;
        }
        config.width = width;
        config.height = height;
    }

    lux::game::GameHost host;
    if (!host.init(config))
        return 1;
    const int result = host.run();
    host.shutdown();
    return result;
}
