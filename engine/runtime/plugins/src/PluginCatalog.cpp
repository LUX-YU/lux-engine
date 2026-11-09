#include <lux/engine/plugins/PluginCatalog.hpp>

#include <lux/cxx/algorithm/Sha256.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <functional>
#include <set>
#include <unordered_set>

namespace lux::plugins
{
    namespace
    {
        using Json = nlohmann::json;

        bool declarationMatches(const Json& input, std::string_view digest)
        {
            Json projection;
            for (const auto* key :
                 {"abilities",
                  "implementations",
                  "systems",
                  "components",
                  "configurations",
                  "render_features",
                  "render_scene_bindings"})
            {
                projection[key] = input.at(key);
            }
            for (const auto* key : {"geometries", "partitions"})
            {
                if (input.contains(key))
                {
                    projection[key] = input.at(key);
                }
            }
            for (const auto* key : {"id", "version", "dependencies"})
            {
                projection["module"][key] = input.at("plugin").at(key);
            }
            // Fingerprint the declared runtime fields, independently of presentation projections.
            const auto select = [&](const char* kind, std::initializer_list<const char*> fields)
            {
                auto values = Json::array();
                for (const auto& source : input.at(kind))
                {
                    Json value = Json::object();
                    for (const auto* field : fields)
                    {
                        if (source.contains(field))
                        {
                            value[field] = source.at(field);
                        }
                    }
                    values.push_back(std::move(value));
                }
                projection[kind] = std::move(values);
            };
            select("abilities", {"id", "version"});
            select("components", {"id", "version", "storage", "fields"});
            select("configurations", {"id", "version", "fields"});
            select(
                "render_features",
                {"id",
                 "abi_version",
                 "configuration",
                 "scene_configurable",
                 "operation_count",
                 "dependencies",
                 "conflicts",
                 "multiplicity",
                 "operation_layout"}
            );
            const auto bytes = projection.dump();
            const auto expected = lux::cxx::algorithm::Sha256Digest::fromHex(digest);
            return expected && lux::cxx::algorithm::Sha256::hash(std::as_bytes(std::span{bytes})) == *expected;
        }

        bool identity(const Json& input, MetadataIdentity& value)
        {
            value.id = input.at("id").get<std::string>();
            const auto& version = input.at("version");
            if (!version.is_number_unsigned() || version.get<std::uint64_t>() > UINT32_MAX)
            {
                return false;
            }
            value.version = version.get<std::uint32_t>();
            return isMetadataName(value.id) && value.version != 0;
        }

        bool digestValid(std::string_view value) noexcept
        {
            return value.size() == 64 &&
                   std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        }

        bool library(const Json& input, PluginLibraryDescription& value, bool editor)
        {
            if (!input.at("exports").is_array())
            {
                return false;
            }
            const auto path = input.at("path").get<std::string>();
            value.path = std::filesystem::u8path(path);
            const bool invalid_path = path.empty() || path.find('\\') != std::string::npos ||
                                      path.find(':') != std::string::npos || value.path.is_absolute() ||
                                      value.path.has_root_path() ||
                                      std::ranges::any_of(value.path, [](const auto& part) { return part == ".."; });
            if (invalid_path || input.at("interface_version") != 1)
            {
                return false;
            }
            value.sdk_abi = input.at("sdk_abi").get<std::string>();
            value.build_id = input.at("build_id").get<std::string>();
            value.declaration_digest = input.at("declaration_digest").get<std::string>();
            if (value.sdk_abi.empty() || !digestValid(value.build_id) || !digestValid(value.declaration_digest))
            {
                return false;
            }
            static constexpr std::string_view
                names[]{"simulation", "scene", "components", "render", "render_scene", "editor"};
            for (const auto& item : input.at("exports"))
            {
                const auto name = item.get<std::string>();
                const auto found = std::ranges::find(names, name);
                if (found == std::end(names))
                {
                    return false;
                }
                const auto kind = static_cast<EPluginExport>(found - std::begin(names));
                if ((kind == EPluginExport::EDITOR) != editor ||
                    std::ranges::find(value.exports, kind) != value.exports.end())
                {
                    return false;
                }
                value.exports.push_back(kind);
            }
            return !value.exports.empty();
        }

