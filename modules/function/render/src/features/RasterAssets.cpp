#include <lux/engine/render/capabilities/RasterAssets.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>

namespace lux::render::detail
{
    namespace
    {
        template <class T> RenderPayloadContract resourceInput(std::string_view schema, std::string_view description)
        {
            RenderPayloadContract result;
            result.schema = schema;
            result.use = ERenderPayloadUse::RESOURCE_INPUT;
            result.lifetime = ERenderPayloadLifetime::UNTIL_RESPONSE;
            result.description = description;
            result.setCppType(cxx::typeToken<T>());
            return result;
        }
    } // namespace

    const FeatureGraphOps& RasterAssets::graph(bool) noexcept
    {
        static const std::array inputs{
            CapabilityStorageInput{kIndexedMeshInput, 1, ECapabilityInputScope::GLOBAL, kMeshStorageType, "assets"},
            CapabilityStorageInput{
                kSampledTextureInput,
                1,
                ECapabilityInputScope::GLOBAL,
                kTextureStorageType,
                "assets"
            },
            CapabilityStorageInput{
                kRasterMaterialInput,
                1,
                ECapabilityInputScope::GLOBAL,
                kMaterialStorageType,
                "assets"
            }
        };
        static const FeatureGraphOps graph{.storage_inputs = inputs};
        return graph;
    }

    const CapabilitySupport& RasterAssets::capabilities() noexcept
    {
        static const CapabilitySupport support = []
        {
            CapabilitySupport result;
            result.implementation = name.name();
            result.description = "Shared indexed geometry, sampled textures and cooked raster materials.";
            RenderCapability capability;
            capability.name = kRasterAssetsCapability;
            capability.description = result.description;
            capability.inputs = {
                resourceInput<MeshWrite>(
                    kIndexedMeshInput,
                    "Borrowed interleaved vertices and indices through Response."
                ),
                resourceInput<TextureWrite>(kSampledTextureInput, "Borrowed texture mip bytes through Response."),
                resourceInput<MaterialWrite>(kRasterMaterialInput, "Owned cooked program and material values.")
            };
            capability.inputs.back().lifetime = ERenderPayloadLifetime::MOVED_VALUE;
            capability.outputs = {"mesh", "texture", "material"};
            result.capabilities.push_back(std::move(capability));
            return result;
        }();
        return support;
    }
} // namespace lux::render::detail
