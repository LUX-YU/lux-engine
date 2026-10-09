#include "FlowGraphFixture.hpp"

#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/StateLayout.hpp>
#include <lux/engine/function/script/native/NativeModule.hpp>

#include <cstring>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;
    using test::require;

    std::uint64_t variable(FlowGraph& graph, std::string name, int initial)
    {
        const auto id = graph.addVariable(std::move(name), &meta::ref_type_of_v<int>, meta::RuntimeObject{initial});
        require(id != 0);
        return id;
    }

    void repeatedRead(const std::filesystem::path& linker)
    {
        FlowGraph graph;
        test::GraphFixture fixture;
        const auto source = variable(graph, "source", 1);
        const auto before = variable(graph, "before", 0);
        const auto after = variable(graph, "after", 0);
        const auto* integer = &meta::ref_type_of_v<int>;
        const auto entry = fixture.add<EventEntryPayload>(graph, "lux.flow.event", {}, "ReadWriteRead");
        const auto read = fixture.add<GetVariablePayload>(graph, "lux.flow.get_variable", {source, integer});
        const auto first = fixture.add<SetVariablePayload>(graph, "lux.flow.set_variable", {before, integer});
        const auto write = fixture.add<SetVariablePayload>(graph, "lux.flow.set_variable", {source, integer});
        const auto second = fixture.add<SetVariablePayload>(graph, "lux.flow.set_variable", {after, integer});
        const auto end = fixture.add<ReturnPayload>(graph, "lux.flow.return");
        require(graph.addExport({{1}, entry, 41, {}}));
        test::link(graph, test::execOut(graph, entry), test::execIn(graph, first));
        test::link(graph, test::execOut(graph, first), test::execIn(graph, write));
        test::link(graph, test::execOut(graph, write), test::execIn(graph, second));
        test::link(graph, test::execOut(graph, second), test::execIn(graph, end));
        test::link(graph, test::dataOut(graph, read), test::dataIn(graph, first));
        test::link(graph, test::dataOut(graph, read), test::dataIn(graph, second));
        require(graph.pin(test::dataIn(graph, write))->setDefault(meta::RuntimeObject{7}));

        auto compiled = compileFlowForgeObject(graph, {.module_name = "memory_read_contract"});
        if (!compiled)
        {
            std::fprintf(stderr, "%s\n", compiled.error().message.c_str());
        }
        require(compiled.has_value());
        auto artifact = linkFlowForgeObject(*compiled, linker);
        require(artifact.has_value());
        auto module = script::loadNativeModule(artifact->payload(), "memory_read_contract");
        require(module.has_value());
        const auto* function = module->findFunction(script::ScriptSymbolId{41});
        require(function != nullptr && function->arg_count == 0 && function->invoke != nullptr);
        std::string error;
        const auto layout = computeStateLayout(graph, &error);
        require(error.empty() && layout.size == module->stateSize());
        require(layout.align <= alignof(std::max_align_t));
        auto state = layout.defaults;
        lux_script_native_instance_context context{state.data(), nullptr, 0, 0};
        lux_script_call_frame frame{};
        require(function->invoke(&context, &frame) == 0);
        const auto load = [&](std::uint64_t id)
        {
            const auto* field = layout.find(id);
            require(field != nullptr);
            int value{};
            std::memcpy(&value, state.data() + field->offset, sizeof(value));
            return value;
        };
        require(load(source) == 7);
        require(load(before) == 1);
        require(load(after) == 7);
        require(function->invoke(&context, &frame) == 0);
        require(load(before) == 7 && load(after) == 7);
        std::puts("PASS: one registered read observes 1 then 7 across a write; second native invocation observes 7");
    }
} // namespace

int main(int argc, char** argv)
{
    meta::meta_module_init();
    repeatedRead(argc == 2 ? std::filesystem::path{argv[1]} : std::filesystem::path{});
    meta::meta_module_deinit();
}
