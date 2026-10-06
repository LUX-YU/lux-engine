#include <lux/engine/editor/workspace/LegacyWorkspaceMigration.hpp>
#include <lux/engine/editor/storage/FilePublication.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <sstream>

namespace lux::editor::workspace
{
    namespace
    {
        auto failed(EWorkspaceError code, std::string detail)
        {
            return lux::cxx::unexpected(WorkspaceFailure{code, std::move(detail)});
        }
        std::string digest(std::string_view text)
        {
            return storage::publicationDigest(std::as_bytes(std::span(text)));
        }
        LayoutId legacyId(std::string_view file)
        {
            return {digest("lux.workspace.legacy.layout.v1:" + std::string(file)).substr(0, 32)};
        }
        std::string_view trim(std::string_view value)
        {
            auto start = value.find_first_not_of(" \t\r");
            if (start == value.npos)
                return {};
            return value.substr(start, value.find_last_not_of(" \t\r") - start + 1);
        }
        std::string_view token(std::string_view line, std::string_view name)
        {
            auto pos = line.find(name);
            if (pos == line.npos)
                return {};
            auto value = line.substr(pos + name.size());
            return value.substr(0, value.find_first_of(" \t\r"));
        }
        std::optional<std::uint32_t> integer(std::string_view text, int base = 10)
        {
            if (base == 16 && text.starts_with("0x"))
                text.remove_prefix(2);
            std::uint32_t value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
            if (error != std::errc{} || end != text.data() + text.size())
                return {};
            return value;
        }
        bool pair(std::string_view text, double& a, double& b)
        {
            const auto comma = text.find(',');
            if (comma == text.npos)
                return false;
            const auto [end_a, err_a] = std::from_chars(text.data(), text.data() + comma, a);
            const auto [end_b, err_b] = std::from_chars(text.data() + comma + 1, text.data() + text.size(), b);
            return err_a == std::errc{} && err_b == std::errc{} && end_a == text.data() + comma &&
                   end_b == text.data() + text.size();
        }
        struct LegacyNode final
        {
            std::uint32_t id{}, parent{};
            char split{};
            double x{}, y{}, width{1280}, height{720};
            bool space{};
            std::vector<std::uint32_t> children;
        };
        struct LegacyWindow final
        {
            std::string id;
            std::uint32_t dock{};
            double x{}, y{}, width{640}, height{480};
        };
        WorkspaceResult<void> geometry(DockLayout& layout, std::string_view ini, WorkspaceLimits limits)
        {
            std::map<std::uint32_t, LegacyNode> nodes;
            std::vector<std::uint32_t> order;
            std::map<std::string, LegacyWindow> windows;
            LegacyWindow* window{};
            bool docking{};
            bool recognized{};
            std::size_t at{};
            while (at < ini.size())
            {
                const auto end = ini.find('\n', at);
                const auto line = trim(ini.substr(at, end == ini.npos ? ini.size() - at : end - at));
                at = end == ini.npos ? ini.size() : end + 1;
                if (line.empty() || line.starts_with(';'))
                    continue;
                if (line.starts_with('['))
                {
                    window = nullptr;
                    docking = line == "[Docking][Data]";
                    if (!line.ends_with(']'))
                        return failed(EWorkspaceError::INVALID_DATA, "legacy INI section");
                    if (line.starts_with("[Window]["))
                    {
                        recognized = true;
                        auto name = line.substr(9, line.size() - 10);
                        const auto marker = name.find("###");
                        if (marker == name.npos)
                            continue; // ImGui/tool-private window, retained in original bytes.
                        const std::string id(name.substr(marker + 3));
                        if (windows.size() >= limits.entries)
                            return failed(EWorkspaceError::CAPACITY, "legacy windows");
                        auto [entry, inserted] = windows.emplace(id, LegacyWindow{id});
                        if (!inserted)
                            return failed(EWorkspaceError::INVALID_DATA, "duplicate legacy window");
                        window = &entry->second;
                    }
                    continue;
                }
                if (docking && (line.starts_with("DockNode ") || line.starts_with("DockSpace ")))
                {
                    recognized = true;
                    const auto id = integer(token(line, "ID="), 16);
                    const auto parent_text = token(line, "Parent=");
                    const auto parent =
                        parent_text.empty() ? std::optional<std::uint32_t>{0} : integer(parent_text, 16);
                    if (!id || !*id || !parent)
                        return failed(EWorkspaceError::INVALID_DATA, "legacy dock identity");
                    LegacyNode node{*id, *parent};
                    node.space = line.starts_with("DockSpace ");
                    auto split = token(line, "Split=");
                    if (!split.empty() && split != "X" && split != "Y")
                        return failed(EWorkspaceError::INVALID_DATA, "legacy split");
                    node.split = split.empty() ? 0 : split.front();
                    const auto pos = token(line, "Pos="), size = token(line, "Size="), ref = token(line, "SizeRef=");
                    const bool invalid_pos = !pos.empty() && !pair(pos, node.x, node.y);
                    const bool invalid_size = !size.empty() && !pair(size, node.width, node.height);
                    const bool invalid_ref = !ref.empty() && !pair(ref, node.width, node.height);
                    if (invalid_pos || invalid_size || invalid_ref)
                        return failed(EWorkspaceError::INVALID_DATA, "legacy geometry");
                    if (nodes.size() >= limits.entries)
                        return failed(EWorkspaceError::CAPACITY, "legacy nodes");
                    if (!nodes.emplace(*id, node).second)
                        return failed(EWorkspaceError::INVALID_DATA, "duplicate legacy node");
                    order.push_back(*id);
                }
                else if (window)
                {
                    if (line.starts_with("Pos=") && !pair(line.substr(4), window->x, window->y))
                        return failed(EWorkspaceError::INVALID_DATA, "legacy window position");
                    if (line.starts_with("Size=") && !pair(line.substr(5), window->width, window->height))
                        return failed(EWorkspaceError::INVALID_DATA, "legacy window size");
                    if (line.starts_with("DockId="))
                    {
                        const auto value =
                            line.substr(7, line.find(',') == line.npos ? line.size() - 7 : line.find(',') - 7);
                        const auto id = integer(value, 16);
                        if (!id)
                            return failed(EWorkspaceError::INVALID_DATA, "legacy window dock");
                        window->dock = *id;
                    }
                }
                else if (!docking && !recognized)
                    return failed(EWorkspaceError::INVALID_DATA, "unrecognized legacy dock data");
            }
            if (!recognized)
                return failed(EWorkspaceError::INVALID_DATA, "no legacy geometry");
            for (auto id : order)
            {
                const auto parent = nodes.at(id).parent;
                if (!parent)
                    continue;
                auto found = nodes.find(parent);
                if (found == nodes.end())
                    return failed(EWorkspaceError::INVALID_DATA, "legacy parent missing");
                found->second.children.push_back(id);
            }
            std::map<std::uint32_t, std::size_t> index;
            for (auto id : order)
            {
                const auto& source = nodes.at(id);
                DockNode node{id};
                if (source.split)
                {
                    if (source.children.size() != 2)
                        return failed(EWorkspaceError::INVALID_DATA, "legacy split children");
                    node.split = source.split == 'X' ? EDockSplit::HORIZONTAL : EDockSplit::VERTICAL;
                    node.first = source.children[0];
                    node.second = source.children[1];
                    const auto& a = nodes.at(node.first);
                    const auto& b = nodes.at(node.second);
                    node.ratio = source.split == 'X' ? a.width / (a.width + b.width) : a.height / (a.height + b.height);
                }
                else if (!source.children.empty())
                    return failed(EWorkspaceError::INVALID_DATA, "legacy leaf children");
                index[id] = layout.dock.nodes.size();
                layout.dock.nodes.push_back(std::move(node));
                if (!source.parent)
                    layout.dock.roots.push_back({id, source.x, source.y, source.width, source.height, !source.space});
            }
            std::uint32_t next{1};
            for (const auto& slot : layout.slots)
            {
                const auto name = std::string(slot.restore_key.name()).substr(7); // explicit legacy: mapping
                const auto found = windows.find(name);
                LegacyWindow placement = found == windows.end() ? LegacyWindow{} : found->second;
                if (placement.dock)
                {
                    const auto target = index.find(placement.dock);
                    if (target == index.end())
                        return failed(EWorkspaceError::INVALID_DATA, "legacy window missing dock");
                    layout.dock.nodes[target->second].slots.push_back(slot.id);
                }
                else
                {
                    while (nodes.contains(next))
                    {
                        if (next == UINT32_MAX)
                            return failed(EWorkspaceError::CAPACITY, "legacy node identity");
                        ++next;
                    }
                    const auto id = next++;
                    nodes.emplace(id, LegacyNode{id});
                    layout.dock.nodes.push_back({id, EDockSplit::LEAF, 0, 0, 0.5, {slot.id}});
                    layout.dock.roots.push_back({id, placement.x, placement.y, placement.width, placement.height, true}
                    );
                }
            }
            return {};
        }
        bool knownTool(std::string_view type)
        {
            return type == "lux.editor.scene.v1" || type == "lux.editor.material.v1" ||
                   type == "lux.editor.flowforge.v1";
        }
        bool assetLocator(std::string_view payload)
        {
            if (!payload.starts_with("v1:") || payload.size() != 39)
                return false;
            const auto uuid = payload.substr(3);
            for (std::size_t i = 0; i < uuid.size(); ++i)
            {
                const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
                const char c = uuid[i];
                const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                if (separator ? c != '-' : !hex)
                    return false;
            }
            return true;
        }
    }
    std::string LegacyWorkspaceInput::sourceDigest() const
    {
        std::string manifest;
        for (const auto& file : layouts)
            manifest += file.relative_path + "\n" + file.version + "\n";
        if (settings)
            manifest += "settings\n" + settings->version;
        return digest(manifest);
    }
    WorkspaceResult<LegacyMigration> prepareLegacyMigration(LegacyWorkspaceInput input, WorkspaceLimits limits)
    {
        if (input.layouts.size() > limits.entries)
            return failed(EWorkspaceError::CAPACITY, "legacy count");
        std::ranges::sort(input.layouts, {}, &LegacyWorkspaceFile::relative_path);
        std::string_view previous;
        std::size_t total{};
        for (const auto& file : input.layouts)
        {
            if (file.bytes.size() > limits.file_bytes - total)
                return failed(EWorkspaceError::CAPACITY, "legacy input bytes");
            total += file.bytes.size();
            const bool is_invalid_path = !file.relative_path.starts_with(".lux/editor/layouts/") ||
                !file.relative_path.ends_with(".toml") || file.relative_path.size() <= 25;
            if (is_invalid_path)
                return failed(EWorkspaceError::INVALID_DATA, "legacy relative path");
            const auto name = std::string_view(file.relative_path).substr(20);
            const bool has_nested_path = name.find_first_of("/\\") != name.npos;
            const bool is_duplicate = file.relative_path == previous;
            if (has_nested_path || is_duplicate)
                return failed(EWorkspaceError::INVALID_DATA, "legacy relative path");
            if (file.version != storage::publicationDigest(file.bytes))
                return failed(EWorkspaceError::CONFLICT, "legacy input version");
            previous = file.relative_path;
        }
        if (input.settings)
        {
            if (input.settings->relative_path != ".lux/editor/settings.toml")
                return failed(EWorkspaceError::INVALID_DATA, "legacy settings path");
            if (input.settings->bytes.size() > limits.file_bytes)
                return failed(EWorkspaceError::CAPACITY, "legacy settings bytes");
            if (input.settings->version != storage::publicationDigest(input.settings->bytes))
                return failed(EWorkspaceError::CONFLICT, "legacy settings version");
        }
        LegacyMigration migration;
        // Each legacy file is an independent snapshot. Only explicit selection supplies recovery bindings.
        // Read settings first, but keep its contribution last in the original canonical input digest.
        const auto& settings = input.settings;
        std::string selected_file;
        if (settings)
        {
            const std::string text(reinterpret_cast<const char*>(settings->bytes.data()), settings->bytes.size());
            auto parsed = toml::parse(text);
            if (!parsed || parsed["version"].value_or(0) != 1)
                return failed(EWorkspaceError::INVALID_DATA, "legacy settings");
            auto selected = parsed["selected"].value<std::string>();
            if (!selected)
                return failed(EWorkspaceError::INVALID_DATA, "legacy selected");
            if (!selected->empty())
            {
                selected_file = *selected + ".toml";
                migration.preferences_.selected_layout = legacyId(selected_file);
            }
            else
                migration.diagnostics_.push_back("Legacy selected is empty; no snapshot supplies recovery.");
            migration.preferences_.opaque.push_back({"lux.workspace.legacy.settings", 1, settings->bytes});
        }
        else
            migration.diagnostics_.push_back("Legacy settings are absent; no snapshot supplies recovery.");
        std::size_t input_bytes{};
        bool selected_found{};
        for (const auto& file : input.layouts)
        {
            const bool is_selected = std::string_view(file.relative_path).substr(20) == selected_file;
            selected_found = selected_found || is_selected;
            const auto& relative = file.relative_path;
            const auto* input = &file;
            if (input->bytes.size() > limits.file_bytes - input_bytes)
                return failed(EWorkspaceError::CAPACITY, "migration aggregate input bytes");
            input_bytes += input->bytes.size();
            const std::string text(reinterpret_cast<const char*>(input->bytes.data()), input->bytes.size());
            auto parsed = toml::parse(text);
            if (!parsed || parsed["version"].value_or(0) != 1)
                return failed(EWorkspaceError::UNSUPPORTED_VERSION, relative);
            const auto* panes = parsed["panes"].as_array();
            auto dock = parsed["dock"].value<std::string>();
            if (!panes || !dock)
                return failed(EWorkspaceError::INVALID_DATA, relative);
            if (panes->size() > limits.entries)
                return failed(EWorkspaceError::CAPACITY, "legacy panes");
            DockLayout layout;
            layout.id = legacyId(std::string_view(file.relative_path).substr(20));
            layout.label = std::string(std::string_view(file.relative_path).substr(20, file.relative_path.size() - 25));
            layout.legacy_origin = LegacyOrigin{relative, input->version};
            layout.opaque.push_back({"lux.workspace.legacy.toml", 1, input->bytes});
            std::set<std::string> pane_ids;
            for (const auto& item : *panes)
            {
                const auto* row = item.as_table();
                if (!row)
                    return failed(EWorkspaceError::INVALID_DATA, "legacy pane");
                const auto type = (*row)["type"].value<std::string>();
                const auto id = (*row)["id"].value<std::string>();
                const auto payload = (*row)["payload"].value<std::string>();
                if (!type || !id || !payload || id->empty())
                    return failed(EWorkspaceError::INVALID_DATA, "legacy pane fields");
                if (!pane_ids.insert(*id).second)
                    return failed(EWorkspaceError::INVALID_DATA, "duplicate legacy pane");
                const auto bytes = std::as_bytes(std::span(*payload));
                LayoutSlot slot{
                    {static_cast<std::uint32_t>(layout.slots.size() + 1)},
                    views::ViewRestoreKey{"legacy:" + *id},
                    views::ViewTypeId{*type},
                    (*row)["visible"].value_or(true),
                    {1, {bytes.begin(), bytes.end()}}
                };
                if (knownTool(*type) && assetLocator(*payload))
                {
                    if (is_selected)
                        migration.recovery_.entries.push_back(
                            {slot.restore_key, slot.type, {{"asset:" + payload->substr(3), false}}, 0}
                        );
                    // Every snapshot keeps its original locator in the envelope, never in active view state.
                    slot.state.bytes.clear();
                }
                else if (knownTool(*type) && !payload->empty())
                    migration.diagnostics_.push_back("Unrecognized content locator retained: " + *id);
                layout.slots.push_back(std::move(slot));
            }
            auto geometry_result = geometry(layout, *dock, limits);
            if (!geometry_result)
                return lux::cxx::unexpected(geometry_result.error());
            auto valid = ValidatedLayout::validate(std::move(layout), limits);
            if (!valid)
                return lux::cxx::unexpected(valid.error());
            migration.layouts_.push_back(valid->value());
        }
        const bool selected_missing = !selected_file.empty() && !selected_found;
        if (selected_missing)
            migration.diagnostics_.push_back(
                "Legacy selected snapshot is missing; no snapshot supplies recovery: " + selected_file
            );
        migration.source_digest_ = input.sourceDigest();
        migration.recovery_.legacy_origin = LegacyOrigin{"legacy.workspace.v1", migration.source_digest_};
        migration.preferences_.legacy_origin = migration.recovery_.legacy_origin;
        auto valid_recovery = validateRecovery(migration.recovery_, limits);
        if (!valid_recovery)
            return lux::cxx::unexpected(valid_recovery.error());
        migration.diagnostics_.push_back("Legacy locators do not contain unsaved author changes; no content is opened.");
        return migration;
    }
}
