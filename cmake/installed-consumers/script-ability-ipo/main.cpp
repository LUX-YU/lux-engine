#include "TinyAbility.hpp"
#include "TinyAbility.ability.generated.hpp"
#include "TinyAbility.ability.native.generated.hpp"

#include <cassert>
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
    const auto dynamic = lux::script::ScriptAbilityCpp<Ability>::create(binding);
    const auto specialized = lux::script::ScriptAbilityStatic<Ability, Provider>::create(provider, binding);
    const auto native = lux::script::native::makeScriptAbilityNativeContribution<Ability>();
    assert(dynamic && specialized && native.valid() && native.methods.size() == 1U);
    using NativeRead = std::int32_t (*)(void*, const void*, std::int32_t) noexcept;
    const auto native_read = reinterpret_cast<NativeRead>(native.methods.front().entry);
    assert(provider.read(3) == 7);
    assert(dynamic->read(3) == 7);
    assert(specialized->read(3) == 7);
    assert(native_read(binding.context, binding.dispatch, 3) == 7);
    if (argc == 1) return 0;
    if (argc != 3 || std::string_view{argv[1]} != "--output") return 1;
    std::ofstream output(argv[2]);
    if (!output) return 2;
    output << "scenario,backend,size,seed,sample,nanoseconds,calls,checksum\n";
    const auto measure = [&](std::string_view name, auto&& operation) {
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
        !measure("native-typed-entry", [&](std::int32_t value) {
            return native_read(binding.context, binding.dispatch, value);
        })) return 3;
    return 0;
}
