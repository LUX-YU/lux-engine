#include "FlowGraphFixture.hpp"
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/ObjectNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/function/script/native/NativeModule.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <source_location>
#include <utility>
#include <vector>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool value, std::source_location where = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "nested control contract failed at %u\n", where.line());
            std::abort();
        }
    }

    using test::dataIn;
    using test::dataOut;
    using test::execIn;
    using test::execOut;
    using test::link;

    PinId branchTree(
        FlowGraph& graph,
        test::GraphFixture& fixture,
        NodeId entry,
        std::uint64_t variable,
        unsigned depth,
        unsigned levels,
        unsigned value,
        std::vector<PinId>& leaves
    )
    {
        if (depth == levels)
        {
            const auto write =
                fixture.add<SetVariablePayload>(graph, "lux.flow.set_variable", {variable, &meta::ref_type_of_v<int>});
            auto constant = meta::RuntimeObject::defaultOf(meta::ref_type_of_v<int>);
            require(constant.has_value());
            *static_cast<int*>(constant->data()) = static_cast<int>(value + 1);
            auto* input = graph.pin(dataIn(graph, write));
            require(input->setDefault(std::move(*constant)));
            leaves.push_back(execOut(graph, write));
            return execIn(graph, write);
        }
        const auto branch = fixture.add<BranchPayload>(graph, "lux.flow.branch");
        link(graph, dataOut(graph, entry, depth), dataIn(graph, branch));
        link(
            graph,
            execOut(graph, branch),
            branchTree(graph, fixture, entry, variable, depth + 1, levels, value * 2 + 1, leaves)
        );
        link(
            graph,
            execOut(graph, branch, 1),
            branchTree(graph, fixture, entry, variable, depth + 1, levels, value * 2, leaves)
        );
        return execIn(graph, branch);
    }

    FlowGraph example(unsigned levels, bool reverse)
    {
        FlowGraph graph;
        test::GraphFixture fixture;
        auto initial = meta::RuntimeObject::defaultOf(meta::ref_type_of_v<int>);
        require(initial.has_value());
        const auto variable = graph.addVariable("result", &meta::ref_type_of_v<int>, std::move(*initial));
        require(variable != 0);
        const auto* boolean = &meta::ref_type_of_v<bool>;
        const auto entry = fixture.add<EventEntryPayload>(
            graph,
            "lux.flow.event",
            {std::vector<FuncArgInfo>{{boolean, "outer"}, {boolean, "middle"}, {boolean, "inner"}}},
            "Choose"
        );
        require(graph.addExport({{1}, entry, 41, {}}));
        std::vector<PinId> leaves;
        link(graph, execOut(graph, entry), branchTree(graph, fixture, entry, variable, 0, levels, 0, leaves));
        const auto merge = fixture.add<ReturnPayload>(graph, "lux.flow.return");
        for (std::size_t i{}; i != leaves.size(); ++i)
        {
            const auto index = reverse ? leaves.size() - i - 1 : i;
            link(graph, leaves[index], execIn(graph, merge));
        }
        return graph;
    }

    bool run(unsigned levels, bool reverse, const std::filesystem::path& linker)
    {
        auto graph = example(levels, reverse);
        auto object = compileFlowForgeObject(graph, {.module_name = "nested_control"});
        if (!object)
        {
            std::printf(
                "depth%u reverse%u COMPILE_FAIL %s\n",
                levels,
                unsigned(reverse),
                object.error().message.c_str()
            );
            return false;
        }
        auto artifact = linkFlowForgeObject(*object, linker);
        if (!artifact)
        {
            std::printf("LINK_FAIL %s\n", artifact.error().message.c_str());
            return false;
        }
        auto module = script::loadNativeModule(artifact->payload(), "nested_control");
        require(module.has_value());
        const auto* function = module->findFunction(script::ScriptSymbolId{41});
        require(function != nullptr && function->arg_count == 3 && function->invoke != nullptr);
        require(module->stateSize() == sizeof(int) && module->stateAlignment() <= alignof(int));
        int state{};
        lux_script_native_instance_context context{&state, nullptr, 0, 0};
        for (unsigned mask{}; mask != 8; ++mask)
        {
            std::array<bool, 3> values{(mask & 4U) != 0, (mask & 2U) != 0, (mask & 1U) != 0};
            std::array<lux_script_value_slot, 3> arguments{};
            unsigned expected{};
            for (std::size_t i{}; i != arguments.size(); ++i)
            {
                const auto& type = function->args[i];
                require(type.kind == LUX_SCRIPT_VK_BOOL && type.size == sizeof(bool));
                arguments[i] = {type.kind, {}, type.size, type.type_id, &values[i]};
                if (i < levels)
                {
                    expected = expected * 2 + unsigned(values[i]);
                }
            }
            state = -1;
            lux_script_call_frame frame{arguments.data(), 3, 0, nullptr, 0, 0, nullptr};
            require(function->invoke(&context, &frame) == 0);
            require(state == static_cast<int>(expected + 1));
        }
        std::printf("depth%u reverse%u PASS 8 actual native invocations\n", levels, unsigned(reverse));
        return true;
    }
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    meta::meta_module_init();
    bool passed = true;
    for (unsigned levels = 1; levels != 4; ++levels)
    {
        for (bool reverse : {false, true})
        {
            passed =
                run(levels, reverse, argc == 2 ? std::filesystem::path{argv[1]} : std::filesystem::path{}) && passed;
        }
    }
    meta::meta_module_deinit();
    return passed ? 0 : 42;
}
