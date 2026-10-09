#include "NativeGraphFixture.hpp"
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace
{
    void require(bool condition) noexcept
    {
        if (!condition)
        {
            std::abort();
        }
    }
} // namespace

int main(int argc, char** argv)
{
    require(argc == 2);
    using namespace lux;
    using namespace lux::flowforge;
    using Library = engine::platform::DynamicLibrary;
    meta::meta_module_init();
    bool unloaded{};
    std::weak_ptr<Library> observed;
    native_fixture::Definition definition;
    {
        auto library = std::shared_ptr<Library>(
            new Library(std::filesystem::path{argv[1]}),
            [&](Library* value) noexcept
            {
                delete value;
                unloaded = true;
            }
        );
        require(library->is_loaded());
        observed = library;
        using Entry = void(native_fixture::Definition&, object::CodeLease) noexcept;
        auto entry = library->get_symbol<Entry>("createNativeDefinition");
        require(entry != nullptr);
        entry(definition, object::CodeLease::plugin(library));
    }
    require(!unloaded && !observed.expired());
    {
        FlowGraph graph;
        auto owner = native_fixture::make(std::move(definition));
        const auto added = graph.addNode(std::move(owner));
        require(added.has_value());
        const auto id = *added;
        const auto* node = graph.node(id);
        const auto& info = native_fixture::native(*node).definition->signature();
        require(info.name == "plugin_compute");
        require(info.parameters[0].name == "argument");
        const auto input = native_fixture::pin(graph, id, EFlowPinRole::DATA, graph::EPinDirection::INPUT);
        int initial{};
        std::memcpy(&initial, graph.pin(input)->default_value.data(), sizeof(initial));
        require(initial == 0);
        {
            auto replacement = native_fixture::candidate(id, native_fixture::native(*node).definition);
            native_fixture::preserveExecutionPins(replacement, graph);
            const std::array erase{id};
            const std::array<FlowNodeEntry, 1> insert{{{id, &replacement.value, replacement.pins}}};
            auto prepared = FlowGraphEdit::prepare(graph, {.insert = insert, .erase = erase});
            require(prepared.has_value() && !unloaded && graph.node(id) == node);
            const auto* old_definition = native_fixture::native(*node).definition.get();
            prepared->commit();
            auto removed = prepared->takeRemoved();
            require(
                removed.size() == 1 && native_fixture::native(removed.front().value).definition.get() == old_definition
            );
            removed.clear();
            require(!unloaded && graph.node(id));
        }
        const auto& current = native_fixture::native(*graph.node(id)).definition->signature();
        int argument = 25;
        int result{};
        void* arguments[]{&argument};
        current.invoker(nullptr, arguments, &result);
        require(result == 42);
        require(!unloaded);
    }
    require(unloaded && observed.expired());
    meta::meta_module_deinit();
    std::puts("PASS real native DLL: source gone, host node pins callback and definition/control-block cleanup return");
}
