#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/engine/function/script/BoundScriptCall.hpp>
#include <lux/engine/function/script/ScriptApi.hpp>
#include <lux/engine/function/script/ScriptSymbol.hpp>

#include <cstdint>

int main()
{
    lux::script::BoundScriptCall call;
    const auto type = lux::semantic::makeType<std::uint64_t>();
    constexpr lux::script::ScriptApiContractIdView contract{"consumer.Ability"};
    return !call && type.type_id != lux::semantic::InvalidTypeId && lux::script::InvalidScriptSymbolId == 0U &&
                   contract.isValid()
               ? 0
               : 1;
}
