#include <lux/engine/render/features/RasterMesh.hpp>
#include <lux/engine/render/runtime/Registration.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMaterialTableCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceCommandHandler.hpp>

namespace lux::render
{
    const FeatureDescriptor MeshDepthRasterDescriptor::
        descriptor{name, cppType<Config>(), "Depth raster passes over indexed triangle meshes and raster instances.", detail::describe<true>};

    template <>
    RenderResult<void> RenderServer::prepareFeature<MeshDepthRasterDescriptor>(
        RegistrationBatch& batch,
        std::string_view
    ) noexcept
    {
        if (auto result = prepareStorage<IndexedTriangleMeshStorageDescriptor>(batch, {}, true, {}); !result)
        {
            return result;
        }
        if (auto result = prepareStorage<RasterMaterialTableStorageDescriptor>(batch, {}, true, {}); !result)
        {
            return result;
        }
        if (auto result = prepareStorage<RasterMeshInstanceStorageDescriptor>(batch, {}, true, {}); !result)
        {
            return result;
        }
        static const FeatureGraphOps contiguous{detail::build<true, false>};
        static const FeatureGraphOps paged{detail::build<true, true>};
        batch.addFeature({&MeshDepthRasterDescriptor::descriptor, pagedInstances() ? &paged : &contiguous});
        return {};
    }

    template LUX_RENDER_PUBLIC RenderResult<FeatureId> RenderServer::registerFeature<MeshDepthRasterDescriptor>(
        std::string_view
    ) noexcept;
} // namespace lux::render