        bool readContracts(const Json& input, std::vector<MetadataSystem::DataContract>& output)
        {
            if (!input.is_array())
            {
                return false;
            }
            std::set<std::pair<bool, std::string>> names;
            for (const auto& item : input)
            {
                MetadataSystem::DataContract value;
                if (!identity(item, value.identity))
                {
                    return false;
                }
                const auto kind = item.at("kind").get<std::string>();
                const auto& phase = item.at("phase");
                const bool invalid_kind = kind != "schema" && kind != "prepared_data";
                const bool invalid_phase = !phase.is_number_unsigned() || phase.get<std::uint64_t>() > 3;
                if (invalid_kind || invalid_phase)
                {
                    return false;
                }
                value.prepared = kind == "prepared_data";
                value.phase = phase.get<std::uint8_t>();
                if (!names.emplace(value.prepared, value.identity.id).second)
                {
                    return false;
                }
                output.push_back(std::move(value));
            }
            return true;
        }

        bool valueType(const Json& input, MetadataValueType& value, bool component)
        {
            const bool invalid_shape = input.size() != (component ? 5u : 4u) || !input.at("fields").is_array();
            if (invalid_shape)
            {
                return false;
            }
            if (!identity(input, value.identity))
            {
                return false;
            }
            value.display_name = input.at("display_name").get<std::string>();
            if (component)
            {
                const auto storage = input.at("storage").get<std::string>();
                if (storage != "authoring" && storage != "runtime_derived")
                {
                    return false;
                }
                value.runtime_derived = storage == "runtime_derived";
            }
            std::unordered_set<std::string> fields;
            for (const auto& item : input.at("fields"))
            {
                MetadataField field;
                field.id = item.at("id").get<std::string>();
                field.type = item.at("type").get<std::string>();
                const bool invalid_field = item.size() != 2 || !isMetadataName(field.id) || field.type.empty();
                const bool duplicate_field = !fields.insert(field.id).second;
                if (invalid_field || duplicate_field)
                {
                    return false;
                }
                value.fields.push_back(std::move(field));
            }
            return !value.display_name.empty();
        }

