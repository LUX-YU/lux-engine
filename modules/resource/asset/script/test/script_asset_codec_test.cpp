#include <lux/engine/resource/asset/script/ScriptAsset.hpp>

#include <cassert>
#include <limits>
#include <memory>

int
main()
{
    using namespace lux;

    asset::ScriptAssetContent source;
    source.description.module_name = "lux.test.asset";
    source.description.model = rdesc::EScriptModel::ENTITY_BEHAVIOR;
    source.description.exports.push_back({"tick", 11U, {}, {}});
    source.description.body = rdesc::LuaSourceScript{"module"};
    source.payload = {std::byte{1U}, std::byte{2U}, std::byte{3U}};

    const auto descriptor = asset::scriptAssetCodecDescriptor(std::make_shared<int>(1));
    const asset::AssetCodecLimits limits{
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max()};
    auto encoded = descriptor.encode(&source, asset::AssetEncodeContext{limits});
    assert(encoded);
    assert((*encoded)[4] == std::byte{2U});

    auto decoded = descriptor.decode(*encoded, asset::AssetDecodeContext{limits});
    assert(decoded);
    const auto content = std::static_pointer_cast<const asset::ScriptAssetContent>(decoded->payload);
    assert(content->description.schema_version == 4U);
    assert(content->description.model == rdesc::EScriptModel::ENTITY_BEHAVIOR);
    assert(content->description.exports.front().symbol_id == 11U);
    assert(content->payload == source.payload);

    auto old_schema = *encoded;
    old_schema[8] = std::byte{3U};
    assert(!descriptor.decode(old_schema, asset::AssetDecodeContext{limits}));
    auto old_wire = *encoded;
    old_wire[4] = std::byte{1U};
    assert(!descriptor.decode(old_wire, asset::AssetDecodeContext{limits}));
    auto corrupt_model = *encoded;
    corrupt_model[16] = std::byte{0xFFU};
    assert(!descriptor.decode(corrupt_model, asset::AssetDecodeContext{limits}));
    assert(!descriptor.decode(*encoded, asset::AssetDecodeContext{asset::AssetCodecLimits{encoded->size(), 1U, 0U}}));
    auto trailing = *encoded;
    trailing.push_back(std::byte{});
    assert(!descriptor.decode(trailing, asset::AssetDecodeContext{limits}));
    return 0;
}
