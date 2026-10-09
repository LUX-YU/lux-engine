#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>

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
    NativeFuncCall::Definition definition;
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
        using Entry = void(NativeFuncCall::Definition&, object::CodeLease) noexcept;
        auto entry = library->get_symbol<Entry>("createNativeDefinition");
        require(entry != nullptr);
        entry(definition, object::CodeLease::plugin(library));
    }
    require(!unloaded && !observed.expired());
    {
        NativeFuncCall node(std::move(definition));
        require(node.info().name == "plugin_compute");
        require(node.info().parameters[0].name == "argument");
        int initial{};
        std::memcpy(&initial, node.dataInPins()[0]->constantData().data(), sizeof(initial));
        require(initial == 0);
        node.reconstruct();
        int argument = 25;
        int result{};
        void* arguments[]{&argument};
        node.info().invoker(nullptr, arguments, &result);
        require(result == 42);
        require(!unloaded);
    }
    require(unloaded && observed.expired());
    meta::meta_module_deinit();
    std::puts("PASS real native DLL: source gone, host node pins callback and definition/control-block cleanup return");
}
