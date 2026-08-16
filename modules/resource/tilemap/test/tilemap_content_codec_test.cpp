#include <lux/engine/resource/tilemap/TilemapChunk.hpp>

#include <cstdlib>
#include <utility>
#include <vector>

namespace
{
    void require(bool condition)
    {
        if (!condition)
            std::abort();
    }
}

int main()
{
    using namespace lux::tilemap;

    require(kTilemapChunkContentTypeName == "lux.tilemap.chunk2d");
    require(kTilemapChunkTileCount == 65536u);

    TilemapChunkBlobV1 source;
    source.tiles.resize(kTilemapChunkTileCount);
    for (std::size_t index = 0u; index < source.tiles.size(); ++index)
        source.tiles[index] = static_cast<std::uint16_t>(index);

    auto encoded = encodeTilemapChunkBlob(source);
    require(encoded.has_value());
    auto decoded = decodeTilemapChunkBlob(*encoded);
    require(decoded.has_value());
    require(*decoded == source);
    auto encoded_again = encodeTilemapChunkBlob(*decoded);
    require(encoded_again.has_value());
    require(*encoded_again == *encoded);

    auto truncated = *encoded;
    truncated.pop_back();
    auto truncated_result = decodeTilemapChunkBlob(truncated);
    require(!truncated_result);
    require(truncated_result.error().error ==
        ETilemapChunkCodecError::TRUNCATED);

    auto trailing = *encoded;
    trailing.push_back(std::byte{0u});
    auto trailing_result = decodeTilemapChunkBlob(trailing);
    require(!trailing_result);
    require(trailing_result.error().error ==
        ETilemapChunkCodecError::TRAILING_BYTES);

    auto bad_magic = *encoded;
    bad_magic[0u] ^= std::byte{0xffu};
    auto bad_magic_result = decodeTilemapChunkBlob(bad_magic);
    require(!bad_magic_result);
    require(bad_magic_result.error().error ==
        ETilemapChunkCodecError::BAD_MAGIC);

    auto invalid = source;
    invalid.tiles.pop_back();
    require(!validateTilemapChunkBlob(invalid));
    require(!encodeTilemapChunkBlob(invalid));
    return 0;
}
