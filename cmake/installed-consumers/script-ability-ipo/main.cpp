#include "TinyAbility.hpp"
#include "TinyAbility.ability.generated.hpp"
#include "TinyAbility.ability.native.generated.hpp"

#include <cassert>
#include <array>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <string_view>

namespace
{
    struct Provider final
    {
        volatile std::int32_t bias{4};

        [[nodiscard]] std::int32_t read(std::int32_t input) noexcept
        {
            return bias + input;
        }
    };
}

int main(int argc, char** argv)
{
    using Ability = installed_consumer::TinyAbility;
    Provider provider;
    const auto binding = lux::script::bindScriptAbility<Ability>(provider);
    // Model a runtime publication without adding a volatile load to the timed call. Otherwise
    // whole-program IPO can turn the supposedly dynamic comparison into another static call.
    const void* volatile dispatch_publication = binding.dispatch;
    auto dynamic_binding = binding;
    dynamic_binding.dispatch = dispatch_publication;
    const auto dynamic = lux::script::ScriptAbilityCpp<Ability>::create(dynamic_binding);
    const auto specialized = lux::script::ScriptAbilityStatic<Ability, Provider>::create(provider, binding);
    const auto native = lux::script::native::makeScriptAbilityNativeContribution<Ability>();
    const auto bound_native = lux::script::native::makeScriptAbilityNativeContribution<Ability>(provider, binding);
    Provider foreign;
    assert(!lux::script::native::makeScriptAbilityNativeContribution<Ability>(foreign, binding));
    auto invalid = binding;
    invalid.dispatch = &foreign;
    assert(!lux::script::native::makeScriptAbilityNativeContribution<Ability>(provider, invalid));
    assert(bound_native && bound_native->expected_context == &provider);
    assert(dynamic && specialized && native.valid() && native.methods.size() == 1U);
    using NativeRead = std::int32_t (*)(void*, const void*, std::int32_t) noexcept;
    NativeRead volatile native_publication = reinterpret_cast<NativeRead>(native.methods.front().entry);
    NativeRead volatile bound_publication = reinterpret_cast<NativeRead>(bound_native->methods.front().entry);
    const auto native_read = native_publication;
    const auto bound_read = bound_publication;
    assert(bound_read(binding.context, binding.dispatch, 3) == 7);
    assert(provider.read(3) == 7);
    assert(dynamic->read(3) == 7);
    assert(specialized->read(3) == 7);
    assert(native_read(binding.context, binding.dispatch, 3) == 7);
    if (argc == 1) return 0;
    std::string_view selected;
    const char* path{};
    if (argc == 3 && std::string_view{argv[1]} == "--output") path = argv[2];
    else if (argc == 5 && std::string_view{argv[1]} == "--case" &&
        std::string_view{argv[3]} == "--output")
    {
        selected = argv[2];
        path = argv[4];
        constexpr std::array names{std::string_view{"direct"}, std::string_view{"typed-dynamic"},
            std::string_view{"typed-static-ipo"}, std::string_view{"native-provider-specialized"},
            std::string_view{"native-typed-entry"}};
        bool found{};
        for (const auto name : names) found |= name == selected;
        if (!found) return 1;
    }
    else return 1;
    std::ofstream output(path);
    if (!output) return 2;
    output << "scenario,backend,size,seed,sample,nanoseconds,calls,checksum\n";
    const auto measure = [&](std::string_view name, auto&& operation) {
        if (!selected.empty() && selected != name) return true;
        for (std::size_t sample{}; sample < 35U; ++sample)
        {
            std::uint64_t checksum{};
            const auto begin = std::chrono::steady_clock::now();
            for (std::int32_t index{}; index < 10000; ++index)
                checksum += static_cast<std::uint32_t>(operation(index ^ 2026));
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count();
            std::uint64_t expected{};
            for (std::int32_t index{}; index < 10000; ++index)
                expected += static_cast<std::uint32_t>((index ^ 2026) + 4);
            if (checksum != expected) return false;
            if (sample >= 5U)
                output << "tiny-ability," << name << ",10000,2026," << sample - 5U << ',' << elapsed
                       << ",10000," << checksum << '\n';
        }
        return true;
    };
    if (!measure("direct", [&](std::int32_t value) { return provider.read(value); }) ||
        !measure("typed-dynamic", [&](std::int32_t value) { return dynamic->read(value); }) ||
        !measure("typed-static-ipo", [&](std::int32_t value) { return specialized->read(value); }) ||
        !measure("native-provider-specialized", [&](std::int32_t value) {
            return bound_read(binding.context, dynamic_binding.dispatch, value);
        }) ||
        !measure("native-typed-entry", [&](std::int32_t value) {
            return native_read(binding.context, dynamic_binding.dispatch, value);
        })) return 3;
    return 0;
}
