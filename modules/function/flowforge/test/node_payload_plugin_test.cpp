#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/flowforge/FlowNodePayload.hpp>

#include "node_payload_plugin_state.hpp"

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
    using namespace lux::flowforge;
    using Library = engine::platform::DynamicLibrary;
    FlowPayloadPluginState state;
    FlowNodePayload payload;
    std::weak_ptr<Library> observed;
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
        observed = library;
        using Entry = void(FlowNodePayload&, const object::CodeLease&, FlowPayloadPluginState&) noexcept;
        const auto entry = library->get_symbol<Entry>("createFlowPayload");
        require(entry != nullptr);
        entry(payload, object::CodeLease::plugin(library), state);
    }
    require(!state.unloaded && !observed.expired());
    state.reject_clone = true;
    auto failure = payload.clone();
    require(!failure && failure.error().message == "DLL clone rejected: external semantic data");
    require(failure.error().node_id == 11 && failure.error().pin_id == 19);
    require(state.constructed == 1 && state.destroyed == 0);
    state.reject_clone = false;
    auto copy = payload.clone();
    require(copy.has_value() && state.clone_calls == 2);
    payload = FlowNodePayload{};
    require(state.destroyed == 1 && !state.unloaded);
    *copy = FlowNodePayload{};
    require(state.destroyed == 2 && state.unloaded && observed.expired());
    require(failure.error().message == "DLL clone rejected: external semantic data");
    std::puts("PASS Flow DLL: last payload pins clone/destruction return; owning error survives unload");
}
