#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool value, std::source_location where = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "Flow control contract failed at %u\n", where.line());
            std::abort();
        }
    }

    template <class T, class... Args> T& add(FlowGraph& graph, Args&&... args)
    {
        auto owner = std::make_unique<T>(std::forward<Args>(args)...);
        auto& value = *owner;
        require(graph.addNode(std::move(owner)).valid());
        return value;
    }

    void link(FlowGraph& graph, const Pin& from, const Pin& to)
    {
        require(
            graph.connect(*graph.findPin(graph.pinId(&from)), *graph.findPin(graph.pinId(&to))) == ELinkError::SUCCESS
        );
    }

    void writeCount(std::ostream& output, std::size_t value)
    {
        for (unsigned i = 0; i != 8; ++i)
        {
            output.put(static_cast<char>(static_cast<std::uint64_t>(value) >> (8U * i)));
        }
    }

    FlowGraph example(unsigned mode)
    {
        FlowGraph graph;
        const auto& boolean = meta::ref_type_of_v<bool>;
        auto& entry = add<OnEventNode>(graph, "Tick", std::vector<FuncArgInfo>{{&boolean, "condition"}});
        require(graph.addExport({{1}, graph.nodeId(&entry), 41, {}}));
        auto& result = add<ReturnNode>(graph);
        if (mode == 0 || mode == 1 || mode == 2)
        {
            auto& branch = add<BranchNode>(graph);
            link(graph, entry.execOutPin(), branch.execInPin());
            link(graph, *entry.paramPins().front(), branch.dataInPin());
            if (mode == 1)
            {
                auto& other = add<ReturnNode>(graph);
                link(graph, branch.execOutPinUp(), result.execInPin());
                link(graph, branch.execOutPinDown(), other.execInPin());
            }
            else
            {
                auto& left = add<SequenceNode>(graph);
                auto& right = add<SequenceNode>(graph);
                link(graph, branch.execOutPinUp(), left.execInPin());
                link(graph, branch.execOutPinDown(), right.execInPin());
                link(graph, right.execOutPin(), result.execInPin());
                if (mode == 0)
                {
                    link(graph, left.execOutPin(), result.execInPin());
                }
                else
                {
                    auto& nested = add<BranchNode>(graph);
                    link(graph, left.execOutPin(), nested.execInPin());
                    link(graph, *entry.paramPins().front(), nested.dataInPin());
                    link(graph, nested.execOutPinUp(), result.execInPin());
                    link(graph, nested.execOutPinDown(), result.execInPin());
                }
            }
        }
        else if (mode == 3)
        {
            auto& loop = add<ForLoopNode>(graph);
            auto& branch = add<BranchNode>(graph);
            auto& stop = add<BreakNode>(graph);
            auto& body = add<SequenceNode>(graph);
            link(graph, entry.execOutPin(), loop.execInPin());
            link(graph, loop.loopBody(), branch.execInPin());
            link(graph, *entry.paramPins().front(), branch.dataInPin());
            link(graph, branch.execOutPinUp(), stop.execInPin());
            link(graph, branch.execOutPinDown(), body.execInPin());
            link(graph, loop.completed(), result.execInPin());
        }
        else if (mode == 4)
        {
            auto& loop = add<WhileLoopNode>(graph);
            auto& stop = add<BreakNode>(graph);
            link(graph, entry.execOutPin(), loop.execInPin());
            link(graph, *entry.paramPins().front(), loop.dataInPin());
            link(graph, loop.loopBody(), stop.execInPin());
            link(graph, loop.completed(), result.execInPin());
        }
        else if (mode == 5)
        {
            auto& sequence = add<SequenceNode>(graph);
            auto* second = sequence.addExecOutPin();
            require(second != nullptr);
            auto& branch = add<BranchNode>(graph);
            auto& left = add<SequenceNode>(graph);
            auto& right = add<SequenceNode>(graph);
            link(graph, entry.execOutPin(), sequence.execInPin());
            link(graph, sequence.execOutPin(), branch.execInPin());
            link(graph, *entry.paramPins().front(), branch.dataInPin());
            link(graph, branch.execOutPinUp(), left.execInPin());
            link(graph, branch.execOutPinDown(), right.execInPin());
            link(graph, *second, result.execInPin());
        }
        else
        {
            auto& stop = add<BreakNode>(graph);
            link(graph, entry.execOutPin(), stop.execInPin());
        }
        return graph;
    }
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::ofstream output;
    if (argc == 2)
    {
        output.open(argv[1], std::ios::binary | std::ios::trunc);
        require(output.good());
    }
    std::size_t bytes{};
    for (unsigned mode = 0; mode != 7; ++mode)
    {
        auto graph = example(mode);
        auto compiled = compileFlowForgeObject(graph, {.module_name = "control_contract"});
        if (!compiled)
        {
            require(compiled.error().code == EFlowForgeError::AOT_CODEGEN_FAILED);
            require(compiled.error().node_id == 0 && compiled.error().pin_id == 0);
            std::printf(
                "mode%u FAIL %u %llu %llu %s\n",
                mode,
                static_cast<unsigned>(compiled.error().code),
                static_cast<unsigned long long>(compiled.error().node_id),
                static_cast<unsigned long long>(compiled.error().pin_id),
                compiled.error().message.c_str()
            );
            const bool is_explicit_invalid_break =
                mode == 6 && compiled.error().message == "compile failed: Break is only valid inside a loop body";
            require(is_explicit_invalid_break);
            continue;
        }
        require(mode != 6 && !compiled->object.empty());
        std::printf("mode%u object%zu\n", mode, compiled->object.size());
        bytes += compiled->object.size();
        if (output.is_open())
        {
            writeCount(output, compiled->object.size());
            output.write(reinterpret_cast<const char*>(compiled->object.data()), compiled->object.size());
        }
    }
    std::printf("control AOT total bytes=%zu unexpected_failure=0\n", bytes);
    // Every legal case must compile; the archived pre-correction probe retains the former failure.
    return 0;
}
