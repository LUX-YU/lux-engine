#include "FlowGraphFixture.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/ObjectNodes.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
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

    using test::dataIn;
    using test::dataOut;
    using test::execIn;
    using test::execOut;
    using test::link;

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
        test::GraphFixture fixture;
        const auto& boolean = meta::ref_type_of_v<bool>;
        const auto entry = fixture.add<EventEntryPayload>(
            graph,
            "lux.flow.event",
            {std::vector<FuncArgInfo>{{&boolean, "condition"}}},
            "Tick"
        );
        require(graph.addExport({{1}, entry, 41, {}}));
        const auto result = fixture.add<ReturnPayload>(graph, "lux.flow.return");
        if (mode == 0 || mode == 1 || mode == 2)
        {
            const auto branch = fixture.add<BranchPayload>(graph, "lux.flow.branch");
            link(graph, execOut(graph, entry), execIn(graph, branch));
            link(graph, dataOut(graph, entry), dataIn(graph, branch));
            if (mode == 1)
            {
                const auto other = fixture.add<ReturnPayload>(graph, "lux.flow.return");
                link(graph, execOut(graph, branch), execIn(graph, result));
                link(graph, execOut(graph, branch, 1), execIn(graph, other));
            }
            else
            {
                const auto left = fixture.add<SequencePayload>(graph, "lux.flow.sequence");
                const auto right = fixture.add<SequencePayload>(graph, "lux.flow.sequence");
                link(graph, execOut(graph, branch), execIn(graph, left));
                link(graph, execOut(graph, branch, 1), execIn(graph, right));
                link(graph, execOut(graph, right), execIn(graph, result));
                if (mode == 0)
                {
                    link(graph, execOut(graph, left), execIn(graph, result));
                }
                else
                {
                    const auto nested = fixture.add<BranchPayload>(graph, "lux.flow.branch");
                    link(graph, execOut(graph, left), execIn(graph, nested));
                    link(graph, dataOut(graph, entry), dataIn(graph, nested));
                    link(graph, execOut(graph, nested), execIn(graph, result));
                    link(graph, execOut(graph, nested, 1), execIn(graph, result));
                }
            }
        }
        else if (mode == 3)
        {
            const auto loop = fixture.add<ForLoopPayload>(graph, "lux.flow.for_loop");
            const auto branch = fixture.add<BranchPayload>(graph, "lux.flow.branch");
            const auto stop = fixture.add<BreakPayload>(graph, "lux.flow.break");
            const auto body = fixture.add<SequencePayload>(graph, "lux.flow.sequence");
            link(graph, execOut(graph, entry), execIn(graph, loop));
            link(graph, execOut(graph, loop), execIn(graph, branch));
            link(graph, dataOut(graph, entry), dataIn(graph, branch));
            link(graph, execOut(graph, branch), execIn(graph, stop));
            link(graph, execOut(graph, branch, 1), execIn(graph, body));
            link(graph, execOut(graph, loop, 1), execIn(graph, result));
        }
        else if (mode == 4)
        {
            const auto loop = fixture.add<WhileLoopPayload>(graph, "lux.flow.while_loop");
            const auto stop = fixture.add<BreakPayload>(graph, "lux.flow.break");
            link(graph, execOut(graph, entry), execIn(graph, loop));
            link(graph, dataOut(graph, entry), dataIn(graph, loop));
            link(graph, execOut(graph, loop), execIn(graph, stop));
            link(graph, execOut(graph, loop, 1), execIn(graph, result));
        }
        else if (mode == 5)
        {
            const auto sequence = fixture.add<SequencePayload>(graph, "lux.flow.sequence", {1});
            const auto second = execOut(graph, sequence, 1);
            require(second.valid());
            const auto branch = fixture.add<BranchPayload>(graph, "lux.flow.branch");
            const auto left = fixture.add<SequencePayload>(graph, "lux.flow.sequence");
            const auto right = fixture.add<SequencePayload>(graph, "lux.flow.sequence");
            link(graph, execOut(graph, entry), execIn(graph, sequence));
            link(graph, execOut(graph, sequence), execIn(graph, branch));
            link(graph, dataOut(graph, entry), dataIn(graph, branch));
            link(graph, execOut(graph, branch), execIn(graph, left));
            link(graph, execOut(graph, branch, 1), execIn(graph, right));
            link(graph, second, execIn(graph, result));
        }
        else
        {
            const auto stop = fixture.add<BreakPayload>(graph, "lux.flow.break");
            link(graph, execOut(graph, entry), execIn(graph, stop));
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