        PluginResult<PluginDescription> decode(const Json& input, const std::filesystem::path& root)
        {
            PluginDescription value;
            const auto invalid = [&](std::string subject)
            {
                return lux::cxx::unexpected(
                    PluginFailure{EPluginError::INVALID_DESCRIPTION, value.identity.id, std::move(subject)}
                );
            };
            const bool unsupported_version = input.at("version") != 4;
            if (input.at("format") != "lux.engine.plugin" || unsupported_version)
            {
                return invalid("format");
            }
            for (const auto* key :
                 {"abilities",
                  "implementations",
                  "systems",
                  "components",
                  "configurations",
                  "render_features",
                  "render_scene_bindings"})
            {
                if (!input.at(key).is_array())
                {
                    return invalid(key);
                }
            }
            for (const auto* key : {"geometries", "partitions"})
            {
                if (!input.contains(key))
                {
                    continue;
                }
                if (!input.at(key).is_array())
                {
                    return invalid(key);
                }
                std::set<std::string> names;
                for (const auto& item : input.at(key))
                {
                    MetadataIdentity id;
                    if (!identity(item, id) || !names.insert(id.id).second)
                    {
                        return invalid(key);
                    }
                    if (std::string_view(key) == "geometries")
                    {
                        MetadataGeometry geometry;
                        geometry.identity = std::move(id);
                        geometry.partition_input_type = item.at("partition_input_type").get<std::string>();
                        if (geometry.partition_input_type.empty() || !item.at("components").is_array() ||
                            !readContracts(item.at("data"), geometry.data))
                        {
                            return invalid(key);
                        }
                        std::set<std::string> components;
                        for (const auto& member : item.at("components"))
                        {
                            MetadataIdentity component;
                            if (!identity(member, component) || !components.insert(component.id).second)
                            {
                                return invalid(key);
                            }
                            geometry.components.push_back(std::move(component));
                        }
                        if (geometry.components.empty())
                        {
                            return invalid(key);
                        }
                        value.geometries.push_back(std::move(geometry));
                    }
                    else
                    {
                        MetadataPartition partition;
                        partition.identity = std::move(id);
                        partition.input_type = item.at("input_type").get<std::string>();
                        partition.configuration_type = item.at("configuration_type").get<std::string>();
                        const auto& version = item.at("configuration_version");
                        if (partition.input_type.empty() || !version.is_number_unsigned() ||
                            version.get<std::uint64_t>() > UINT32_MAX || !item.at("queries").is_array())
                        {
                            return invalid(key);
                        }
                        partition.configuration_version = version.get<std::uint32_t>();
                        partition.constructs_empty_world = item.at("constructs_empty_world").get<bool>();
                        std::set<std::string> queries;
                        for (const auto& member : item.at("queries"))
                        {
                            MetadataIdentity query;
                            if (!identity(member, query) || !queries.insert(query.id).second)
                            {
                                return invalid(key);
                            }
                            partition.queries.push_back(std::move(query));
                        }
                        value.partitions.push_back(std::move(partition));
                    }
                }
            }
            const auto& plugin = input.at("plugin");
            if (!plugin.at("dependencies").is_array())
            {
                return invalid("dependencies");
            }
            if (!identity(plugin, value.identity))
            {
                return invalid("plugin.identity");
            }
            value.root = root;
            value.author = plugin.at("author").get<std::string>();
            value.description = plugin.at("description").get<std::string>();
            const auto source = plugin.at("source").get<std::string>();
            if (source != "builtin" && source != "plugin")
            {
                return invalid("plugin.source");
            }
            value.builtin = source == "builtin";
            if (!library(plugin.at("runtime_library"), value.runtime_library, false))
            {
                return invalid("runtime_library");
            }
            if (!declarationMatches(input, value.runtime_library.declaration_digest))
            {
                return lux::cxx::unexpected(
                    PluginFailure{EPluginError::DECLARATION_MISMATCH, value.identity.id, "description"}
                );
            }
            if (plugin.contains("editor_library"))
            {
                value.editor_library.emplace();
                if (!library(plugin.at("editor_library"), *value.editor_library, true))
                {
                    return invalid("editor_library");
                }
                if (value.editor_library->declaration_digest != value.runtime_library.declaration_digest)
                {
                    return invalid("editor_library.declaration_digest");
                }
            }
            for (const auto& item : plugin.at("dependencies"))
            {
                MetadataIdentity dep;
                if (!identity(Json{{"id", item.at("plugin")}, {"version", item.at("version")}}, dep))
                {
                    return invalid("dependencies");
                }
                if (std::ranges::any_of(value.dependencies, [&](const auto& other) { return other.id == dep.id; }))
                {
                    return invalid("dependencies.duplicate");
                }
                value.dependencies.push_back(std::move(dep));
            }
            for (const auto& item : input.at("abilities"))
            {
                MetadataAbility ability;
                if (!identity(item, ability.identity))
                {
                    return invalid("abilities");
                }
                ability.display_name = item.at("display_name").get<std::string>();
                ability.description = item.value("description", "");
                value.abilities.push_back(std::move(ability));
            }
            for (const auto& item : input.at("implementations"))
            {
                MetadataImplementation implementation;
                if (!identity(item.at("ability"), implementation.ability))
                {
                    return invalid("implementations");
                }
                implementation.system = item.at("system").get<std::string>();
                value.implementations.push_back(std::move(implementation));
            }
            for (const auto& item : input.at("systems"))
            {
                if (!item.at("requirements").is_array())
                {
                    return invalid("requirements");
                }
                MetadataSystem system;
                if (!identity(item, system.identity))
                {
                    return invalid("systems.identity");
                }
                const auto domain = item.at("domain").get<std::string>();
                if (domain != "simulation" && domain != "scene")
                {
                    return invalid("systems.domain");
                }
                system.domain =
                    domain == "simulation" ? EMetadataSystemDomain::SIMULATION : EMetadataSystemDomain::SCENE;
                const auto read_data = [&](const char* field, auto& contracts) -> bool
                {
                    const auto& values = item.at(field);
                    if (!values.is_array())
                    {
                        return false;
                    }
                    std::set<std::pair<bool, std::string>> names;
                    for (const auto& entry : values)
                    {
                        MetadataSystem::DataContract contract;
                        if (!identity(entry, contract.identity))
                        {
                            return false;
                        }
                        const auto kind = entry.at("kind").get<std::string>();
                        const auto& phase = entry.at("phase");
                        const bool is_invalid_kind = kind != "schema" && kind != "prepared_data";
                        const bool is_invalid_phase = !phase.is_number_unsigned() || phase.get<std::uint64_t>() > 3;
                        if (is_invalid_kind || is_invalid_phase)
                        {
                            return false;
                        }
                        contract.prepared = kind == "prepared_data";
                        contract.phase = phase.get<std::uint8_t>();
                        if (!names.emplace(contract.prepared, contract.identity.id).second)
                        {
                            return false;
                        }
                        contracts.push_back(std::move(contract));
                    }
                    return true;
                };
                if (!read_data("data_inputs", system.data_inputs) || !read_data("data_outputs", system.data_outputs))
                {
                    return invalid("systems.data_contracts");
                }
                if (item.contains("configuration"))
                {
                    system.configuration.emplace();
                    if (!identity(item.at("configuration"), *system.configuration))
                    {
                        return invalid("systems.configuration");
                    }
                }
                for (const auto& [key, output] :
                     {std::pair{"requirements", &system.requirements},
                      std::pair{"preparation_requirements", &system.preparation_requirements}})
                {
                    if (!item.contains(key))
                    {
                        continue;
                    }
                    if (!item.at(key).is_array())
                    {
                        return invalid(key);
                    }
                    std::set<std::string> slots;
                    for (const auto& entry : item.at(key))
                    {
                        MetadataRequirement requirement;
                        requirement.slot = entry.at("slot").get<std::string>();
                        requirement.optional = entry.value("optional", false);
                        const bool invalid_requirement = !isMetadataName(requirement.slot) ||
                                                         !identity(entry.at("ability"), requirement.ability) ||
                                                         !slots.insert(requirement.slot).second;
                        if (invalid_requirement)
                        {
                            return invalid(key);
                        }
                        output->push_back(std::move(requirement));
                    }
                }
                if (item.contains("access"))
                {
                    for (const bool external : {false, true})
                    {
                        if (!item.at("access").at(external ? "external" : "components").is_array())
                        {
                            return invalid("access");
                        }
                        for (const auto& entry : item.at("access").at(external ? "external" : "components"))
                        {
                            const auto mode = entry.at("mode").get<std::string>();
                            const auto type = entry.at(external ? "resource" : "component").get<std::string>();
                            if ((mode != "read" && mode != "write") || !isMetadataName(type))
                            {
                                return invalid("access");
                            }
                            system.access.push_back({type, mode == "write", external});
                        }
                    }
                }
                value.systems.push_back(std::move(system));
            }
            for (const bool component : {true, false})
            {
                for (const auto& item : input.at(component ? "components" : "configurations"))
                {
                    MetadataValueType type;
                    if (!valueType(item, type, component))
                    {
                        return invalid("value_type");
                    }
                    (component ? value.components : value.configurations).push_back(std::move(type));
                }
            }
            for (const auto& item : input.at("render_features"))
            {
                if (!item.at("dependencies").is_array() || !item.at("conflicts").is_array())
                {
                    return invalid("feature.relations");
                }
                MetadataFeature feature;
                if (!identity(Json{{"id", item.at("id")}, {"version", item.at("abi_version")}}, feature.identity))
                {
                    return invalid("render_features.identity");
                }
                feature.display_name = item.at("display_name").get<std::string>();
                feature.scene_configurable = item.at("scene_configurable").get<bool>();
                const auto& count = item.at("operation_count");
                if (!count.is_number_unsigned() || count.get<std::uint64_t>() > 16)
                {
                    return invalid("operation_count");
                }
                feature.operation_count = count.get<std::uint32_t>();
                const auto& layout = item.at("operation_layout");
                if (!layout.is_array() || layout.size() != feature.operation_count)
                {
                    return invalid("operation_layout");
                }
                std::unordered_set<std::string> operation_names;
                for (const auto& operation : layout)
                {
                    const auto name = operation.at("name").get<std::string>();
                    const auto payload = operation.at("payload").get<std::string>();
                    const auto kind = operation.at("kind").get<std::string>();
                    const auto lane = operation.at("lane").get<std::string>();
                    const bool valid_kind =
                        kind == "stream" || kind == "bulk" || kind == "blob" || kind == "resource" || kind == "param";
                    const bool valid_lane = lane == "program" || lane == "control" || lane == "upload";
                    if (!isMetadataName(name) || payload.empty() || !valid_kind || !valid_lane ||
                        !operation_names.insert(name).second)
                    {
                        return invalid("operation_layout");
                    }
                }
                const auto multiplicity = item.at("multiplicity").get<std::string>();
                if (multiplicity != "single" && multiplicity != "multiple")
                {
                    return invalid("multiplicity");
                }
                feature.multiple = multiplicity == "multiple";
                if (item.contains("configuration"))
                {
                    feature.configuration.emplace();
                    if (!identity(item.at("configuration"), *feature.configuration))
                    {
                        return invalid("feature.configuration");
                    }
                }
                if (feature.scene_configurable && !feature.configuration)
                {
                    return invalid("feature.configuration");
                }
                for (const auto& entry : item.at("dependencies"))
                {
                    MetadataFeatureDependency dep;
                    if (!identity(Json{{"id", entry.at("feature")}, {"version", entry.at("version")}}, dep.feature))
                    {
                        return invalid("feature.dependencies");
                    }
                    dep.optional = entry.at("optional").get<bool>();
                    feature.dependencies.push_back(std::move(dep));
                }
                feature.conflicts = item.at("conflicts").get<std::vector<std::string>>();
                value.render_features.push_back(std::move(feature));
            }
            for (const auto& item : input.at("render_scene_bindings"))
            {
                MetadataRenderBinding binding;
                binding.feature = item.at("feature").get<std::string>();
                binding.scene_system = item.at("scene_system").get<std::string>();
                for (const auto& entry : item.at("observations"))
                {
                    MetadataObservation observation{entry.at("component").get<std::string>()};
                    for (const auto& event : entry.at("events"))
                    {
                        const unsigned bit = event == "construct" ? 1
                                             : event == "update"  ? 2
                                             : event == "destroy" ? 4
                                                                  : 0;
                        if (!bit || (observation.events & bit))
                        {
                            return invalid("observations.events");
                        }
                        observation.events |= bit;
                    }
                    if (!observation.events)
                    {
                        return invalid("observations.events");
                    }
                    binding.observations.push_back(std::move(observation));
                }
                value.render_scene_bindings.push_back(std::move(binding));
            }
            return value;
        }
    } // namespace

