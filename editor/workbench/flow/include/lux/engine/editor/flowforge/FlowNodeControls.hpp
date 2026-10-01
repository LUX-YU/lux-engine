#pragma once
#include <lux/engine/flowforge/graph/FlowSource.hpp>
namespace lux::editor::flowforge
{
    using VFlowScalar = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double>;
    [[nodiscard]] std::unique_ptr<lux::flowforge::Node> makeFlowNode(lux::flowforge::ENodeOperation);
    [[nodiscard]] std::unique_ptr<lux::flowforge::Node> chooseRegisteredFlowNode(const lux::flowforge::
                                                                                     FlowSourceEnvironment&);
    [[nodiscard]] VFlowScalar flowScalar(const lux::flowforge::FlowSourceLiteral&);
    [[nodiscard]] lux::flowforge::FlowSourceLiteral flowScalarLiteral(const VFlowScalar&);
    [[nodiscard]] std::span<const lux::meta::RefType* const> flowScalarTypes();
    [[nodiscard]] bool editFlowScalar(VFlowScalar&);
    [[nodiscard]] bool editFlowArguments(const char*, std::vector<lux::flowforge::FlowSourceArgument>&);
}
