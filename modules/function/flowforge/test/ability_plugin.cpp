#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>

namespace
{
    using namespace lux::script;
    constexpr ScriptAbilityParameterDescription parameters[]{
        {"argument",
         {lux::semantic::typeId("lux.i32"),
          "lux.i32",
          lux::semantic::EValuePass::VALUE,
          static_cast<std::uint8_t>(lux::semantic::EAbiKind::I32),
          4,
          4,
          EScriptAbilityValueLifetime::OWNED_VALUE}}
    };
    constexpr lux::flowforge::ScriptAbilityNodeDescription description{
        ScriptApiContractIdView{"external.dll"},
        ScriptApiMethodIdView{"compute"},
        "DLL ability",
        "Compute",
        1,
        819,
        EScriptAbilityReceiverKind::NONE,
        EScriptApiMethodKind::COMMAND,
        parameters,
        {}
    };
} // namespace

#if defined(_WIN32)
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" TEST_EXPORT const lux::flowforge::ScriptAbilityNodeDescription* abilityDescription() noexcept
{
    return &description;
}
