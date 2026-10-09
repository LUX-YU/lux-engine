#pragma once

#include <lux/engine/material/MaterialNodeCatalog.hpp>

#include <unordered_map>

namespace lux::material
{
    class MaterialGraph;

    namespace detail
    {
        struct MaterialCompilePin final
        {
            graph::PinId id;
            MaterialPinDeclaration declaration;
        };

        struct MaterialCompileNode final
        {
            std::vector<MaterialCompilePin> inputs;
            std::vector<graph::PinId> outputs;
        };

        // Disposable compilation indexes, never an authoring authority. Built once from the frozen graph.
        struct MaterialCompileGraph final
        {
            std::unordered_map<graph::NodeId, MaterialCompileNode> nodes;
            std::unordered_map<graph::PinId, const graph::PinRecord*> pins;
            std::unordered_map<graph::PinId, graph::PinId> incoming;
        };

        [[nodiscard]] MaterialNodeResult<MaterialCompileGraph> validateMaterialGraph(
            const MaterialGraph&,
            shadergen::ShaderIR& resources
        ) noexcept;
    } // namespace detail
} // namespace lux::material
