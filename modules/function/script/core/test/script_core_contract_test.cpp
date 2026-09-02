#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/engine/function/script/BoundScriptCall.hpp>
#include <lux/engine/function/script/ScriptApi.hpp>
#include <lux/engine/function/script/ScriptSymbol.hpp>

#include <cassert>
#include <cstdint>
#include <type_traits>

int main()
{
    static_assert(lux::script::InvalidScriptSymbolId == 0U);
    static_assert(std::is_trivially_copyable_v<lux::script::BoundScriptCall>);
    static_assert(lux::semantic::typeId("lux.i32") == lux::semantic::makeType<std::int32_t>().type_id);
    static_assert(lux::semantic::makeType<std::int32_t>().canonical_name == "lux.i32");
    constexpr lux::script::ScriptApiContractIdView contract{"lux.test.Ability"};
    constexpr lux::script::ScriptApiMethodIdView method{"lux.test.Ability.read"};
    static_assert(contract.isValid() && method.isValid());
    static_assert(contract.hash() == lux::cxx::Fnv1a64::hash(contract.name()));
    static_assert(lux::script::EScriptApiMethodKind::ASYNC_OPERATION != lux::script::EScriptApiMethodKind::QUERY);

    lux::script::BoundScriptCall call;
    assert(!call);
    return 0;
}
