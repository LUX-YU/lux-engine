#include <lux/engine/function/render/standard/content/ClassicMeshBatch.hpp>

int
main()
{
    lux::classic_mesh::ClassicMeshBatchBlobV1 source;
    static_cast<void>(lux::classic_mesh::validateClassicMeshBatchBlob(source));
    return 0;
}
