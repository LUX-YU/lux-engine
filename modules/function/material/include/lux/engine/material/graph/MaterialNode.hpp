#pragma once

#include <lux/engine/material/MaterialNodeCatalog.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lux::material
{
    // Definition and semantic payload only. The store key, never this value, identifies a node.
    struct LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialNode final
    {
        std::shared_ptr<const MaterialNodeType> definition;
        std::string name;
        MaterialNodePayload payload;

        [[nodiscard]] MaterialNodeResult<MaterialNode> clone() const noexcept;
    };

    // Editable pin metadata; identity, owner, direction and connections belong to GraphTopology.
    struct MaterialPinPayload final
    {
        std::string name;
        EValueType type{EValueType::FLOAT};
        std::array<float, 4> constant{};

        [[nodiscard]] bool operator==(const MaterialPinPayload&) const = default;
    };

    // An owned restoration value. Structural records are snapshots, not live payload fields.
    struct MaterialPinEntry final
    {
        graph::PinRecord record;
        MaterialPinPayload value;
    };

    struct MaterialNodeSnapshot final
    {
        graph::NodeId id;
        MaterialNode value;
        std::vector<MaterialPinEntry> pins;
        std::vector<graph::LinkRecord> links;
        std::optional<graph::GraphNodeLayout> layout;
    };
} // namespace lux::material
