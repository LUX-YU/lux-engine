#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool value, std::source_location at = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "scalar contract failed at %u\n", at.line());
            std::abort();
        }
    }

    const DataOutPin& addInput(FlowGraph& graph, const meta::RefType& type, std::string name)
    {
        auto initial = meta::RuntimeObject::defaultOf(type);
        require(initial.has_value());
        const auto variable = graph.addVariable(name, &type, std::move(*initial));
        require(variable != 0U);
        auto input = std::make_unique<GetVariableNode>(variable, DataPinInfo{name, &type});
        const auto* pointer = input.get();
        require(graph.addNodes(std::move(input)) != 0U);
        return pointer->valuePin();
    }

    void addBinary(FlowGraph& graph, ENodeOperation operation, const meta::RefType& type, std::size_t& count)
    {
        auto expression = std::make_unique<BinaryOpNode>(operation, &type);
        auto function = std::make_unique<OnEventNode>("binary_" + std::to_string(count++));
        const auto* result_type = expression->result().info().type;
        auto initial = meta::RuntimeObject::defaultOf(*result_type);
        require(initial.has_value());
        const auto variable = graph.addVariable("result_" + std::to_string(count), result_type, std::move(*initial));
        require(variable != 0U);
        auto result = std::make_unique<SetVariableNode>(variable, DataPinInfo{"result", result_type});
        auto* f = function.get();
        auto* x = expression.get();
        auto* r = result.get();
        require(graph.addNodes(std::move(function)) != 0U);
        require(graph.addNodes(std::move(expression)) != 0U);
        require(graph.addNodes(std::move(result)) != 0U);
        require(graph.addExport({{count}, f->id(), count, {}}));
        require(f->execOutPin().linkTo(&r->execInPin()) == ELinkError::SUCCESS);
        const auto& lhs = addInput(graph, type, "lhs_" + std::to_string(count));
        const auto& rhs = addInput(graph, type, "rhs_" + std::to_string(count));
        require(graph.connect(*graph.findPin(lhs.id()), *graph.findPin(x->lhs().id())) == ELinkError::SUCCESS);
        require(graph.connect(*graph.findPin(rhs.id()), *graph.findPin(x->rhs().id())) == ELinkError::SUCCESS);
        require(graph.findPin(x->result().id())->linkTo(graph.findPin(r->valueIn().id())) == ELinkError::SUCCESS);
    }

    void addUnary(FlowGraph& graph, ENodeOperation operation, const meta::RefType& type, std::size_t& count)
    {
        auto expression = std::make_unique<UnaryOpNode>(operation, &type);
        auto function = std::make_unique<OnEventNode>("unary_" + std::to_string(count++));
        const auto* result_type = expression->result().info().type;
        auto initial = meta::RuntimeObject::defaultOf(*result_type);
        require(initial.has_value());
        const auto variable = graph.addVariable("result_" + std::to_string(count), result_type, std::move(*initial));
        require(variable != 0U);
        auto result = std::make_unique<SetVariableNode>(variable, DataPinInfo{"result", result_type});
        auto* f = function.get();
        auto* x = expression.get();
        auto* r = result.get();
        require(graph.addNodes(std::move(function)) != 0U);
        require(graph.addNodes(std::move(expression)) != 0U);
        require(graph.addNodes(std::move(result)) != 0U);
        require(graph.addExport({{count}, f->id(), count, {}}));
        require(f->execOutPin().linkTo(&r->execInPin()) == ELinkError::SUCCESS);
        const auto& input = addInput(graph, type, "value_" + std::to_string(count));
        require(graph.connect(*graph.findPin(input.id()), *graph.findPin(x->operand().id())) == ELinkError::SUCCESS);
        require(graph.findPin(x->result().id())->linkTo(graph.findPin(r->valueIn().id())) == ELinkError::SUCCESS);
    }
} // namespace

int main(int argc, char** argv)
{
    meta::meta_module_init();
    {
        FlowGraph graph;
        const std::array types{
            &meta::ref_type_of_v<std::int8_t>,
            &meta::ref_type_of_v<std::uint8_t>,
            &meta::ref_type_of_v<std::int16_t>,
            &meta::ref_type_of_v<std::uint16_t>,
            &meta::ref_type_of_v<std::int32_t>,
            &meta::ref_type_of_v<std::uint32_t>,
            &meta::ref_type_of_v<std::int64_t>,
            &meta::ref_type_of_v<std::uint64_t>,
            &meta::ref_type_of_v<float>,
            &meta::ref_type_of_v<double>
        };
        const std::array operations{
            ENodeOperation::ADD,
            ENodeOperation::SUBTRACT,
            ENodeOperation::MULTIPLY,
            ENodeOperation::DIVIDE,
            ENodeOperation::MODULO,
            ENodeOperation::CMP_EQ,
            ENodeOperation::CMP_NE,
            ENodeOperation::CMP_LT,
            ENodeOperation::CMP_LE,
            ENodeOperation::CMP_GT,
            ENodeOperation::CMP_GE
        };
        std::size_t count{};
        for (const auto* type : types)
        {
            for (const auto operation : operations)
            {
                addBinary(graph, operation, *type, count);
            }
            addUnary(graph, ENodeOperation::NEGATE, *type, count);
        }
        addBinary(graph, ENodeOperation::LOGICAL_AND, meta::ref_type_of_v<bool>, count);
        addBinary(graph, ENodeOperation::LOGICAL_OR, meta::ref_type_of_v<bool>, count);
        addUnary(graph, ENodeOperation::LOGICAL_NOT, meta::ref_type_of_v<bool>, count);
        require(count == 123U);
        auto compiled = compileFlowForgeObject(graph, {.module_name = "scalar_lowering"});
        if (!compiled)
        {
            std::fprintf(stderr, "%s\n", compiled.error().message.c_str());
        }
        require(compiled.has_value() && !compiled->object.empty());
        if (argc == 2)
        {
            std::ofstream output(argv[1], std::ios::binary);
            output.write(reinterpret_cast<const char*>(compiled->object.data()), compiled->object.size());
            require(output.good());
        }
        std::printf(
            "PASS actual Flow AOT: %zu observable scalar exports, %zu object bytes\n",
            count,
            compiled->object.size()
        );
    }
    meta::meta_module_deinit();
}
