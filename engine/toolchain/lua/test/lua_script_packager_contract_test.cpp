#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>

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
    assert(description.exports.size() == 1U);
    assert(description.exports[0].name == "tick");
    assert(description.exports[0].args.size() == 3U);
    assert(description.exports[0].args[0].canonical_name ==
        "lux.simulation.SimulationStepInfo");
    assert(description.exports[0].args[0].pass ==
        lux::semantic::EValuePass::CONST_REF);
    assert(description.exports[0].args[0].abi_kind == 10U);
    assert(description.exports[0].args[0].size == 16U);
    assert(description.exports[0].args[0].alignment == 8U);
    assert(description.exports[0].args[1].canonical_name ==
        "lux.test.CollisionEvent");
    assert(description.exports[0].args[1].pass ==
        lux::semantic::EValuePass::CONST_REF);
    assert(description.exports[0].args[1].abi_kind == 10U);
    assert(description.exports[0].args[1].size == 8U);
    assert(description.exports[0].args[1].alignment == 4U);
    assert(description.exports[0].args[2].canonical_name == "lux.f32");
    assert(description.exports[0].returns[0].canonical_name == "lux.i32");
    const auto encoded = lux::asset::TAssetSerDeser<
        lux::script::ScriptArtifactAsset>::encode(
        **decoded,
        lux::asset::AssetEncodeLimits{std::numeric_limits<std::size_t>::max()}
    );
    assert(encoded);
    assert(*encoded == bytes);
}
