#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/MaterialMeshRaster.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>
#include <lux/engine/render/storage/IndexedTriangleMesh.hpp>
#include <lux/engine/render/storage/RasterMaterialTable.hpp>
#include <lux/engine/render/storage/RasterMeshInstances.hpp>
#include <lux/engine/render/storage/SampledTexture2D.hpp>

namespace lux::render
{
    const FeatureDescriptor MaterialMeshRasterDescriptor::
        descriptor{name, cxx::typeToken<Config>(), "Material color and depth raster passes over indexed triangle meshes and raster instances.", detail::describe<false>};

    const FeatureGraphOps& detail::MaterialMeshRaster::graph(bool paged_instances) noexcept
    {
        static const FeatureGraphOps contiguous{detail::build<false, false>};
        static const FeatureGraphOps paged{detail::build<false, true>};
        return paged_instances ? paged : contiguous;
    }
} // namespace lux::render
