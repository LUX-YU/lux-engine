#include <lux/engine/editor/assets/ModelImportRecipe.hpp>
#include <lux/engine/editor/project/ProjectManifest.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <toml++/toml.hpp>

namespace lux::editor::assets
{
    namespace
    {
        constexpr std::size_t recipe_limit = 16U * 1024U * 1024U;

        auto failed(std::string domain, std::string detail = {})
        {
            return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, std::move(domain), 0, std::move(detail)});
        }

        EditorResult<void> validate(const ModelImportRecipe& recipe)
        {
            const bool is_invalid_root = !validProjectPath(recipe.root);
            const bool is_invalid_entry = !validProjectPath(recipe.entry);
            const bool is_invalid_count = recipe.files.size() > 4096;
            const bool is_invalid_schema = is_invalid_root || is_invalid_entry || is_invalid_count;
            if (is_invalid_schema)
                return failed("model.recipe.schema");
            const auto& config = recipe.configuration;
            const bool is_invalid_scale = !std::isfinite(config.uniform_scale) || config.uniform_scale <= 0.0F;
            if (is_invalid_scale)
                return failed("model.recipe.scale");
            for (const auto value : config.pre_rotation.coeffs())
            {
                const bool is_invalid_rotation = !std::isfinite(value) || std::abs(value) > 1.0F;
                if (is_invalid_rotation)
                    return failed("model.recipe.rotation");
            }
            if (std::abs(config.pre_rotation.squaredNorm() - 1.0F) > 0.0001F)
                return failed("model.recipe.rotation");
            std::set<std::string> unique_paths;
            bool has_entry{};
            for (const auto& file : recipe.files)
            {
                const bool is_invalid_path = !validProjectPath(file.path);
                const bool is_invalid_digest_length = file.digest.size() != 64;
                const bool is_invalid_digest_chars = !std::ranges::all_of(file.digest, [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                });
                const bool is_invalid_file = is_invalid_path || is_invalid_digest_length || is_invalid_digest_chars;
                if (is_invalid_file)
                    return failed("model.recipe.files");
                auto folded = file.path;
                for (auto& character : folded)
                {
                    if (character >= 'A' && character <= 'Z')
                        character = static_cast<char>(character - 'A' + 'a');
                }
                if (!unique_paths.insert(std::move(folded)).second)
                    return failed("model.recipe.duplicate-path", file.path);
                has_entry = has_entry || file.path == recipe.entry;
            }
            if (!has_entry)
                return failed("model.recipe.entry", recipe.entry);
            return {};
        }
    }

    EditorResult<ModelImportRecipe> decodeModelImportRecipe(std::span<const std::byte> bytes)
    {
        if (bytes.size() > recipe_limit)
            return failed("model.recipe.size");
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        auto parsed = toml::parse(text);
        if (!parsed)
            return failed("model.recipe.parse", std::string(parsed.error().description()));
        const auto& table = parsed.table();
        constexpr std::array known_fields{
            "format", "version", "root", "entry", "scale", "left_handed", "animations", "rotation", "files"
        };
        for (const auto& [key, value] : table)
        {
            if (std::ranges::find(known_fields, key.str()) == known_fields.end())
                return failed("model.recipe.unknown-field", std::string(key.str()));
        }
        const auto format = table["format"].value<std::string>();
        const auto version = table["version"].value<std::int64_t>();
        const auto root = table["root"].value<std::string>();
        const auto entry = table["entry"].value<std::string>();
        const auto scale = table["scale"].value<double>();
        const auto handed = table["left_handed"].value<bool>();
        const auto animated = table["animations"].value<bool>();
        const auto* rotation = table["rotation"].as_array();
        const auto* files = table["files"].as_array();
        const bool is_invalid_format = !format || *format != "lux.editor.model-source";
        const bool is_invalid_version = !version || *version != 1;
        const bool is_missing_path = !root || !entry;
        const bool is_missing_config = !scale || !handed || !animated;
        const bool is_invalid_rotation = !rotation || rotation->size() != 4;
        const bool is_invalid_files = !files || files->size() > 4096;
        const bool is_invalid_schema = is_invalid_format || is_invalid_version || is_missing_path ||
            is_missing_config || is_invalid_rotation || is_invalid_files;
        if (is_invalid_schema)
            return failed("model.recipe.schema");
        const bool is_invalid_scale = !std::isfinite(*scale) || *scale <= 0.0 ||
            *scale > std::numeric_limits<float>::max();
        if (is_invalid_scale)
            return failed("model.recipe.scale");
        ModelImportRecipe recipe{*root, *entry};
        recipe.configuration.uniform_scale = static_cast<float>(*scale);
        recipe.configuration.make_left_handed = *handed;
        recipe.configuration.import_animations = *animated;
        for (std::size_t index{}; index < 4; ++index)
        {
            const auto value = (*rotation)[index].value<double>();
            const bool is_missing_value = !value;
            const bool is_invalid_value = value && (!std::isfinite(*value) || std::abs(*value) > 1.0);
            const bool is_invalid_component = is_missing_value || is_invalid_value;
            if (is_invalid_component)
                return failed("model.recipe.rotation");
            recipe.configuration.pre_rotation.coeffs()[index] = static_cast<float>(*value);
        }
        recipe.files.reserve(files->size());
        for (const auto& item : *files)
        {
            const auto* record = item.as_table();
            const bool is_invalid_record = !record || record->size() != 2;
            if (is_invalid_record)
                return failed("model.recipe.files");
            const auto path = (*record)["path"].value<std::string>();
            const auto digest = (*record)["digest"].value<std::string>();
            const bool is_missing_field = !path || !digest;
            if (is_missing_field)
                return failed("model.recipe.files");
            recipe.files.push_back({*path, *digest});
        }
        auto valid = validate(recipe);
        if (!valid)
            return cxx::unexpected(std::move(valid.error()));
        return recipe;
    }

    EditorResult<std::vector<std::byte>> encodeModelImportRecipe(const ModelImportRecipe& recipe)
    {
        auto valid = validate(recipe);
        if (!valid)
            return cxx::unexpected(std::move(valid.error()));
        toml::array files;
        for (const auto& file : recipe.files)
            files.push_back(toml::table{{"path", file.path}, {"digest", file.digest}});
        toml::array rotation;
        for (const auto value : recipe.configuration.pre_rotation.coeffs())
            rotation.push_back(static_cast<double>(value));
        toml::table table{
            {"format", "lux.editor.model-source"},
            {"version", 1},
            {"root", recipe.root},
            {"entry", recipe.entry},
            {"rotation", std::move(rotation)},
            {"scale", static_cast<double>(recipe.configuration.uniform_scale)},
            {"left_handed", recipe.configuration.make_left_handed},
            {"animations", recipe.configuration.import_animations},
            {"files", std::move(files)}
        };
        std::ostringstream output;
        output << table;
        const auto text = output.str();
        if (text.size() > recipe_limit)
            return failed("model.recipe.size");
        const auto bytes = std::as_bytes(std::span(text));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    }
}
