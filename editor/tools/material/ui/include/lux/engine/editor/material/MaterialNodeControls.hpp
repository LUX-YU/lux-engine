#pragma once
#include <lux/engine/material/graph/MaterialGraph.hpp>
namespace lux::editor::material
{
    // Operate only on a locally owned gesture value, never directly on an author's node.
    [[nodiscard]] std::unique_ptr<lux::material::Node> makeMaterialNode(lux::material::EMatNodeKind);
    [[nodiscard]] bool editMaterialValueType(const char*, lux::material::EValueType&);
    [[nodiscard]] bool
    editMaterialNodePayload(lux::material::Node&, std::span<const lux::material::TextureSlotDecl>, std::span<const lux::material::ParamSlotDecl>);
}
