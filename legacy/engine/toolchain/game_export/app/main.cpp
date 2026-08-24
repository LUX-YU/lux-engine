#include <lux/engine/toolchain/game_export/GameExporter.hpp>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace
{
    struct Options final
    {
        std::filesystem::path project;
        std::filesystem::path cooked;
        std::filesystem::path runtime_root;
        std::filesystem::path output;
        std::string configuration;
        lux::toolchain::ETargetPlatform target{
            lux::toolchain::ETargetPlatform::UNSPECIFIED};
        lux::toolchain::GameBinaryRecipe binary;
        bool help{false};
    };

    void printUsage(std::FILE* output)
    {
        std::fputs(
            "usage:\n"
            "  lux_game_exporter cook --project <file.luxproject> --output <cooked-dir>\n"
            "  lux_game_exporter assemble --cooked <cooked-dir> --runtime-root <prefix>\n"
            "      --output <directory> --target <windows|linux|macos|android>\n"
            "      [--configuration <name>] [binary options]\n"
            "  lux_game_exporter export --project <file.luxproject> --runtime-root <prefix>\n"
            "      --output <directory> --target <windows|linux|macos|android>\n"
            "      [--configuration <name>] [binary options]\n\n"
            "binary options:\n"
            "  --binary-mode <reference|prebuilt|native>\n"
            "  --binary-file <path> --runtime-inventory <path>   (prebuilt)\n"
            "  --native-project <path> --icon <path>             (reserved)\n"
            "  --display-name <text> --version <text>\n"
            "  --publisher <text> --description <text>\n",
            output
        );
    }

    bool parseBinaryMode(
        std::string_view value,
        lux::toolchain::EGameBinaryMode& output)
    {
        if (value == "reference")
            output = lux::toolchain::EGameBinaryMode::REFERENCE_PLAYER;
        else if (value == "prebuilt")
            output = lux::toolchain::EGameBinaryMode::PREBUILT_BINARY;
        else if (value == "native")
            output = lux::toolchain::EGameBinaryMode::NATIVE_PROJECT;
        else
            return false;
        return true;
    }

    bool parseOptions(int argc, char** argv, Options& result)
    {
        for (int index = 2; index < argc; ++index)
        {
            const std::string_view option = argv[index];
            if (option == "--help" || option == "-h")
            {
                result.help = true;
                continue;
            }
            if (index + 1 >= argc)
            {
                std::fprintf(
                    stderr,
                    "missing value after %.*s\n",
                    static_cast<int>(option.size()),
                    option.data()
                );
                return false;
            }
            const std::string value = argv[++index];
            if (option == "--project")
                result.project = value;
            else if (option == "--cooked")
                result.cooked = value;
            else if (option == "--runtime-root")
                result.runtime_root = value;
            else if (option == "--output")
                result.output = value;
            else if (option == "--configuration")
                result.configuration = value;
            else if (option == "--target")
            {
                const auto platform =
                    lux::toolchain::parseTargetPlatform(value);
                if (!platform)
                {
                    std::fprintf(stderr, "unknown target platform: %s\n", value.c_str());
                    return false;
                }
                result.target = *platform;
            }
            else if (option == "--binary-mode")
            {
                if (!parseBinaryMode(value, result.binary.mode))
                {
                    std::fprintf(stderr, "unknown binary mode: %s\n", value.c_str());
                    return false;
                }
            }
            else if (option == "--binary-file")
                result.binary.binary_file = value;
            else if (option == "--runtime-inventory")
                result.binary.runtime_inventory = value;
            else if (option == "--native-project")
                result.binary.native_project_file = value;
            else if (option == "--icon")
                result.binary.icon_file = value;
            else if (option == "--display-name")
                result.binary.metadata.display_name = value;
            else if (option == "--version")
                result.binary.metadata.version = value;
            else if (option == "--publisher")
                result.binary.metadata.publisher = value;
            else if (option == "--description")
                result.binary.metadata.description = value;
            else
            {
                std::fprintf(
                    stderr,
                    "unknown option: %.*s\n",
                    static_cast<int>(option.size()),
                    option.data()
                );
                return false;
            }
        }
        return true;
    }

    int printFailure(
        std::string_view operation,
        const lux::toolchain::GameExportFailure& failure)
    {
        std::fprintf(
            stderr,
            "%.*s failed (%u): %s\n",
            static_cast<int>(operation.size()),
            operation.data(),
            static_cast<unsigned>(failure.code),
            failure.detail.c_str()
        );
        return 1;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printUsage(stderr);
        return 2;
    }
    const std::string_view command = argv[1];
    if (command != "cook" && command != "assemble" && command != "export")
    {
        printUsage(stderr);
        return 2;
    }

    Options options;
    if (!parseOptions(argc, argv, options))
        return 2;
    if (options.help)
    {
        printUsage(stdout);
        return 0;
    }

    if (command == "cook")
    {
        auto result = lux::toolchain::cookGame(
            lux::toolchain::GameCookRequest{options.project, options.output});
        if (!result)
            return printFailure("cook", result.error());
        std::printf(
            "cooked %zu assets (%ju bytes)\n"
            "  receipt: %s\n"
            "  game pak: %s\n",
            result->asset_count,
            static_cast<std::uintmax_t>(result->payload_bytes),
            result->cook_receipt.string().c_str(),
            result->game_pak.string().c_str()
        );
        return 0;
    }

    if (command == "assemble")
    {
        auto result = lux::toolchain::assembleGame(
            lux::toolchain::GameAssemblyRequest{
                options.cooked,
                options.runtime_root,
                options.output,
                options.target,
                options.configuration,
                options.binary});
        if (!result)
            return printFailure("assembly", result.error());
        std::printf(
            "assembled %zu runtime files and %zu extensions for %.*s\n"
            "  executable: %s\n"
            "  manifest:   %s\n"
            "  game pak:   %s\n",
            result->runtime_file_count,
            result->extension_count,
            static_cast<int>(lux::toolchain::targetPlatformName(options.target).size()),
            lux::toolchain::targetPlatformName(options.target).data(),
            result->executable.string().c_str(),
            result->runtime_manifest.string().c_str(),
            result->game_pak.string().c_str()
        );
        return 0;
    }

    auto result = lux::toolchain::exportGame(
        lux::toolchain::GameExportRequest{
            options.project,
            options.runtime_root,
            options.output,
            options.target,
            options.configuration,
            options.binary});
    if (!result)
        return printFailure("export", result.error());

    std::printf(
        "exported %zu assets (%ju bytes), %zu runtime files and %zu extensions for %.*s\n"
        "  executable: %s\n"
        "  manifest:   %s\n"
        "  game pak:   %s\n",
        result->asset_count,
        static_cast<std::uintmax_t>(result->payload_bytes),
        result->runtime_file_count,
        result->extension_count,
        static_cast<int>(lux::toolchain::targetPlatformName(options.target).size()),
        lux::toolchain::targetPlatformName(options.target).data(),
        result->executable.string().c_str(),
        result->runtime_manifest.string().c_str(),
        result->game_pak.string().c_str()
    );
    return 0;
}