    PluginResult<void> PluginCatalog::read(
        const std::filesystem::path& file,
        const std::filesystem::path& root
    ) noexcept
    {
        std::error_code error;
        const auto bytes = std::filesystem::file_size(file, error);
        if (error || bytes > 16U * 1024U * 1024U)
        {
            return lux::cxx::unexpected(PluginFailure{EPluginError::IO_FAILURE, {}, file.string(), error.message()});
        }
        const auto absolute_root = std::filesystem::canonical(root, error);
        if (error)
        {
            return lux::cxx::unexpected(PluginFailure{EPluginError::INVALID_PATH, {}, root.string(), error.message()});
        }
        const auto description_file = std::filesystem::canonical(file, error);
        if (error)
        {
            return lux::cxx::unexpected(PluginFailure{EPluginError::IO_FAILURE, {}, file.string(), error.message()});
        }
        try
        {
            std::ifstream stream(file, std::ios::binary);
            std::vector<std::unordered_set<std::string>> keys;
            bool duplicate_key{};
            const auto json = Json::parse(
                stream,
                [&](int, Json::parse_event_t event, Json& value)
                {
                    if (event == Json::parse_event_t::object_start)
                    {
                        keys.emplace_back();
                    }
                    if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
                    {
                        duplicate_key = true;
                    }
                    if (event == Json::parse_event_t::object_end)
                    {
                        keys.pop_back();
                    }
                    return true;
                },
                false
            );
            if (json.is_discarded() || duplicate_key)
            {
                return lux::cxx::unexpected(PluginFailure{EPluginError::INVALID_DESCRIPTION, {}, file.string()});
            }
            std::vector<PluginDescription> pending;
            const bool aggregate = json.at("format") == "lux.engine.capabilities";
            if (aggregate && json.at("version") != 1)
            {
                return lux::cxx::unexpected(PluginFailure{EPluginError::INVALID_DESCRIPTION, {}, "format"});
            }
            const auto entries = aggregate ? json.at("plugins") : Json::array({json});
            if (!entries.is_array())
            {
                return lux::cxx::unexpected(PluginFailure{EPluginError::INVALID_DESCRIPTION});
            }
            for (const auto& entry : entries)
            {
                auto parsed = decode(entry, absolute_root);
                if (!parsed)
                {
                    return lux::cxx::unexpected(parsed.error());
                }
                parsed->description_file = description_file;
                const auto& id = parsed->identity.id;
                if (find(id) ||
                    std::ranges::any_of(pending, [&](const auto& other) { return other.identity.id == id; }))
                {
                    return lux::cxx::unexpected(PluginFailure{EPluginError::DUPLICATE_IDENTITY, id});
                }
                pending.push_back(std::move(*parsed));
            }
            PluginCatalog addition;
            for (auto& plugin : pending)
            {
                addition.plugins_.push_back(std::move(plugin));
            }
            return append(std::move(addition));
        }
        catch (const Json::exception& exception)
        {
            return lux::cxx::unexpected(
                PluginFailure{EPluginError::INVALID_DESCRIPTION, {}, file.string(), exception.what()}
            );
        }
    }

