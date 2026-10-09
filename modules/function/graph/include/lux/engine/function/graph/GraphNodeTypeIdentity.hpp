#pragma once

#include <lux/cxx/algorithm/hash.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>

#include <string>
#include <string_view>

namespace lux::graph
{
    [[nodiscard]] constexpr NodeTypeId nodeTypeId(std::string_view canonical_name) noexcept
    {
        return NodeTypeId{cxx::algorithm::fnv1a(canonical_name)};
    }

    // Semantic type identity, independent of node instances and runtime catalog slots.
    struct GraphNodeTypeIdentity final
    {
        NodeTypeId id;
        std::string canonical_name;
        std::uint32_t version{1};
    };
} // namespace lux::graph
