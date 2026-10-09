#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/material/MaterialNodeCatalog.hpp>

#include "node_plugin_state.hpp"

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
    using namespace lux;
    using namespace lux::material;
    using Library = engine::platform::DynamicLibrary;
    NodePluginState state;
    std::shared_ptr<const MaterialNodeType> type;
    MaterialNodePayload payload;
    std::weak_ptr<Library> observed_library;
    {
        auto library = std::shared_ptr<Library>(
            new Library(std::filesystem::path{argv[1]}),
            [&](Library* value) noexcept
            {
                require(state.destroyed == state.constructed);
                delete value;
                state.unloaded = true;
            }
        );
        require(library->is_loaded());
        observed_library = library;
        using Entry = void(MaterialNodeRegistration&, const object::CodeLease&, NodePluginState&) noexcept;
        const auto entry = library->get_symbol<Entry>("materialNodeRegistration");
        require(entry != nullptr);
        MaterialNodeRegistration registration;
        entry(registration, object::CodeLease::plugin(library), state);
        MaterialNodeCatalog catalog;
        require(catalog.add({&registration, 1}).has_value());
        type = catalog.find(registration.identity.id);
        require(bool(type));
        auto result = type->create();
        require(result.has_value());
        payload = std::move(*result);
    }
    require(!observed_library.expired() && !state.unloaded);
    require(type->describePins(payload)->size() == 1);
    shadergen::ShaderIR ir;
    auto outputs = type->compile(payload, {}, ir);
    require(outputs.has_value() && outputs->size() == 1);
    require(ir.values[outputs->front()].constant[0] == 0.625F);
    type.reset();
    require(!observed_library.expired());
    state.reject_clone = true;
    auto rejection = payload.clone();
    require(!rejection && rejection.error().message == "DLL clone rejected");
    require(!observed_library.expired());
    state.reject_clone = false;
    auto copy = payload.clone();
    require(copy.has_value() && state.clone_returns == 2);
    payload = MaterialNodePayload{};
    require(state.destroyed == 1 && !state.unloaded);
    *copy = MaterialNodePayload{};
    require(state.destroyed == 2 && state.unloaded && observed_library.expired());
    std::puts(
        "PASS: actual Material DLL code retained through catalog release, compilation, clone and final payload cleanup"
    );
}
