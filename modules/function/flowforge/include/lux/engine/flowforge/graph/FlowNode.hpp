#pragma once

#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/function/graph/GraphTopology.hpp>
#include <lux/engine/meta/RuntimeObject.hpp>

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lux::flowforge
{
    using graph::NodeId;
    using graph::PinId;

    using VFlowGraphFailure = std::variant<graph::GraphTopologyFailure, FlowForgeFailure, meta::ERuntimeObjectError>;
    template <class T> using FlowGraphResult = cxx::expected<T, VFlowGraphFailure>;

    // Owns semantics and their immutable provider. Identity and membership are exclusively store keys.
    struct LUX_ENGINE_FLOWFORGE_PUBLIC FlowNode final
    {
        std::shared_ptr<const FlowNodeType> definition;
        std::string name;
        std::string creator;
        FlowNodePayload payload;

        [[nodiscard]] FlowForgeResult<FlowNode> clone() const noexcept;
    };

    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowForgeResult<FlowNode> createFlowNode(
        std::shared_ptr<const FlowNodeType>,
        FlowNodePayload
    ) noexcept;

    // No Node pointer, PinId, direction, fan limit or graph membership. Execution/data is a
    // domain value role; all structural facts are read from the sole GraphTopology.
    struct LUX_ENGINE_FLOWFORGE_PUBLIC FlowPinPayload final
    {
        std::string name;
        EFlowPinRole role{EFlowPinRole::DATA};
        const meta::RefType* type{};
        bool allow_default{};
        bool necessary{};
        meta::RuntimeObject default_value;

        [[nodiscard]] cxx::expected<FlowPinPayload, meta::ERuntimeObjectError> clone() const noexcept;
        [[nodiscard]] cxx::expected<void, meta::ERuntimeObjectError> resetDefault() noexcept;
        [[nodiscard]] bool setDefault(meta::RuntimeObject) noexcept;
    };

    // Structural records here are detached restoration values, never a second live topology.
    struct FlowPinEntry final
    {
        graph::PinRecord record;
        FlowPinPayload value;
    };

    struct LUX_ENGINE_FLOWFORGE_PUBLIC FlowNodeSnapshot final
    {
        NodeId id;
        FlowNode value;
        std::vector<FlowPinEntry> pins;
        std::vector<graph::LinkRecord> links;
        std::optional<graph::GraphNodeLayout> layout;

        FlowNodeSnapshot() noexcept = default;
        ~FlowNodeSnapshot() = default;
        FlowNodeSnapshot(FlowNodeSnapshot&&) noexcept = default;
        FlowNodeSnapshot& operator=(FlowNodeSnapshot&&) noexcept;
        FlowNodeSnapshot(const FlowNodeSnapshot&) = delete;
        FlowNodeSnapshot& operator=(const FlowNodeSnapshot&) = delete;
    };
} // namespace lux::flowforge
