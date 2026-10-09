#pragma once

#include <lux/engine/flowforge/FlowNodeCatalog.hpp>

namespace lux::flowforge
{
    // Semantic operand declaration only; identity, pins and connections belong to the graph.
    // Create through the definition, then set this field before admitting the node.
    struct ScalarNodePayload final
    {
        const meta::RefType* operand_type{};
    };

    // Builtins use the same immutable definitions and compile/codec callbacks as extensions.
    [[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC std::vector<FlowNodeRegistration> scalarNodeRegistrations(
        object::CodeLease code = object::CodeLease::builtin()
    ) noexcept;
} // namespace lux::flowforge
