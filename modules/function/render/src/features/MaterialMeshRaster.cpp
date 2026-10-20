#include <lux/engine/render/features/RasterMesh.hpp>
#include <lux/engine/render/runtime/Registration.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMaterialTableCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceCommandHandler.hpp>
#include <lux/engine/render/storage/SampledTexture2DCommandHandler.hpp>

namespace lux::render
{
    const FeatureDescriptor MaterialMeshRasterDescriptor::descriptor{
        name,
        cppType<Config>(),
        "Material color and depth raster passes over indexed triangle meshes and raster instances."
    };

    template <>
    RenderResult<void> RenderServer::prepareFeature<MaterialMeshRasterDescriptor>(
        RegistrationBatch& batch,
        std::string_view
    ) noexcept
    {
        if (auto result = prepareStorage<SampledTexture2DStorageDescriptor>(batch, {}, true, {}); !result)
        {
            return result;
        }
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
        static const FeatureGraphOps graph{detail::describe<false>, detail::build<false>};
        batch.addFeature({&MaterialMeshRasterDescriptor::descriptor, &graph, detail::programs<false>});
        return {};
    }

    template LUX_RENDER_PUBLIC RenderResult<FeatureId> RenderServer::registerFeature<MaterialMeshRasterDescriptor>(
        std::string_view
    ) noexcept;
} // namespace lux::render
