#include <algorithm>
#include <cmath>
#include <lux/engine/editor/EditorLayout.hpp>
#include <lux/engine/platform/FileIo.hpp>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace lux::editor
{
    namespace
    {
        constexpr std::size_t MaxLayoutBytes = 4 * 1024 * 1024;

        ProjectFailure fileFailure(platform::FileFailure failure) noexcept
        {
            using E = platform::EFileIoError;
            switch (failure.code)
            {
            case E::CANCELLED:
                return {EProjectError::CANCELLED, 0, failure.system};
            case E::PUBLICATION_UNKNOWN:
                return {EProjectError::PUBLICATION_UNKNOWN, 0, failure.system};
            case E::LIMIT:
                return {EProjectError::LIMIT, 0, failure.system};
            case E::INVALID_PATH:
                return {EProjectError::INVALID_PATH, 0, failure.system};
            default:
                return {EProjectError::IO, 0, failure.system};
            }
        }

        bool fields(const nlohmann::json& value, std::initializer_list<std::string_view> names) noexcept
        {
            if (!value.is_object() || value.size() != names.size())
            {
                return false;
            }
            for (auto name : names)
            {
                if (!value.contains(name))
                {
                    return false;
                }
            }
            return true;
        }

        std::uint32_t index(const nlohmann::json& value)
        {
            if (!value.is_number_unsigned() || value.get<std::uint64_t>() > UINT32_MAX)
            {
                return UINT32_MAX;
            }
            return value.get<std::uint32_t>();
        }
    } // namespace

    ProjectLayout defaultProjectLayout(EditorLayout panes)
    {
        ProjectLayout layout;
        layout.panes = std::move(panes);
        if (layout.panes.empty())
        {
            return layout;
        }
        layout.nodes.resize(7);
        layout.nodes[0] = {ELayoutSplit::HORIZONTAL, 1, 2, 0.22F};
        layout.nodes[2] = {ELayoutSplit::VERTICAL, 3, 4, 0.74F};
        layout.nodes[3] = {ELayoutSplit::HORIZONTAL, 5, 6, 0.72F};
        for (const auto& pane : layout.panes)
        {
            const auto node = pane.region == EPaneRegion::LEFT     ? 1u
                              : pane.region == EPaneRegion::RIGHT  ? 6u
                              : pane.region == EPaneRegion::BOTTOM ? 4u
                                                                   : 5u;
            layout.nodes[node].panes.push_back(pane.name);
        }
        layout.surfaces.push_back({0});
        return layout;
    }

    ProjectResult<void> validateProjectLayout(const ProjectLayout& layout) noexcept
    {
        const auto invalid = [] { return cxx::unexpected(ProjectFailure{EProjectError::INVALID_LAYOUT}); };
        if (layout.version != 1)
        {
            return cxx::unexpected(ProjectFailure{EProjectError::UNSUPPORTED_VERSION});
        }
        std::unordered_set<std::string_view> keys;
        keys.reserve(layout.panes.size());
        for (const auto& pane : layout.panes)
        {
            const bool invalid_key = pane.name.empty() || pane.name.find('\0') != pane.name.npos;
            const bool invalid_type = pane.type.empty() || pane.type.find('\0') != pane.type.npos;
            const bool invalid_region = pane.region > EPaneRegion::BOTTOM;
            if (invalid_key || invalid_type || invalid_region || !keys.insert(pane.name).second)
            {
                return invalid();
            }
        }
        std::vector<std::uint8_t> visited(layout.nodes.size());
        std::vector<std::uint32_t> pending;
        pending.reserve(layout.nodes.size());
        unsigned main_surfaces{};
        for (const auto& surface : layout.surfaces)
        {
            const bool invalid_bounds = !std::isfinite(surface.x) || !std::isfinite(surface.y) ||
                                        !std::isfinite(surface.width) || !std::isfinite(surface.height) ||
                                        surface.width <= 0 || surface.height <= 0;
            if (invalid_bounds || (!surface.floating && ++main_surfaces > 1))
            {
                return invalid();
            }
            pending.push_back(surface.node);
            while (!pending.empty())
            {
                const auto ordinal = pending.back();
                pending.pop_back();
                if (ordinal >= layout.nodes.size() || visited[ordinal])
                {
                    return invalid();
                }
                visited[ordinal] = 1;
                const auto& node = layout.nodes[ordinal];
                if (node.split == ELayoutSplit::LEAF)
                {
                    if (node.first != UINT32_MAX || node.second != UINT32_MAX)
                    {
                        return invalid();
                    }
                    for (const auto& key : node.panes)
                    {
                        if (!keys.erase(key))
                        {
                            return invalid();
                        }
                    }
                }
                else
                {
                    const bool invalid_split =
                        node.split != ELayoutSplit::HORIZONTAL && node.split != ELayoutSplit::VERTICAL;
                    const bool invalid_ratio = !std::isfinite(node.ratio) || node.ratio <= 0 || node.ratio >= 1;
                    if (invalid_split || invalid_ratio || !node.panes.empty())
                    {
                        return invalid();
                    }
                    pending.push_back(node.second);
                    pending.push_back(node.first);
                }
            }
        }
        if (!keys.empty() || std::ranges::find(visited, 0) != visited.end())
        {
            return invalid();
        }
        return {};
    }

    ProjectResult<ProjectLayout> decodeProjectLayout(std::string_view bytes) noexcept
    try
    {
        if (bytes.size() > MaxLayoutBytes)
        {
            return cxx::unexpected(ProjectFailure{EProjectError::LIMIT});
        }
        bool duplicate{};
        std::vector<std::unordered_set<std::string>> names;
        auto input = nlohmann::json::parse(
            bytes,
            [&](int, auto event, nlohmann::json& value)
            {
                using E = nlohmann::json::parse_event_t;
                if (event == E::object_start)
                {
                    names.emplace_back();
                }
                if (event == E::key && !names.back().insert(value.get<std::string>()).second)
                {
                    duplicate = true;
                }
                if (event == E::object_end)
                {
                    names.pop_back();
                }
                return true;
            },
            false
        );
        const auto invalid = [] { return cxx::unexpected(ProjectFailure{EProjectError::INVALID_LAYOUT}); };
        const bool valid_root = !duplicate && fields(input, {"version", "panes", "nodes", "surfaces"});
        if (!valid_root || !input["panes"].is_array() || !input["nodes"].is_array() || !input["surfaces"].is_array())
        {
            return invalid();
        }
        ProjectLayout layout;
        layout.version = index(input["version"]);
        for (const auto& pane : input["panes"])
        {
            if (!fields(pane, {"type", "name", "title"}))
            {
                return invalid();
            }
            layout.panes.push_back(
                {pane.at("type").get<std::string>(),
                 pane.at("name").get<std::string>(),
                 pane.at("title").get<std::string>()}
            );
        }
        for (const auto& node : input["nodes"])
        {
            if (!fields(node, {"split", "first", "second", "ratio", "panes"}) || !node["panes"].is_array())
            {
                return invalid();
            }
            const auto split = node.at("split").get<std::string>();
            if (split != "leaf" && split != "horizontal" && split != "vertical")
            {
                return invalid();
            }
            ProjectLayoutNode decoded;
            decoded.split = split == "leaf"         ? ELayoutSplit::LEAF
                            : split == "horizontal" ? ELayoutSplit::HORIZONTAL
                                                    : ELayoutSplit::VERTICAL;
            const bool invalid_first = !node["first"].is_null() && index(node["first"]) == UINT32_MAX;
            const bool invalid_second = !node["second"].is_null() && index(node["second"]) == UINT32_MAX;
            if (invalid_first || invalid_second)
            {
                return invalid();
            }
            decoded.first = node["first"].is_null() ? UINT32_MAX : index(node["first"]);
            decoded.second = node["second"].is_null() ? UINT32_MAX : index(node["second"]);
            decoded.ratio = node.at("ratio").get<float>();
            decoded.panes = node.at("panes").get<std::vector<std::string>>();
            layout.nodes.push_back(std::move(decoded));
        }
        for (const auto& surface : input["surfaces"])
        {
            if (!fields(surface, {"node", "x", "y", "width", "height", "floating"}))
            {
                return invalid();
            }
            layout.surfaces.push_back(
                {index(surface["node"]),
                 surface.at("x").get<float>(),
                 surface.at("y").get<float>(),
                 surface.at("width").get<float>(),
                 surface.at("height").get<float>(),
                 surface.at("floating").get<bool>()}
            );
        }
        if (auto valid = validateProjectLayout(layout); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return layout;
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (const nlohmann::json::exception&)
    {
        return cxx::unexpected(ProjectFailure{EProjectError::INVALID_LAYOUT});
    }

    ProjectResult<std::string> encodeProjectLayout(const ProjectLayout& layout) noexcept
    try
    {
        if (auto valid = validateProjectLayout(layout); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        nlohmann::json output{
            {"version", layout.version},
            {"panes", nlohmann::json::array()},
            {"nodes", nlohmann::json::array()},
            {"surfaces", nlohmann::json::array()}
        };
        for (const auto& pane : layout.panes)
        {
            output["panes"].push_back({{"type", pane.type}, {"name", pane.name}, {"title", pane.title}});
        }
        for (const auto& node : layout.nodes)
        {
            output["nodes"].push_back(
                {{"split",
                  node.split == ELayoutSplit::LEAF         ? "leaf"
                  : node.split == ELayoutSplit::HORIZONTAL ? "horizontal"
                                                           : "vertical"},
                 {"first", node.first == UINT32_MAX ? nlohmann::json{} : nlohmann::json(node.first)},
                 {"second", node.second == UINT32_MAX ? nlohmann::json{} : nlohmann::json(node.second)},
                 {"ratio", node.ratio},
                 {"panes", node.panes}}
            );
        }
        for (const auto& surface : layout.surfaces)
        {
            output["surfaces"].push_back(
                {{"node", surface.node},
                 {"x", surface.x},
                 {"y", surface.y},
                 {"width", surface.width},
                 {"height", surface.height},
                 {"floating", surface.floating}}
            );
        }
        auto bytes = output.dump(2);
        if (bytes.size() > MaxLayoutBytes)
        {
            return cxx::unexpected(ProjectFailure{EProjectError::LIMIT});
        }
        return bytes;
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (const nlohmann::json::exception&)
    {
        return cxx::unexpected(ProjectFailure{EProjectError::INVALID_LAYOUT});
    }

    ProjectResult<ProjectLayout> readProjectLayout(const std::filesystem::path& path, std::stop_token stop) noexcept
    {
        auto bytes = platform::readFileBounded(path, MaxLayoutBytes, stop);
        if (!bytes)
        {
            return cxx::unexpected(fileFailure(bytes.error()));
        }
        return decodeProjectLayout({reinterpret_cast<const char*>(bytes->data()), bytes->size()});
    }

    ProjectResult<void>
    writeProjectLayout(const std::filesystem::path& path, const ProjectLayout& layout, std::stop_token stop) noexcept
    {
        auto bytes = encodeProjectLayout(layout);
        if (!bytes)
        {
            return cxx::unexpected(bytes.error());
        }
        auto written =
            platform::writeFileAtomic(path, std::as_bytes(std::span{*bytes}), platform::EFileWriteMode::REPLACE, stop);
        return written ? ProjectResult<void>{} : cxx::unexpected(fileFailure(written.error()));
    }
} // namespace lux::editor