    PluginResult<void> PluginCatalog::append(PluginCatalog addition) noexcept
    {
        for (const auto& plugin : addition.plugins_)
        {
            if (find(plugin.identity.id))
            {
                return lux::cxx::unexpected(PluginFailure{EPluginError::DUPLICATE_IDENTITY, plugin.identity.id});
            }
        }
        std::unordered_set<std::string> identities;
        const auto check = [&](const PluginDescription& plugin)
        {
            const auto add = [&](const auto& values, std::string_view domain)
            {
                for (const auto& value : values)
                {
                    if (!identities.insert(std::string(domain) + value.identity.id).second)
                    {
                        return false;
                    }
                }
                return true;
            };
            return add(plugin.abilities, "ability:") && add(plugin.systems, "system:") &&
                   add(plugin.components, "component:") && add(plugin.configurations, "configuration:") &&
                   add(plugin.render_features, "feature:");
        };
        for (const auto& plugin : plugins_)
        {
            if (!check(plugin))
            {
                std::terminate();
            }
        }
        for (const auto& plugin : addition.plugins_)
        {
            if (!check(plugin))
            {
                return lux::cxx::unexpected(PluginFailure{EPluginError::DUPLICATE_IDENTITY, plugin.identity.id});
            }
        }
        for (auto& plugin : addition.plugins_)
        {
            plugins_.push_back(std::move(plugin));
        }
        decltype(implementations_) implementations;
        for (const auto& plugin : plugins_)
        {
            for (const auto& implementation : plugin.implementations)
            {
                implementations[implementation.ability.id].push_back(&implementation);
            }
        }
        implementations_.swap(implementations);
        return {};
    }

