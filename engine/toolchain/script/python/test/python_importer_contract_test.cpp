#include <lux/engine/resource/asset/script/ScriptAsset.hpp>

#include <cassert>
#include <cstddef>
#include <fstream>
#include <limits>
#include <vector>

int main()
{
    std::ifstream input(LUX_PYTHON_FIXTURE_LXSA, std::ios::binary);
    assert(input);
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    assert(size > 0);
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    assert(input);

    const auto codec = lux::asset::scriptAssetCodecDescriptor({});
    const lux::asset::AssetCodecLimits limits{
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max()};
    const auto decoded = codec.decode(
        bytes,
        lux::asset::AssetDecodeContext{limits}
    );
    assert(decoded);
    const auto asset = std::static_pointer_cast<
        const lux::asset::ScriptAssetContent>(decoded->payload);
    assert(asset->description.schema_version == 4U);
    assert(asset->description.kind() ==
        lux::rdesc::Script::Kind::PYTHON_SOURCE);
    assert(asset->description.model ==
        lux::rdesc::EScriptModel::ENTITY_BEHAVIOR);
    assert(asset->description.exports.size() == 1U);
    assert(asset->description.exports[0].name == "tick");
    assert(asset->description.exports[0].args.size() == 3U);
    assert(asset->description.exports[0].args[0].canonical_name ==
        "lux.simulation.SimulationStepInfo");
    assert(asset->description.exports[0].args[0].pass ==
        lux::script::EScriptPassMode::CONST_REF);
    assert(asset->description.exports[0].args[1].canonical_name ==
        "lux.test.CollisionEvent");
    assert(asset->description.exports[0].args[1].pass ==
        lux::script::EScriptPassMode::CONST_REF);
    assert(asset->description.exports[0].args[2].canonical_name == "lux.f32");
    assert(asset->description.exports[0].returns[0].canonical_name == "lux.i32");
    const auto encoded = codec.encode(
        asset.get(),
        lux::asset::AssetEncodeContext{limits}
    );
    assert(encoded);
    assert(*encoded == bytes);
}
