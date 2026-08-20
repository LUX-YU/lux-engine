#include <lux/engine/resource/asset/codecs/StaticColliderBatch3DCodec.hpp>
#include <lux/engine/resource/asset/codecs/TerrainTileCodec.hpp>
#include <lux/engine/resource/asset/codecs/TilemapChunkCodec.hpp>

int main()
{
    const lux::terrain::TerrainTileBlobV1 terrain;
    const lux::tilemap::TilemapChunkBlobV1 tilemap;
    const lux::physics3d::StaticColliderBatch3DBlobV1 physics;

    static_cast<void>(lux::terrain::validateTerrainTileBlob(terrain));
    static_cast<void>(lux::tilemap::validateTilemapChunkBlob(tilemap));
    static_cast<void>(
        lux::physics3d::validateStaticColliderBatch3DBlob(physics));
    return 0;
}
