#include <lux/engine/scene/SceneStreamingQuery.hpp>

#include <algorithm>
#include <array>

namespace lux::scene
{
    namespace
    {
        constexpr auto kInvalid = error::errorId("lux.scene.content.invalid");

        cxx::expected<void, error::Error>
        querySpatialIndex(
            const SceneSpatialIndex& index, const spatial::Spatial3DQueryBounds& query,
            std::vector<std::uint32_t>& output
        ) noexcept
        {
            if (index.nodes.empty()) return {};
            const bool three = index.dimension == ESceneSpatialDimension::THREE;
            const auto overlaps = [&](const spatial::Spatial3DQueryBounds& bounds) noexcept
            {
                return bounds.minimum.x < query.maximum.x && bounds.maximum.x >= query.minimum.x &&
                    bounds.minimum.y < query.maximum.y && bounds.maximum.y >= query.minimum.y &&
                    (!three || (bounds.minimum.z < query.maximum.z && bounds.maximum.z >= query.minimum.z));
            };
            std::array<std::uint32_t, 64> stack{};
            std::size_t pending = 1;
            std::size_t visited{};
            while (pending)
            {
                const auto position = stack[--pending];
                if (position >= index.nodes.size() || ++visited > index.nodes.size())
                    return cxx::unexpected(error::Error{kInvalid});
                const auto& node = index.nodes[position];
                if (!overlaps(node.bounds)) continue;
                if (!node.count)
                {
                    const bool invalid_children = node.first <= position ||
                        std::uint64_t(node.first) + 1 >= index.nodes.size() || pending + 2 > stack.size();
                    if (invalid_children) return cxx::unexpected(error::Error{kInvalid});
                    stack[pending++] = node.first + 1;
                    stack[pending++] = node.first;
                }
                else
                {
                    const bool invalid_range = node.first > index.entries.size() ||
                        node.count > index.entries.size() - node.first;
                    if (invalid_range) return cxx::unexpected(error::Error{kInvalid});
                    const auto entries = std::span(index.entries).subspan(node.first, node.count);
                    for (const auto& entry : entries)
                    {
                        if (overlaps(entry.bounds)) output.push_back(entry.group);
                    }
                }
            }
            return {};
        }
    }

    cxx::expected<void, error::Error>
    querySceneGroups(
        const SceneStreamingData& data, std::uint32_t geometry, const spatial::Spatial3DQueryBounds& bounds,
        std::vector<std::uint32_t>& output
    ) noexcept
    {
        if (geometry >= data.geometry_indexes.size()) return cxx::unexpected(error::Error{kInvalid, {geometry}});
        const auto& index = data.geometry_indexes[geometry];
        const bool two = index.dimension == ESceneSpatialDimension::TWO;
        const bool three = index.dimension == ESceneSpatialDimension::THREE;
        const bool invalid_bounds = !math::isFinite(bounds.minimum) || !math::isFinite(bounds.maximum) ||
            bounds.minimum.x >= bounds.maximum.x || bounds.minimum.y >= bounds.maximum.y ||
            (three && bounds.minimum.z >= bounds.maximum.z);
        if ((!two && !three) || invalid_bounds) return cxx::unexpected(error::Error{kInvalid, {geometry}});
        output.clear();
        auto queried = querySpatialIndex(index, bounds, output);
        if (!queried) return queried;
        std::ranges::sort(output);
        output.erase(std::unique(output.begin(), output.end()), output.end());
        if (!output.empty() && output.back() >= data.groups.size()) return cxx::unexpected(error::Error{kInvalid});
        return {};
    }
} // namespace lux::scene
