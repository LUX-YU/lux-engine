#include <lux/engine/render/extensions/storage/IndexedTriangleMesh.hpp>
#include <lux/engine/render/extensions/storage/RasterMaterialTable.hpp>
#include <lux/engine/render/extensions/storage/RasterMeshInstances.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

namespace lux::render
{

    const FeatureGraphOps& detail::MeshDepthRaster::graph(bool paged_instances) noexcept
    {
        static const FeatureGraphOps contiguous{detail::describe<true>, detail::build<true, false>};
        static const FeatureGraphOps paged{detail::describe<true>, detail::build<true, true>};
        return paged_instances ? paged : contiguous;
    }
} // namespace lux::render
