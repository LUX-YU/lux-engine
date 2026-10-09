#include "FlowGraphFixture.hpp"
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/ObjectNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool value, std::source_location at = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "native compiler contract failed at %u\n", at.line());
            std::abort();
        }
    }

    void invoke(void*, void** input, void* output)
    {
        *static_cast<int*>(output) = *static_cast<int*>(input[0]) + 1;
    }

    void addCall(FlowGraph& graph, bool reflected, std::uint64_t export_id)
    {
        std::shared_ptr<const NativeCallDefinition> definition;
        {
            std::string name = reflected ? "reflected_compute" : "direct_compute";
            std::string signature{"int(int)"};
            std::string parameter{"argument"};
            meta::RefInvokable info;
            info.name = name;
            info.full_name = name;
            info.type_signature = signature;
            info.return_type = meta::ref_type_of_v<int>;
            info.parameters.push_back({parameter, meta::ref_type_of_v<int>, "int", meta::ref_type_of_v<int>.hash, false}
            );
            info.invoker = reflected ? &invoke : nullptr;
            auto made = NativeCallDefinition::create(info, object::CodeLease::builtin());
            require(made.has_value());
            definition = std::move(*made);
        }
        test::GraphFixture fixture;
        const auto* type = &meta::ref_type_of_v<int>;
        auto initial = meta::RuntimeObject::defaultOf(*type);
        require(initial.has_value());
        const auto variable = graph.addVariable("result_" + std::to_string(export_id), type, std::move(*initial));
        require(variable != 0U);
        const auto c = fixture.add<NativeCallPayload>(graph, "lux.flow.native_call", {std::move(definition)});
        const auto e =
            fixture.add<EventEntryPayload>(graph, "lux.flow.event", {}, "native_" + std::to_string(export_id));
        const auto w = fixture.add<SetVariablePayload>(graph, "lux.flow.set_variable", {variable, type});
        require(graph.addExport({{export_id}, e, export_id, {}}));
        require(graph.connect(test::execOut(graph, e), test::execIn(graph, c)).has_value());
        require(graph.connect(test::execOut(graph, c), test::execIn(graph, w)).has_value());
        require(graph.connect(test::dataOut(graph, c), test::dataIn(graph, w)).has_value());
    }
} // namespace

int main()
{
    using namespace lux;
    using namespace lux::flowforge;
    meta::meta_module_init();
    {
        FlowGraph graph;
        addCall(graph, false, 1);
        addCall(graph, true, 2);
        auto compiled = compileFlowForgeObject(graph, {.module_name = "native_owned_signature"});
        if (!compiled)
        {
            std::fprintf(stderr, "%s\n", compiled.error().message.c_str());
        }
        require(compiled.has_value() && !compiled->object.empty());
        std::printf(
            "PASS actual native AOT: direct/reflected calls after source destruction, %zu bytes\n",
            compiled->object.size()
        );
    }
    meta::meta_module_deinit();
}
