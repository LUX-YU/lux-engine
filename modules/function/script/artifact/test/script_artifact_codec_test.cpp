#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>

#include <cassert>
#include <limits>
#include <memory>

int main()
{
    using namespace lux;

    rdesc::Script description;
    description.module_name = "lux.test.artifact";
    description.exports.push_back({"tick", 11U, {}, {}});
    description.body = rdesc::LuaSourceScript{"module"};
    auto source_result = script::ScriptArtifact::create(
        std::move(description),
        {std::byte{1U}, std::byte{2U}, std::byte{3U}}
    );
    assert(source_result);
    auto source = std::move(*source_result);

    const auto descriptor = script::scriptArtifactCodecDescriptor(std::make_shared<int>(1));
    const asset::AssetCodecLimits limits{
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max()
    };
    auto encoded = descriptor.encode(&source, asset::AssetEncodeContext{limits});
    assert(encoded);
    assert((*encoded)[4] == std::byte{3U});

    auto decoded = descriptor.decode(*encoded, asset::AssetDecodeContext{limits});
    assert(decoded);
    const auto artifact = std::static_pointer_cast<const script::ScriptArtifact>(decoded->payload);
    assert(artifact->description().schema_version == 5U);
    assert(artifact->description().exports.front().symbol_id == 11U);
    assert(artifact->payload().size() == source.payload().size());
    assert(artifact->findExport(11U) == std::addressof(artifact->description().exports.front()));
    assert(artifact->findExport(12U) == nullptr);

    auto old_schema = *encoded;
    old_schema[8] = std::byte{3U};
    assert(!descriptor.decode(old_schema, asset::AssetDecodeContext{limits}));
    auto old_wire = *encoded;
    old_wire[4] = std::byte{1U};
    assert(!descriptor.decode(old_wire, asset::AssetDecodeContext{limits}));
    auto corrupt_kind = *encoded;
    corrupt_kind[12] = std::byte{0xFFU};
    assert(!descriptor.decode(corrupt_kind, asset::AssetDecodeContext{limits}));
    assert(!descriptor.decode(
        *encoded,
        asset::AssetDecodeContext{asset::AssetCodecLimits{encoded->size(), 1U, 0U}}
    ));
    auto trailing = *encoded;
    trailing.push_back(std::byte{});
    assert(!descriptor.decode(trailing, asset::AssetDecodeContext{limits}));
    return 0;
}
