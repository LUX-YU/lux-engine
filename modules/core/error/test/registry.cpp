#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/error/detail/DefinitionTable.hpp>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <cassert>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    using namespace lux::error;
    static_assert(std::is_trivially_copyable_v<Error> && sizeof(Error) == 32);
    auto& registry = ErrorRegistry::instance();
    const ErrorDescriptor descriptor{
        "fixture.numbers", "{{value}} {0} {1} {2}", ERecovery::PERMANENT,
        {EArgument::UNSIGNED, EArgument::SIGNED, EArgument::HEX}
    };
    const auto first = registry.registerType(descriptor);
    assert(first && *first == errorId(descriptor.name));
    assert(registry.registerType(descriptor) == first);
    const auto* stable = registry.find(*first);
    assert(stable && stable->name == descriptor.name);
    auto conflict = descriptor;
    conflict.message = "different {0} {1} {2}";
    auto rejected = registry.registerType(conflict);
    assert(!rejected && rejected.error() == ERegistrationError::DEFINITION_MISMATCH);
    assert(registry.find(*first) == stable && stable->message == descriptor.message);
    assert(format(Error{*first, {42, static_cast<std::uint64_t>(-3), 255}}) == "{value} 42 -3 0xff");
    assert(format({}) == "No error");
    assert(format({1, {2, 3, 4}}) == "Unknown error 0x1 0x2 0x3 0x4");

    for (const auto message : {"{", "}", "{3}", "{00}", "{x}", "{0}", "unmatched{{}"})
    {
        const auto invalid = registry.registerType({"fixture.bad", message});
        assert(!invalid && invalid.error() == ERegistrationError::INVALID_DESCRIPTOR);
    }
    assert(!registry.registerType({"", "empty"}));
    assert(!registry.registerType({"bad name", "invalid"}));
    assert(!registry.registerType({"fixture.unused", "unused", ERecovery::BUG, {EArgument::UNSIGNED}}));
    assert(!registry.registerType({"fixture.argument", "{0}", ERecovery::BUG, {static_cast<EArgument>(99)}}));
    assert(!registry.registerType({"fixture.recovery", "invalid", static_cast<ERecovery>(99)}));
    const auto registration_failure = makeError(conflict);
    assert(registration_failure.type == errorId("lux.error.registration"));
    assert(registration_failure.args[0] == *first);
    assert(registration_failure.args[1] == static_cast<std::uint64_t>(ERegistrationError::DEFINITION_MISMATCH));

    // Exercise the production insertion branch with a deliberately occupied hash bucket.
    // No alternate public hash/registration entry point is exposed for this fixture.
    detail::DefinitionTable collisions;
    assert(collisions.insert(*first, descriptor));
    auto other_name = descriptor;
    other_name.name = "fixture.other";
    const auto collision = collisions.insert(*first, other_name);
    assert(!collision && collision.error() == ERegistrationError::HASH_COLLISION);
    assert(collisions.find(*first)->name == descriptor.name);

    std::vector<std::jthread> workers;
    for (int thread{}; thread < 8; ++thread)
        workers.emplace_back([&] {
            for (int i{}; i < 1024; ++i)
            {
                const std::string name = "fixture.concurrent." + std::to_string(i);
                const auto id = registry.registerType({name, "shared"});
                assert(id && registry.find(*id)->name == name);
                assert(registry.find(*first) == stable && stable->message == descriptor.message);
            }
        });
    workers.clear();

    assert(argc == 2);
    Error plugin_error;
    const ErrorDefinition* plugin_definition{};
    {
        lux::engine::platform::DynamicLibrary library(argv[1]);
        assert(library.is_loaded());
        const auto address = reinterpret_cast<ErrorRegistry* (*)() noexcept>(library.get_symbol("error_registry"));
        const auto failure = reinterpret_cast<Error (*)() noexcept>(library.get_symbol("plugin_failure"));
        assert(address && failure && address() == &registry);
        plugin_error = failure();
        plugin_definition = registry.find(plugin_error.type);
        assert(plugin_definition && plugin_definition->recovery == ERecovery::NEEDS_INPUT);
    }
    assert(registry.find(plugin_error.type) == plugin_definition);
    assert(format(plugin_error) == "Plugin result -73");
}
