#include "FlowGraphFixture.hpp"
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/ObjectNodes.hpp>
#include <lux/engine/flowforge/ScalarNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

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

    FlowNode scalar(std::string_view name, const meta::RefType& type)
    {
        FlowNodeCatalog catalog;
        require(catalog.add(scalarNodeRegistrations()).has_value());
        auto definition = catalog.find(graph::nodeTypeId(name));
        require(definition != nullptr);
        auto payload = definition->create();
        require(payload.has_value());
        payload->get<ScalarNodePayload>()->operand_type = &type;
        auto node = createFlowNode(definition, std::move(*payload));
        require(node.has_value());
        return std::move(*node);
    }

    PinId addInput(FlowGraph& graph, test::GraphFixture& fixture, const meta::RefType& type, std::string name)
    {
        auto initial = meta::RuntimeObject::defaultOf(type);
        require(initial.has_value());
        const auto variable = graph.addVariable(name, &type, std::move(*initial));
        require(variable != 0U);
        const auto id = fixture.add<GetVariablePayload>(graph, "lux.flow.get_variable", {variable, &type});
        return test::dataOut(graph, id);
    }

    void addExpression(
        FlowGraph& graph,
        std::string_view operation,
        const meta::RefType& type,
        std::size_t& count,
        bool binary
    )
    {
        test::GraphFixture fixture;
        auto expression = scalar(operation, type);
        const auto name = std::string(binary ? "binary_" : "unary_") + std::to_string(count++);
        auto schema = expression.definition->describePins(expression.payload);
        require(schema.has_value());
        const auto* result_type = schema->back().type;
        auto initial = meta::RuntimeObject::defaultOf(*result_type);
        require(initial.has_value());
        const auto variable = graph.addVariable("result_" + std::to_string(count), result_type, std::move(*initial));
        require(variable != 0U);
        const auto f = fixture.add<EventEntryPayload>(graph, "lux.flow.event", {}, name);
        auto expression_id = graph.addNode(std::move(expression));
        require(expression_id.has_value());
        const auto x = *expression_id;
        const auto r = fixture.add<SetVariablePayload>(graph, "lux.flow.set_variable", {variable, result_type});
        require(graph.addExport({{count}, f, count, {}}));
        require(graph.connect(test::execOut(graph, f), test::execIn(graph, r)).has_value());
        const auto input =
            addInput(graph, fixture, type, std::string(binary ? "lhs_" : "value_") + std::to_string(count));
        require(graph.connect(input, test::dataIn(graph, x)).has_value());
        if (binary)
        {
            const auto rhs = addInput(graph, fixture, type, "rhs_" + std::to_string(count));
            require(graph.connect(rhs, test::dataIn(graph, x, 1)).has_value());
        }
        require(graph.connect(test::dataOut(graph, x), test::dataIn(graph, r)).has_value());
    }
} // namespace

int main(int argc, char** argv)
{
    meta::meta_module_init();
    {
        FlowGraph graph;
        const std::array types{&meta::ref_type_of_v<std::int8_t>, &meta::ref_type_of_v<std::uint8_t>, &meta::ref_type_of_v<std::int16_t>, &meta::ref_type_of_v<std::uint16_t>, &meta::ref_type_of_v<std::int32_t>, &meta::ref_type_of_v<std::uint32_t>, &meta::ref_type_of_v<std::int64_t>, &meta::ref_type_of_v<std::uint64_t>, &meta::ref_type_of_v<float>, &meta::ref_type_of_v<double>};
        const std::array operations{
            std::string_view{"lux.flow.add"},
            std::string_view{"lux.flow.subtract"},
            std::string_view{"lux.flow.multiply"},
            std::string_view{"lux.flow.divide"},
            std::string_view{"lux.flow.modulo"},
            std::string_view{"lux.flow.equal"},
            std::string_view{"lux.flow.not_equal"},
            std::string_view{"lux.flow.less"},
            std::string_view{"lux.flow.less_equal"},
            std::string_view{"lux.flow.greater"},
            std::string_view{"lux.flow.greater_equal"}
        };
        std::size_t count{};
        for (const auto* type : types)
        {
            for (const auto operation : operations)
            {
                addExpression(graph, operation, *type, count, true);
            }
            addExpression(graph, "lux.flow.negate", *type, count, false);
        }
        addExpression(graph, "lux.flow.and", meta::ref_type_of_v<bool>, count, true);
        addExpression(graph, "lux.flow.or", meta::ref_type_of_v<bool>, count, true);
        addExpression(graph, "lux.flow.not", meta::ref_type_of_v<bool>, count, false);
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
