#include <filesystem>
#include <fstream>
#include <inja/inja.hpp>
#include <iostream>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/reflection/runtime/MetaIrJson.hpp>
#include <new>
#include <regex>
#include <set>

namespace
{
    using Json = nlohmann::json;
    using Result = lux::cxx::expected<Json, std::string>;

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

    bool qualifiedName(const std::string& text)
    {
        static const std::regex syntax{"[A-Za-z_][A-Za-z_0-9]*(::[A-Za-z_][A-Za-z_0-9]*)*"};
        return std::regex_match(text, syntax);
    }

    Result prepare(const Json& config)
    {
        const auto name = config.at("name").get<std::string>();
        const auto scope = config.at("namespace").get<std::string>();
        const bool is_invalid_name = !qualifiedName(name) || name.find("::") != name.npos;
        const bool is_invalid_scope = !qualifiedName(scope);
        const bool is_invalid_output = is_invalid_name || is_invalid_scope;
        if (is_invalid_output)
        {
            return lux::cxx::unexpected("invalid generated declaration name");
        }
        Json model{{"name", name}, {"namespace", scope}, {"headers", Json::array()}, {"modules", Json::array()}};
        std::set<std::string> symbols;
        std::set<std::string> headers;
        for (const auto& item : config.at("inputs"))
        {
            const auto header = item.at("logical_path").get<std::string>();
            const std::filesystem::path path(header);
            const bool has_unsafe_text = header.find_first_of("\"\\\r\n<>:") != header.npos;
            const bool is_invalid_header = header.empty() || path.has_root_path() || has_unsafe_text ||
                                           path.lexically_normal() != path || header.starts_with("..");
            if (is_invalid_header)
            {
                return lux::cxx::unexpected("invalid module logical header");
            }
            const auto input = read(std::filesystem::u8path(item.at("ir").get<std::string>()));
            auto unit = lux::cxx::reflection::ir::makeMetaUnitFromTemplateJson(input.dump());
            if (!unit)
            {
                return lux::cxx::unexpected("invalid module MetaUnit");
            }
            const auto projected = lux::cxx::reflection::ir::templateJson(*unit);
            if (!projected)
            {
                return lux::cxx::unexpected("module MetaUnit projection failed");
            }
            const auto declarations = Json::parse(*projected).at("declarations");
            std::size_t count{};
            for (const auto& declaration : declarations)
            {
                bool marked{};
                for (const auto& attribute : declaration.value("attributes", Json::array()))
                {
                    marked = marked || attribute == "luxmodule";
                }
                if (!marked)
                {
                    continue;
                }
                // MetaUnit distinguishes the displayed signature from the callable C++ name.
                const auto symbol = declaration.at("invoke_name").get<std::string>();
                const bool is_function = declaration.value("__kind", std::string{}) == "FunctionDecl";
                const bool is_valid_symbol = qualifiedName(symbol);
                const bool has_parameters = !declaration.at("params").empty();
                const bool is_generic = declaration.value("is_template", false) ||
                                        declaration.value("is_variadic", false);
                const bool is_invalid_declaration = !is_function || !is_valid_symbol || has_parameters || is_generic;
                if (is_invalid_declaration)
                {
                    return lux::cxx::unexpected("luxmodule requires a qualified free function");
                }
                if (!symbols.insert(symbol).second)
                {
                    return lux::cxx::unexpected("duplicate or overloaded module declaration: " + symbol);
                }
                model["modules"].push_back(symbol);
                ++count;
            }
            if (!count)
            {
                return lux::cxx::unexpected("selected module has no luxmodule declaration: " + header);
            }
            if (headers.insert(header).second)
            {
                model["headers"].push_back(header);
            }
        }
        return model;
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "Usage: lux_editor_module_generator config.json template-directory staging-directory\n";
        return 1;
    }
    try
    {
        auto model = prepare(read(std::filesystem::u8path(argv[1])));
        if (!model)
        {
            std::cerr << "EDITOR_MODULE_CODEGEN: " << model.error() << '\n';
            return 2;
        }
        inja::Environment environment(std::string(argv[2]) + '/');
        const auto header = environment.render_file("declarations.template", *model);
        const auto implementation = environment.render_file("index.template", *model);
        const auto output = std::filesystem::u8path(argv[3]);
        const auto name = model->at("name").get<std::string>();
        std::filesystem::create_directories(output);
        const bool written = write(output / (name + ".modules.hpp"), header) &&
                             write(output / (name + ".modules.cpp"), implementation) &&
                             write(output / "semantics.json", model->dump(2));
        if (!written)
        {
            return 3;
        }
        std::cout << "Editor module index: " << model->at("modules").size()
                  << " strong references from MetaUnit/inja; no factories executed\n";
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (const std::exception& error)
    {
        std::cerr << "EDITOR_MODULE_CODEGEN: " << error.what() << '\n';
        return 4;
    }
    return 0;
}
