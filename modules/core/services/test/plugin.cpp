#include "ServiceProbe.hpp"
#include <cassert>
#include <iostream>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>

int main(int argc, char** argv)
{
    assert(argc == 2);
    using namespace lux;
    using namespace lux::services;
    using Library = engine::platform::DynamicLibrary;
    using Get = void (*)(object::CodeLease, std::shared_ptr<const ServiceEntry>&);
    fixture::Trace trace;
    auto library = std::shared_ptr<Library>(
        new Library(argv[1]),
        [&](Library* value) noexcept
        {
            assert(trace.destroyed == 1 && trace.returned == 1);
            delete value;
            ++trace.unloaded;
        }
    );
    assert(library->is_loaded());
    const auto factory = reinterpret_cast<Get>(library->get_symbol("serviceDefinition"));
    assert(factory);
    auto& messages = object::ObjectRuntime::instance();
    std::weak_ptr<fixture::Value> weak;
    {
        ServiceRegistry registry{};
        auto scope = registry.createScope();
        assert(scope && scope->provide(ServiceNameView{"fixture.trace"}, trace));
        std::shared_ptr<const ServiceEntry> entry;
        factory(object::CodeLease::plugin(library), entry);
        assert(registry.publish({entry}) && trace.created == 0);
        auto first = registry.get<fixture::Value>(*scope);
        auto second = registry.get<fixture::Value>(*scope);
        assert(first && second && *first == *second && trace.created == 1);
        assert((*first)->read() == 73);
        weak = *first;
        library.reset();
        entry.reset();
        assert(registry.publish({}));
        first->reset();
        assert(scope->release() && !scope->drained());
        std::jthread worker([last = std::move(*second)]() mutable { last.reset(); });
        worker.join();
        assert(weak.expired() && trace.destroyed == 0 && trace.unloaded == 0);
        assert(messages.collectRetired() == 1);
        assert(scope->drained() && registry.drained());
        assert(trace.destroyed == 1 && trace.returned == 1);
        assert(registry.publish({})); // Owner publication reclaims only physically retired metadata.
        assert(trace.unloaded == 1);
    }
    assert(trace.unloaded == 1);
    weak.reset(); // Host control block must be safe after actual DLL unload.
    assert(messages.pendingRetirements() == 0);
    std::cout << "PASS service DLL: static descriptor, one allocation, worker last release, destructor tail, unload, "
                 "weak cleanup\n";
}
