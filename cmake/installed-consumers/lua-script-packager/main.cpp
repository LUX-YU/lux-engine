#include "InventoryAbility.hpp"
#include "InventoryAbility.ability.generated.hpp"

#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <vector>

int main()
{
    std::ifstream input(LUX_LUA_ARTIFACT, std::ios::binary);
    assert(input);
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    assert(size > 0);
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    assert(input && bytes.size() >= 56U);

    std::array<std::uint8_t, 16U> id_bytes{};
    for (std::size_t index{}; index < id_bytes.size(); ++index)
        id_bytes[index] = std::to_integer<std::uint8_t>(bytes[40U + index]);
    const auto decoded = lux::asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(
        lux::asset::AssetId{id_bytes},
        lux::cxx::SharedBytes<>::copyOf(bytes),
        {bytes.size(), (std::numeric_limits<std::size_t>::max)(), 0U}
    );
    assert(decoded);
    const auto& description = (*decoded)->data().description();
    using Traits = lux::script::ScriptAbilityTraits<installed_consumer::InventoryAbility>;
    assert(description.api_requirements.size() == 1U);
    assert(description.api_requirements.front().contract.name() == Traits::Description.id.name());
    assert(description.api_requirements.front().expected_schema_hash == Traits::Description.schema_hash);
    assert(description.lifecycle.begin_play == 1U);
    assert(description.lifecycle.end_play == 2U);
    const auto& lua = std::get<lux::rdesc::LuaSourceScript>(description.body);
    assert(lua.suspension_capable_exports == std::vector<lux::script::ScriptSymbolId>{3U});
    const auto contribution = lux::script::lua::makeScriptAbilityLuaContribution<
        installed_consumer::InventoryAbility
    >();
    const auto backend = lux::simulation::script::LuaScriptBackend::create({
        1U,
        4U,
        1U,
        4U,
        Traits::Description.methods.size(),
        {},
        {},
        {&contribution, 1U}
    });
    assert(backend);
    return 0;
}
