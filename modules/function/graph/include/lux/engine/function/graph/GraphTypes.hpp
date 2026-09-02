#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace lux::graph
{
    template<class Tag>
    struct StableId final
    {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return value != 0U;
        }

        [[nodiscard]] constexpr auto operator<=>(const StableId&) const noexcept = default;
    };

    struct NodeIdTag final {};
    struct PinIdTag final {};
    struct NodeTypeIdTag final {};
    struct PinSemanticIdTag final {};

    using NodeId = StableId<NodeIdTag>;
    using PinId = StableId<PinIdTag>;
    using NodeTypeId = StableId<NodeTypeIdTag>;
    using PinSemanticId = StableId<PinSemanticIdTag>;

    enum class EPinDirection : std::uint8_t
    {
        INPUT,
        OUTPUT,
    };

    inline constexpr std::uint8_t kUnlimitedFan = 0xFFU;

    struct NodeRecord final
    {
        NodeId id;
        NodeTypeId type;

        [[nodiscard]] constexpr bool operator==(const NodeRecord&) const noexcept = default;
    };

    struct PinRecord final
    {
        PinId id;
        NodeId owner;
        EPinDirection direction{EPinDirection::INPUT};
        std::uint8_t fan_cap{1U};
        PinSemanticId semantic;

        [[nodiscard]] constexpr bool operator==(const PinRecord&) const noexcept = default;
    };

    struct LinkRecord final
    {
        PinId from;
        PinId to;

        [[nodiscard]] constexpr bool operator==(const LinkRecord&) const noexcept = default;
    };

    struct DetachedPin final
    {
        PinRecord pin;
        std::vector<LinkRecord> links;
    };

    struct DetachedNode final
    {
        NodeRecord node;
        std::vector<PinRecord> pins;
        std::vector<LinkRecord> links;
    };

    struct GraphNodeLayout final
    {
        float x{};
        float y{};
        bool placed{};

        [[nodiscard]] constexpr bool operator==(const GraphNodeLayout&) const noexcept = default;
    };

    struct GraphLayoutEntry final
    {
        NodeId node;
        GraphNodeLayout layout;

        [[nodiscard]] constexpr bool operator==(const GraphLayoutEntry&) const noexcept = default;
    };
} // namespace lux::graph

namespace std
{
    template<class Tag>
    struct hash<lux::graph::StableId<Tag>> final
    {
        [[nodiscard]] size_t operator()(lux::graph::StableId<Tag> id) const noexcept
        {
            return hash<std::uint64_t>{}(id.value);
        }
    };
} // namespace std
