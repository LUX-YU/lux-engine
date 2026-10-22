#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/MeshDepthRaster.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>
#include <lux/engine/render/storage/IndexedTriangleMesh.hpp>
#include <lux/engine/render/storage/RasterMaterialTable.hpp>
#include <lux/engine/render/storage/RasterMeshInstances.hpp>

namespace lux::render
{
    const FeatureDescriptor MeshDepthRasterDescriptor::
        descriptor{name, cxx::typeToken<Config>(), "Depth raster passes over indexed triangle meshes and raster instances.", detail::describe<true>};

    const FeatureGraphOps& detail::MeshDepthRaster::graph(bool paged_instances) noexcept
    {
        static const FeatureGraphOps contiguous{detail::build<true, false>};
        static const FeatureGraphOps paged{detail::build<true, true>};
        return paged_instances ? paged : contiguous;
    }
} // namespace lux::render
