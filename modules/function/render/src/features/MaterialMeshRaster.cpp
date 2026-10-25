#include <lux/engine/render/extensions/storage/IndexedTriangleMesh.hpp>
#include <lux/engine/render/extensions/storage/RasterMaterialTable.hpp>
#include <lux/engine/render/extensions/storage/RasterMeshInstances.hpp>
#include <lux/engine/render/extensions/storage/SampledTexture2D.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

namespace lux::render
{

    const FeatureGraphOps& detail::MaterialMeshRaster::graph(bool paged_instances) noexcept
    {
        static const FeatureGraphOps contiguous{detail::describe<false>, detail::build<false, false>};
        static const FeatureGraphOps paged{detail::describe<false>, detail::build<false, true>};
        return paged_instances ? paged : contiguous;
    }
} // namespace lux::render