    const PluginDescription* PluginCatalog::find(std::string_view id) const noexcept
    {
        const auto found =
            std::ranges::find_if(plugins_, [id](const auto& plugin) { return plugin.identity.id == id; });
        return found == plugins_.end() ? nullptr : std::addressof(*found);
    }

    PluginResult<std::vector<const PluginDescription*>> PluginCatalog::loadOrder(std::string_view id) const noexcept
    {
        std::vector<const PluginDescription*> order, visiting;
        std::function<PluginResult<void>(const PluginDescription*)> visit = [&](const PluginDescription* plugin
                                                                            ) -> PluginResult<void>
        {
            if (std::ranges::find(order, plugin) != order.end())
            {
                return {};
            }
            if (std::ranges::find(visiting, plugin) != visiting.end())
            {
                return lux::cxx::unexpected(PluginFailure{EPluginError::DEPENDENCY_CYCLE, plugin->identity.id});
            }
            visiting.push_back(plugin);
            for (const auto& dependency : plugin->dependencies)
            {
                const auto* target = find(dependency.id);
                if (!target)
                {
                    return lux::cxx::unexpected(
                        PluginFailure{EPluginError::MISSING_DEPENDENCY, plugin->identity.id, dependency.id}
                    );
                }
                if (target->identity.version != dependency.version)
                {
                    return lux::cxx::unexpected(
                        PluginFailure{EPluginError::DEPENDENCY_VERSION_MISMATCH, plugin->identity.id, dependency.id}
                    );
                }
                auto result = visit(target);
                if (!result)
                {
                    return result;
                }
            }
            visiting.pop_back();
            order.push_back(plugin);
            return {};
        };
        const auto* plugin = find(id);
        if (!plugin)
        {
            return lux::cxx::unexpected(PluginFailure{EPluginError::MISSING_DEPENDENCY, std::string(id)});
        }
        auto result = visit(plugin);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        const auto abilityExists = [&](const MetadataIdentity& identity)
        {
            for (const auto* module : order)
            {
                for (const auto& ability : module->abilities)
                {
                    if (ability.identity == identity)
                    {
                        return true;
                    }
                }
            }
            return false;
        };
        for (const auto* module : order)
        {
            for (const auto& implementation : module->implementations)
            {
                const bool owns_system = std::ranges::any_of(
                    module->systems,
                    [&](const auto& system) { return system.identity.id == implementation.system; }
                );
                if (!owns_system || !abilityExists(implementation.ability))
                {
                    return lux::cxx::unexpected(PluginFailure{
                        EPluginError::INVALID_DESCRIPTION,
                        module->identity.id,
                        implementation.system,
                        "unknown ability or implementation system"
                    });
                }
            }
            for (const auto& system : module->systems)
            {
                if (system.configuration &&
                    !std::ranges::any_of(
                        module->configurations,
                        [&](const auto& configuration) { return configuration.identity == *system.configuration; }
                    ))
                {
                    return lux::cxx::unexpected(PluginFailure{
                        EPluginError::INVALID_DESCRIPTION,
                        module->identity.id,
                        system.configuration->id,
                        "configuration schema/version not declared"
                    });
                }
                for (const auto& requirements : {system.requirements, system.preparation_requirements})
                {
                    for (const auto& requirement : requirements)
                    {
                        if (!abilityExists(requirement.ability))
                        {
                            return lux::cxx::unexpected(PluginFailure{
                                EPluginError::INVALID_DESCRIPTION,
                                module->identity.id,
                                requirement.ability.id,
                                "unknown required ability/version"
                            });
                        }
                    }
                }
            }
        }
        return order;
    }

    std::span<const MetadataImplementation* const> PluginCatalog::implementations(std::string_view ability
    ) const noexcept
    {
        const auto found = implementations_.find(ability);
        return found == implementations_.end() ? std::span<const MetadataImplementation* const>{} : found->second;
    }

    std::vector<const MetadataSystem*> PluginCatalog::systems(EMetadataSystemDomain domain) const
    {
        std::vector<const MetadataSystem*> result;
        for (const auto& plugin : plugins_)
        {
            for (const auto& system : plugin.systems)
            {
                if (system.domain == domain)
                {
                    result.push_back(&system);
                }
            }
        }
        return result;
    }
} // namespace lux::plugins
