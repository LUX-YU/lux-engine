#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
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

    template <class T, class... Args> T& add(FlowGraph& graph, Args&&... args)
    {
        auto owner = std::make_unique<T>(std::forward<Args>(args)...);
        auto& node = *owner;
        require(graph.addNode(std::move(owner)).valid());
        return node;
    }

    void link(FlowGraph& graph, const Pin& from, const Pin& to)
    {
        require(
            graph.connect(*graph.findPin(graph.pinId(&from)), *graph.findPin(graph.pinId(&to))) == ELinkError::SUCCESS
        );
    }

    const ExecInPin& branchTree(
        FlowGraph& graph,
        const OnEventNode& entry,
        std::uint64_t variable,
        unsigned depth,
        unsigned levels,
        unsigned value,
        std::vector<const ExecOutPin*>& leaves
    )
    {
        if (depth == levels)
        {
            auto& write = add<SetVariableNode>(graph, variable, DataPinInfo{"result", &meta::ref_type_of_v<int>});
            auto constant = meta::RuntimeObject::defaultOf(meta::ref_type_of_v<int>);
            require(constant.has_value());
            *static_cast<int*>(constant->data()) = static_cast<int>(value + 1);
            auto* input = static_cast<DataInPin*>(graph.findPin(graph.pinId(&write.valueIn())));
            require(input->setConstantData(std::move(*constant)));
            leaves.push_back(&write.execOutPin());
            return write.execInPin();
        }
        auto& branch = add<BranchNode>(graph);
        link(graph, *entry.paramPins()[depth], branch.dataInPin());
        link(
            graph,
            branch.execOutPinUp(),
            branchTree(graph, entry, variable, depth + 1, levels, value * 2 + 1, leaves)
        );
        link(graph, branch.execOutPinDown(), branchTree(graph, entry, variable, depth + 1, levels, value * 2, leaves));
        return branch.execInPin();
    }

    FlowGraph example(unsigned levels, bool reverse)
    {
        FlowGraph graph;
        auto initial = meta::RuntimeObject::defaultOf(meta::ref_type_of_v<int>);
        require(initial.has_value());
        const auto variable = graph.addVariable("result", &meta::ref_type_of_v<int>, std::move(*initial));
        require(variable != 0);
        const auto* boolean = &meta::ref_type_of_v<bool>;
        auto& entry = add<OnEventNode>(
            graph,
            "Choose",
            std::vector<FuncArgInfo>{{boolean, "outer"}, {boolean, "middle"}, {boolean, "inner"}}
        );
        require(graph.addExport({{1}, graph.nodeId(&entry), 41, {}}));
        std::vector<const ExecOutPin*> leaves;
        link(graph, entry.execOutPin(), branchTree(graph, entry, variable, 0, levels, 0, leaves));
        auto& merge = add<ReturnNode>(graph);
        for (std::size_t i{}; i != leaves.size(); ++i)
        {
            const auto index = reverse ? leaves.size() - i - 1 : i;
            link(graph, *leaves[index], merge.execInPin());
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
