#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>

#include <cstdio>
#include <cstdlib>
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
    using namespace lux::flowforge;
    ScriptAbilityNodeCatalog catalog;
    std::unique_ptr<ScriptAbilityNode> node;
    {
        lux::engine::platform::DynamicLibrary library{std::filesystem::path{argv[1]}};
        require(library.is_loaded());
        using Entry = const ScriptAbilityNodeDescription*() noexcept;
        const auto entry = library.get_symbol<Entry>("abilityDescription");
        require(entry != nullptr);
        const auto* description = entry();
        require(catalog.add({{description, 1}}).has_value());
        node = std::make_unique<ScriptAbilityNode>(*description);
    }
    const auto& description = catalog.view().nodes().front();
    require(description.contract.name() == "external.dll");
    require(description.contract_display_name == "DLL ability");
    require(description.parameters[0].name == "argument");
    require(description.parameters[0].value.canonical_name == "lux.i32");
    require(node->contract().name() == "external.dll");
    require(node->parameters()[0].name == "argument");
    require(node->parameters()[0].value.canonical_name == "lux.i32");
    require(node->expectedSchemaHash() == 819);
    node = std::make_unique<ScriptAbilityNode>(description);
    require(node->parameters()[0].name == "argument");
    std::puts("PASS: actual DLL unloaded; copied Flow catalog/node metadata remains usable");
}
