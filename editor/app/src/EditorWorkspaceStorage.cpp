#include <lux/engine/editor/detail/EditorWorkspace.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cctype>

namespace lux::editor::detail
{
    namespace
    {
        auto failed(std::string domain, const std::filesystem::path& path)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, std::move(domain), 0, path.string()}
            );
        }
        EditorResult<std::string> read(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            const auto size = file ? std::streamoff(file.tellg()) : -1;
            if (size < 0 || size > 16 * 1024 * 1024)
                return failed("workspace.read", path);
            std::string bytes(static_cast<std::size_t>(size), '\0');
            file.seekg(0);
            if (!file.read(bytes.data(), size))
                return failed("workspace.read", path);
            return bytes;
        }
        EditorResult<void> write(const std::filesystem::path& path, std::string_view bytes)
        {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
                return failed("workspace.directory", path.parent_path());
            auto staged = path;
            staged += ".next";
            {
                std::ofstream file(staged, std::ios::binary | std::ios::trunc);
                if (!file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())))
                    return failed("workspace.write", staged);
                file.flush();
                if (!file)
                    return failed("workspace.flush", staged);
                file.close();
                if (!file)
                    return failed("workspace.close", staged);
            }
            std::filesystem::rename(staged, path, error);
            if (error)
                return failed("workspace.replace", path);
            return {};
        }
        EditorResult<std::vector<std::string>> list(const std::filesystem::path& directory)
        {
            std::error_code error;
            if (!std::filesystem::exists(directory, error))
            {
                if (error)
                    return failed("workspace.list", directory);
                return std::vector<std::string>{};
            }
            std::vector<std::string> names;
            auto iterator = std::filesystem::directory_iterator(directory, error);
            const auto end = std::filesystem::directory_iterator{};
            while (!error && iterator != end)
            {
                const auto& path = iterator->path();
                if (path.extension() == ".toml" && validLayoutName(path.stem().string()))
                    names.push_back(path.stem().string());
                iterator.increment(error);
            }
            if (error)
                return failed("workspace.list", directory);
            std::ranges::sort(names);
            return names;
        }
        std::string encode(const WorkspaceData& data)
        {
            toml::array panes;
            for (const auto& pane : data.panes)
                panes.push_back(toml::table{
                    {"type", pane.type.name()},
                    {"id", pane.id.name()},
                    {"visible", pane.visible},
                    {"payload", pane.payload}
                });
            const auto bytes = data.dock.bytes();
            const std::string dock(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            toml::array windows;
            for (const auto& [id, visible] : data.child_visibility)
                windows.push_back(toml::table{{"id", id.name()}, {"visible", visible}});
            const toml::table
                table{{"version", 1}, {"panes", std::move(panes)}, {"windows", std::move(windows)}, {"dock", dock}};
            std::ostringstream encoded;
            encoded << table;
            return std::move(encoded).str();
        }
    }
    bool validLayoutName(std::string_view name) noexcept
    {
        if (name.empty() || name.size() > 80 || name.front() == '.' || name.back() == ' ' || name.back() == '.')
            return false;
        std::string base{name.substr(0, name.find('.'))};
        std::ranges::transform(base, base.begin(), [](unsigned char value) { return char(std::toupper(value)); });
        const bool reserved = base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
                              (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) &&
                               base.back() >= '1' && base.back() <= '9');
        if (reserved)
            return false;
        return std::ranges::all_of(name, [](unsigned char value) {
            return value >= 32 && std::string_view{"<>:\"/\\|?*"}.find(char(value)) == std::string_view::npos;
        });
    }
    EditorResult<WorkspaceData> readWorkspace(const std::filesystem::path& root, std::string name)
    {
        WorkspaceData data;
        const auto base = root / ".lux/editor";
        auto names = list(base / "layouts");
        if (!names)
            return lux::cxx::unexpected(names.error());
        data.names = std::move(*names);
        if (name.empty())
        {
            std::error_code error;
            if (std::filesystem::exists(base / "settings.toml", error))
            {
                const auto settings = read(base / "settings.toml");
                if (!settings)
                    return lux::cxx::unexpected(settings.error());
                auto parsed = toml::parse(*settings);
                if (!parsed || parsed["version"].value_or(0) != 1)
                    return failed("workspace.settings", base / "settings.toml");
                name = parsed["selected"].value_or(std::string{});
            }
            if (error)
                return failed("workspace.settings", base / "settings.toml");
            if (name.empty())
                return data;
        }
        if (!validLayoutName(name))
            return failed("workspace.name", std::filesystem::path{name});
        const auto path = base / "layouts" / (name + ".toml");
        auto bytes = read(path);
        if (!bytes)
            return lux::cxx::unexpected(bytes.error());
        auto parsed = toml::parse(*bytes);
        const bool valid_header = parsed && parsed["version"].value_or(0) == 1;
        if (!valid_header)
            return failed("workspace.format", path);
        const auto* panes = parsed["panes"].as_array();
        const auto dock = parsed["dock"].value<std::string>();
        if (!panes || panes->size() > 4096 || !dock || dock->empty())
            return failed("workspace.format", path);
        for (const auto& node : *panes)
        {
            const auto* table = node.as_table();
            if (!table)
                return failed("workspace.pane", path);
            const auto& row = *table;
            const auto type = row["type"].value<std::string>();
            const auto id = row["id"].value<std::string>();
            const auto payload = row["payload"].value<std::string>();
            const bool invalid = !type || type->empty() || !id || id->empty() || !payload;
            if (invalid)
                return failed("workspace.pane", path);
            const bool duplicate =
                std::ranges::any_of(data.panes, [&](const auto& pane) { return pane.id.name() == *id; });
            if (duplicate)
                return failed("workspace.duplicate", path);
            data.panes.push_back(
                {lux::ui::PaneTypeId{*type}, lux::ui::PaneId{*id}, row["visible"].value_or(true), *payload}
            );
        }
        const auto raw = std::as_bytes(std::span{dock->data(), dock->size()});
        if (const auto* windows = parsed["windows"].as_array())
        {
            if (windows->size() > 4096)
                return failed("workspace.windows", path);
            for (const auto& window : *windows)
            {
                const auto* table = window.as_table();
                if (!table)
                    return failed("workspace.window", path);
                const auto id = (*table)["id"].value<std::string>();
                const auto visible = (*table)["visible"].value<bool>();
                if (!id || id->empty() || !visible)
                    return failed("workspace.window", path);
                data.child_visibility.emplace_back(lux::ui::PaneId{*id}, *visible);
            }
        }
        data.dock = lux::ui::DockState{std::vector<std::byte>{raw.begin(), raw.end()}};
        data.selected = std::move(name);
        return data;
    }
    EditorResult<WorkspaceData> writeWorkspace(
        const std::filesystem::path& root,
        EWorkspaceAction action,
        std::string name,
        std::string new_name,
        WorkspaceData data
    )
    {
        const auto base = root / ".lux/editor";
        const auto target = base / "layouts" / (name + ".toml");
        const bool invalid_name = action != EWorkspaceAction::DEFAULT && !validLayoutName(name);
        const bool invalid_new_name = action == EWorkspaceAction::RENAME && !validLayoutName(new_name);
        if (invalid_name || invalid_new_name)
            return failed("workspace.name", target);
        std::error_code error;
        if (action == EWorkspaceAction::SAVE)
        {
            if (auto saved = write(target, encode(data)); !saved)
                return lux::cxx::unexpected(saved.error());
            data.selected = name;
        }
        else if (action == EWorkspaceAction::RENAME)
        {
            const auto destination = base / "layouts" / (new_name + ".toml");
            if (std::filesystem::exists(destination, error) || error)
                return failed("workspace.conflict", destination);
            std::filesystem::rename(target, destination, error);
            if (error)
                return failed("workspace.rename", target);
            if (data.selected == name)
                data.selected = new_name;
        }
        else if (action == EWorkspaceAction::REMOVE)
        {
            if (!std::filesystem::remove(target, error) || error)
                return failed("workspace.remove", target);
            if (data.selected == name)
                data.selected.clear();
        }
        else if (action == EWorkspaceAction::DEFAULT)
            data.selected.clear();
        std::ostringstream settings;
        settings << toml::table{{"version", 1}, {"selected", data.selected}};
        if (auto saved = write(base / "settings.toml", settings.str()); !saved)
            return lux::cxx::unexpected(saved.error());
        auto names = list(base / "layouts");
        if (!names)
            return lux::cxx::unexpected(names.error());
        data.names = std::move(*names);
        return data;
    }
}
