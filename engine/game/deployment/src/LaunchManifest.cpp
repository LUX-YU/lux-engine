#include <lux/game/LaunchManifest.hpp>

#undef TOML_HEADER_ONLY
#define TOML_HEADER_ONLY 1
#undef TOML_EXCEPTIONS
#define TOML_EXCEPTIONS 0
#undef TOML_SHARED_LIB
#define TOML_SHARED_LIB 0
#include <toml++/toml.hpp>

#include <fstream>
#include <limits>
#include <optional>
#include <sstream>

namespace lux::game
{
    namespace
    {
        lux::cxx::expected<std::string, std::string> readText(
            const std::filesystem::path& path) noexcept
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream.is_open())
            {
                return lux::cxx::unexpected(
                    std::string{"failed to open runtime manifest '"} +
                    path.string() + "'");
            }

            std::ostringstream text;
            text << stream.rdbuf();
            if (!stream.good() && !stream.eof())
            {
                return lux::cxx::unexpected(
                    std::string{"failed to read runtime manifest '"} +
                    path.string() + "'");
            }
            return text.str();
        }

        lux::cxx::expected<std::uint16_t, std::string> readVersionPart(
            const toml::table& table,
            std::string_view   field,
            const std::filesystem::path& manifest_path) noexcept
        {
            const auto value = table[field].value<std::int64_t>();
            if (!value || *value < 0 || *value > 65535)
            {
                return lux::cxx::unexpected(
                    std::string{"runtime manifest '"} +
                    manifest_path.string() + "' has invalid extension field '" +
                    std::string{field} + "'");
            }
            return static_cast<std::uint16_t>(*value);
        }

        lux::cxx::expected<lux::render::CapacityValue, std::string> readCapacity(
            const toml::table& table,
            std::string_view field,
            const std::filesystem::path& manifest_path) noexcept
        {
            const auto node = table[field];
            if (!node)
                return lux::render::CapacityValue::automatic();
            if (const auto mode = node.value<std::string>(); mode)
            {
                if (*mode == "auto" || *mode == "AUTO")
                    return lux::render::CapacityValue::automatic();
            }
            if (const auto value = node.value<std::int64_t>();
                value && *value > 0)
            {
                return lux::render::CapacityValue::exact(
                    static_cast<std::uint64_t>(*value));
            }
            return lux::cxx::unexpected(
                std::string{"runtime manifest '"} +
                manifest_path.string() + "' has invalid capacity." +
                std::string{field});
        }

        lux::cxx::expected<void, std::string> validateForWrite(
            const LaunchManifest& manifest,
            const std::filesystem::path& path) noexcept
        {
            if (manifest.game_pak.empty())
            {
                return lux::cxx::unexpected(
                    std::string{"runtime manifest '"} + path.string() +
                    "' cannot be written without game_pak");
            }
            const auto capacityFitsToml = [](lux::render::CapacityValue value)
            {
                return value.mode != lux::render::CapacityRequestMode::EXPLICIT ||
                    (value.value != 0u &&
                     value.value <= static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()));
            };
            for (std::size_t index = 0u;
                 index < manifest.render_capacity.domains.size();
                 ++index)
            {
                const auto& entry = manifest.render_capacity.domains[index];
                if (!entry.domain.isValid() ||
                    !lux::render::isValidCapacityDomainName(
                        entry.domain.name()) ||
                    !capacityFitsToml(entry.value))
                {
                    return lux::cxx::unexpected(
                        std::string{"runtime manifest '"} + path.string() +
                        "' contains an invalid capacity domain");
                }
                for (std::size_t previous = 0u;
                     previous < index;
                     ++previous)
                {
                    const auto& existing =
                        manifest.render_capacity.domains[previous].domain;
                    if (existing.hash() != entry.domain.hash())
                        continue;
                    return lux::cxx::unexpected(
                        std::string{"runtime manifest '"} + path.string() +
                        (existing.name() == entry.domain.name()
                             ? "' repeats capacity domain '"
                             : "' contains a capacity hash collision at '") +
                        std::string{entry.domain.name()} + "'");
                }
            }
            for (std::size_t i = 0u; i < manifest.extensions.size(); ++i)
            {
                const auto& extension = manifest.extensions[i];
                if (extension.path.empty() ||
                    !lux::extensions::isCanonicalStableName(
                        extension.id.name()
                    ))
                {
                    return lux::cxx::unexpected(
                        std::string{"runtime manifest '"} + path.string() +
                        "' contains an invalid extension entry");
                }
                for (std::size_t j = 0u; j < i; ++j)
                {
                    const auto& existing = manifest.extensions[j];
                    if (existing.id.hash() != extension.id.hash())
                        continue;
                    return lux::cxx::unexpected(
                        std::string{"runtime manifest '"} + path.string() +
                        (existing.id.name() == extension.id.name()
                             ? "' repeats extension '"
                             : "' contains an extension hash collision at '") +
                        std::string{extension.id.name()} + "'");
                }
            }
            return {};
        }
    } // namespace

    lux::cxx::expected<LaunchManifest, std::string>
    LaunchManifest::loadFromFile(
        const std::filesystem::path& path) noexcept
    {
        auto text = readText(path);
        if (!text)
            return lux::cxx::unexpected(std::move(text.error()));

        auto parsed = toml::parse(*text, path.string());
        if (!parsed)
        {
            const auto& error = parsed.error();
            std::ostringstream message;
            message << "runtime manifest '" << path.string()
                    << "' has invalid TOML: " << error.description()
                    << " at line " << error.source().begin.line;
            return lux::cxx::unexpected(message.str());
        }

        const auto& root = parsed.table();
        const auto* runtime = root["runtime"].as_table();
        if (!runtime)
        {
            return lux::cxx::unexpected(
                std::string{"runtime manifest '"} + path.string() +
                "' is missing [runtime]");
        }

        const auto schema = (*runtime)["schema"].value<std::int64_t>();
        if (!schema ||
            (*schema != kSchemaVersion &&
             *schema != kLegacySchemaVersion))
        {
            return lux::cxx::unexpected(
                std::string{"runtime manifest '"} + path.string() +
                "' has an unsupported runtime.schema");
        }

        LaunchManifest result;
        if (const auto title = (*runtime)["title"].value<std::string>())
            result.title = *title;

        const auto game_pak = (*runtime)["game_pak"].value<std::string>();
        if (!game_pak || game_pak->empty())
        {
            return lux::cxx::unexpected(
                std::string{"runtime manifest '"} + path.string() +
                "' is missing runtime.game_pak");
        }
        result.game_pak = std::filesystem::path{*game_pak};

        // Schema 5 uses the product-neutral key. Schema 4 is accepted so
        // deployed games can be upgraded without regenerating every package.
        auto base_pak = (*runtime)["base_pak"].value<std::string>();
        if (!base_pak && *schema == kLegacySchemaVersion)
            base_pak = (*runtime)["engine_pak"].value<std::string>();
        if (base_pak)
            result.base_pak = std::filesystem::path{*base_pak};
        if (const auto scene = (*runtime)["boot_scene"].value<std::string>())
            result.boot_scene = *scene;

        if (const auto* capacity = root["capacity"].as_table())
        {
            const auto* domains = (*capacity)["domains"].as_array();
            if (!domains)
            {
                return lux::cxx::unexpected(
                    std::string{"runtime manifest '"} + path.string() +
                    "' is missing [[capacity.domains]]");
            }
            result.render_capacity.domains.reserve(domains->size());
            for (const auto& node : *domains)
            {
                const auto* table = node.as_table();
                const auto id = table
                    ? (*table)["id"].value<std::string>()
                    : std::optional<std::string>{};
                if (!table || !id ||
                    !lux::render::isValidCapacityDomainName(*id))
                {
                    return lux::cxx::unexpected(
                        std::string{"runtime manifest '"} + path.string() +
                        "' contains an invalid [[capacity.domains]] entry");
                }
                auto value = readCapacity(*table, "value", path);
                if (!value)
                    return lux::cxx::unexpected(std::move(value.error()));
                lux::render::CapacityDomainId parsed_id{*id};
                for (const auto& existing : result.render_capacity.domains)
                {
                    if (existing.domain.hash() != parsed_id.hash())
                        continue;
                    return lux::cxx::unexpected(
                        std::string{"runtime manifest '"} + path.string() +
                        (existing.domain.name() == parsed_id.name()
                             ? "' repeats capacity domain '"
                             : "' contains a capacity hash collision at '") +
                        *id + "'");
                }
                result.render_capacity.domains.push_back(
                    lux::render::CapacityRequestEntry{
                        std::move(parsed_id),
                        *value});
            }
        }

        if (const auto* extensions = root["extensions"].as_array())
        {
            result.extensions.reserve(extensions->size());
            for (const auto& node : *extensions)
            {
                const auto* table = node.as_table();
                if (!table)
                {
                    return lux::cxx::unexpected(
                        std::string{"runtime manifest '"} + path.string() +
                        "' contains a non-table [[extensions]] entry");
                }

                const auto id = (*table)["id"].value<std::string>();
                const auto module_path = (*table)["path"].value<std::string>();
                auto major = readVersionPart(*table, "major", path);
                auto minor = readVersionPart(*table, "minimum_minor", path);
                if (!id || !module_path || module_path->empty() ||
                    !lux::extensions::isCanonicalStableName(*id) ||
                    !major || !minor)
                {
                    return lux::cxx::unexpected(
                        std::string{"runtime manifest '"} + path.string() +
                        "' has an invalid [[extensions]] entry");
                }

                lux::extensions::ExtensionId parsed_id{*id};
                for (const auto& existing : result.extensions)
                {
                    if (existing.id.hash() != parsed_id.hash())
                        continue;
                    return lux::cxx::unexpected(
                        std::string{"runtime manifest '"} + path.string() +
                        (existing.id.name() == parsed_id.name()
                             ? "' repeats extension '"
                             : "' contains an extension hash collision at '") +
                        *id + "'");
                }

                result.extensions.push_back(ExtensionRequirement{
                    std::move(parsed_id),
                    std::filesystem::path{*module_path},
                    *major,
                    *minor});
            }
        }

        return result;
    }

    lux::cxx::expected<void, std::string>
    LaunchManifest::saveToFile(
        const std::filesystem::path& path) const noexcept
    {
        if (auto valid = validateForWrite(*this, path); !valid)
            return valid;

        toml::table runtime{
            {"schema", static_cast<std::int64_t>(kSchemaVersion)},
            {"title", title},
            {"game_pak", game_pak.generic_string()},
            {"base_pak", base_pak.generic_string()},
            {"boot_scene", boot_scene}};
        toml::table root{{"runtime", std::move(runtime)}};

        toml::table capacity;
        toml::array capacity_domains;
        for (const auto& entry : this->render_capacity.domains)
        {
            toml::table domain{{"id", entry.domain.name()}};
            if (entry.value.mode == lux::render::CapacityRequestMode::EXPLICIT)
            {
                domain.insert(
                    "value",
                    static_cast<std::int64_t>(entry.value.value));
            }
            else
            {
                domain.insert("value", "auto");
            }
            capacity_domains.push_back(std::move(domain));
        }
        capacity.insert("domains", std::move(capacity_domains));
        root.insert("capacity", std::move(capacity));

        if (!extensions.empty())
        {
            toml::array entries;
            for (const auto& extension : extensions)
            {
                entries.push_back(toml::table{
                    {"id", std::string{extension.id.name()}},
                    {"path", extension.path.generic_string()},
                    {"major", static_cast<std::int64_t>(
                                  extension.required_major)},
                    {"minimum_minor", static_cast<std::int64_t>(
                                            extension.minimum_minor)}});
            }
            root.insert("extensions", std::move(entries));
        }

        std::error_code ec;
        const auto parent = path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            return lux::cxx::unexpected(
                std::string{"failed to create runtime manifest directory '"} +
                parent.string() + "': " + ec.message());
        }

        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
        {
            return lux::cxx::unexpected(
                std::string{"failed to open runtime manifest '"} +
                path.string() + "' for writing");
        }
        stream << root << '\n';
        stream.flush();
        if (!stream.good())
        {
            return lux::cxx::unexpected(
                std::string{"failed to write runtime manifest '"} +
                path.string() + "'");
        }
        return {};
    }
} // namespace lux::game
