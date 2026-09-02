#include "TestAbility.hpp"
#include "TestAbility.ability.generated.hpp"
#include "TestAbility.ability_lua.generated.hpp"

#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>
#include <lux/engine/function/script/lua/Lua.hpp>
#include <lux/engine/function/script/lua/ScriptAbilityLua.hpp>

#include <cassert>
#include <cstdint>
#include <string_view>

namespace
{
    struct TestProvider final
    {
        std::int32_t value{7};
        std::size_t calls{};

        std::int32_t readValue(std::int32_t input) noexcept
        {
            ++calls;
            return value + input;
        }

        void setValue(std::int32_t new_value) noexcept
        {
            ++calls;
            value = new_value;
        }

        std::uint64_t identity(std::uint64_t input) noexcept
        {
            return input;
        }

        const std::int32_t& borrowedValue() noexcept
        {
            return value;
        }

        lux::script::ScriptAbilityStartResult beginOperation(
            std::uint64_t,
            lux::script::ScriptAbilityCompletion<std::uint64_t>
        ) noexcept
        {
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{74});
        }
    };
}

int main()
{
    using Ability = lux::simulation::test::TestAbility;
    using Traits = lux::script::ScriptAbilityTraits<Ability>;

    const auto nodes = lux::flowforge::scriptAbilityNodes<Ability>();
    assert(nodes.size() == Traits::Description.methods.size());
    for (std::size_t index{}; index < nodes.size(); ++index)
    {
        assert(nodes[index].contract == Traits::Description.id);
        assert(nodes[index].method == Traits::Description.methods[index].id);
        assert(nodes[index].schema_hash == Traits::Description.schema_hash);
        assert(nodes[index].kind == Traits::Description.methods[index].kind);
    }

    TestProvider provider;
    const auto binding = lux::script::bindScriptAbility<Ability>(provider);
    lux::script::lua::ScriptEngine engine;
    auto projected = lux::script::lua::projectScriptAbility<Ability>(*engine.state(), binding);
    assert(projected);
    auto program = engine.parseScript(
        "assert(lux.TestValue.readValue(5) == 12); lux.TestValue.setValue(31); "
        "assert(lux.TestValue.readValue(1) == 32)"
    );
    assert(program);
    assert(engine.runScript(*program));
    assert(provider.value == 31);
    assert(provider.calls == 3U);
    return 0;
}
