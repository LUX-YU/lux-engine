#include "InspectorModel.hpp"

#include <filesystem>
#include <fstream>
#include <inja/inja.hpp>
#include <iostream>
#include <iterator>
#include <lux/cxx/reflection/runtime/MetaIrJson.hpp>
#include <new>

namespace
{
    using lux::editor::inspector_codegen::Json;

    Json read(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return Json::parse(input);
    }

    bool write(const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
        return bool(file);
    }

    std::string literal(std::string_view text)
    {
        std::string result{"\""};
        for (const unsigned char ch : text)
        {
            if (ch == '"' || ch == '\\')
            {
                result += '\\';
                result += static_cast<char>(ch);
            }
            else if (ch < 32 || ch == 127)
            {
                result += '\\';
                result += static_cast<char>('0' + (ch >> 6));
                result += static_cast<char>('0' + ((ch >> 3) & 7));
                result += static_cast<char>('0' + (ch & 7));
            }
            else
            {
                result += static_cast<char>(ch);
            }
        }
        return result + '"';
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc != 5)
    {
        std::cerr << "Usage: lux_inspector_generator config.json meta-unit.json template-directory staging-directory\n";
        return 1;
    }
    try
    {
        const auto config = read(std::filesystem::u8path(argv[1]));
        const auto input = read(std::filesystem::u8path(argv[2]));
        // Use the original production MetaUnit transport and its validation, not another C++ parser.
        auto unit = lux::cxx::reflection::ir::makeMetaUnitFromTemplateJson(input.dump());
        if (!unit)
        {
            std::cerr << "EDITOR_INSPECTOR_CODEGEN: invalid MetaUnit\n";
            return 2;
        }
        const auto projected = lux::cxx::reflection::ir::templateJson(*unit);
        if (!projected)
        {
            return 2;
        }
        auto model = lux::editor::inspector_codegen::prepare(Json::parse(*projected), config);
        if (!model)
        {
            std::cerr << "EDITOR_INSPECTOR_CODEGEN: " << model.error() << '\n';
            return 3;
        }
        inja::Environment environment(std::string(argv[3]) + '/');
        environment.add_callback(
            "literal",
            1,
            [](inja::Arguments& args) -> Json { return literal(args[0]->get<std::string>()); }
        );
        environment.add_callback(
            "string",
            1,
            [](inja::Arguments& args) -> Json
            { return args[0]->is_string() ? args[0]->get<std::string>() : args[0]->dump(); }
        );
        environment.add_callback(
            "concat",
            [](inja::Arguments& args) -> Json
            {
                std::string result;
                for (const auto* arg : args)
                {
                    result += arg->get<std::string>();
                }
                return result;
            }
        );
        environment.add_callback(
            "access",
            2,
            [](inja::Arguments& args) -> Json
            {
                auto expression = args[0]->get<std::string>();
                expression.replace(expression.find("{}"), 2, args[1]->get<std::string>());
                return expression;
            }
        );
        std::vector<std::pair<std::string, std::string>> outputs;
        outputs.emplace_back(
            config.at("name").get<std::string>() + ".inspector.generated.hpp",
            environment.render_file("declarations.template", *model)
        );
        for (const auto& component : model->at("components"))
        {
            auto projection = *model;
            projection["c"] = component;
            outputs.emplace_back(
                component.at("symbol").get<std::string>() + ".inspector.generated.cpp",
                environment.render_file("component.template", projection)
            );
        }
        const auto staging = std::filesystem::u8path(argv[4]);
        std::filesystem::create_directories(staging);
        Json manifest = Json::array();
        for (const auto& [name, text] : outputs)
        {
            // Output names derive only from the validated CMake job and qualified component symbols.
            if (std::filesystem::path(name).filename() != name)
            {
                std::cerr << "EDITOR_INSPECTOR_CODEGEN: output escapes staging directory\n";
                return 4;
            }
            if (!write(staging / name, text))
            {
                return 4;
            }
            manifest.push_back(name);
        }
        const bool written =
            write(staging / "outputs.json", manifest.dump(2)) && write(staging / "semantics.json", model->dump(2));
        if (!written)
        {
            return 4;
        }
        std::cout << "Editor Inspector: " << model->at("components").size()
                  << " component(s), validated MetaUnit and explicit author/Run inja projections\n";
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (const std::exception& error)
    {
        // Narrow host-tool codec/template boundary; no exceptions cross a runtime or plugin ABI.
        std::cerr << "EDITOR_INSPECTOR_CODEGEN: " << error.what() << '\n';
        return 5;
    }
}
