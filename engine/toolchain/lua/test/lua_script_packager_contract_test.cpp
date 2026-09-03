#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>

#include "DelayAbility.ability.generated.hpp"

#include <cassert>
#include <array>
#include <cstddef>
#include <fstream>
#include <limits>
#include <vector>

int main()
{
    std::ifstream input(LUX_LUA_FIXTURE_LXSA, std::ios::binary);
    assert(input);
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    assert(size > 0);
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(size)
    );
    assert(input);

    std::array<std::uint8_t, 16U> id_bytes{};
    for (std::size_t index = 0U; index < id_bytes.size(); ++index)
        id_bytes[index] = std::to_integer<std::uint8_t>(bytes[40U + index]);
    const lux::asset::AssetId requested{id_bytes};
    const auto decoded = lux::asset::TAssetSerDeser<
        lux::script::ScriptArtifactAsset>::decode(
        requested,
        lux::cxx::SharedBytes<>::copyOf(bytes),
        lux::asset::AssetDecodeLimits{
            bytes.size(),
            std::numeric_limits<std::size_t>::max(),
            0U
        }
    );
    assert(decoded);
    const auto& description = (*decoded)->data().description();
    assert(description.kind() == lux::rdesc::Script::Kind::LUA_SOURCE);
    assert(description.lifecycle.begin_play == 2U);
    assert(description.lifecycle.end_play == 3U);
    assert(description.api_requirements.size() == 1U);
    assert(description.api_requirements[0].contract.name() == "lux.simulation.delay");
    assert(description.api_requirements[0].expected_schema_hash ==
        lux::script::ScriptAbilityTraits<lux::simulation::script::DelayAbility>::Description.schema_hash);
    const auto& lua = std::get<lux::rdesc::LuaSourceScript>(description.body);
    assert(lua.suspension_capable_exports == std::vector<lux::script::ScriptSymbolId>{4U});
    assert(description.exports.size() == 4U);
    const auto tick = std::ranges::find_if(description.exports, [](const auto& function) {
        return function.name == "tick";
    });
    assert(tick != description.exports.end());
    assert(tick->args.size() == 3U);
    assert(tick->args[0].canonical_name ==
        "lux.simulation.SimulationStepInfo");
    assert(tick->args[0].pass ==
        lux::semantic::EValuePass::CONST_REF);
    assert(tick->args[0].abi_kind == 10U);
    assert(tick->args[0].size == 16U);
    assert(tick->args[0].alignment == 8U);
    assert(tick->args[1].canonical_name ==
        "lux.test.CollisionEvent");
    assert(tick->args[1].pass ==
        lux::semantic::EValuePass::CONST_REF);
    assert(tick->args[1].abi_kind == 10U);
    assert(tick->args[1].size == 8U);
    assert(tick->args[1].alignment == 4U);
    assert(tick->args[2].canonical_name == "lux.f32");
    assert(tick->returns[0].canonical_name == "lux.i32");
    const auto end = std::ranges::find_if(description.exports, [&](const auto& function) {
        return function.symbol_id == description.lifecycle.end_play;
    });
    assert(end != description.exports.end() && end->args.size() == 1U);
    assert(end->args[0].canonical_name == "lux.simulation.ScriptEndPlayReason");
    assert(end->args[0].abi_kind == LUX_SCRIPT_VK_UINT32);
    const auto encoded = lux::asset::TAssetSerDeser<
        lux::script::ScriptArtifactAsset>::encode(
        **decoded,
        lux::asset::AssetEncodeLimits{std::numeric_limits<std::size_t>::max()}
    );
    assert(encoded);
    assert(*encoded == bytes);
}
