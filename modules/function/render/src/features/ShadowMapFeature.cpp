#include <lux/engine/render/features/ShadowMapFeature.hpp>

#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/FeatureDefinition.hpp>
#include <lux/engine/render/features/ShadowProjectionBinding.hpp>
#include <lux/engine/render/runtime/Registration.hpp>
#include <lux/engine/render/storage/DirectLightingCommandHandler.hpp>

namespace lux::render
{
    template <> struct ConfigValueTraits<detail::ShadowProjectionBinding::Description> final
    {
        static RenderResult<std::size_t> retainedBytes(const detail::ShadowProjectionBinding::Description& value
        ) noexcept
        {
            auto bytes = ConfigValueTraits<ShadowMapFeatureConfig>::retainedBytes(value.config);
            return bytes ? RenderResult<std::size_t>{*bytes + sizeof(value) - sizeof(value.config)}
                         : cxx::unexpected(bytes.error());
        }
    };

    namespace
    {
        RenderResult<void> describeShadows(const void* value, std::string_view, FeaturePorts& ports) noexcept
        {
            const auto& config = *static_cast<const ShadowMapFeatureConfig*>(value);
            if (auto valid = detail::validateShadowConfiguration(config); !valid)
            {
                return valid;
            }
            const auto add = [&](std::string name, std::uint64_t bytes, std::string_view semantic)
            {
                FeaturePort port{
                    std::move(name),
                    EFeaturePort::OUTPUT,
                    GraphBufferDesc{bytes},
                    EGraphOrigin::IMPORTED,
                    {GraphBufferRange{0, bytes}}
                };
                port.semantic = semantic;
                ports.resources.push_back(std::move(port));
            };
            add("views", std::uint64_t(config.shape.view_count) * sizeof(MeshVisibilityView), kMeshVisibilityViews);
            add("slices", std::uint64_t(config.shape.view_count) * sizeof(ShadowSliceGpu), kShadowSlices);
            add("light_ranges", sizeof(ShadowMappingHeader) + sizeof(LightShadowRange), kLightShadowRanges);
            add("raster_states", std::uint64_t(config.shape.view_count) * sizeof(RasterState), "lux.raster.state.v1");
            constexpr std::array<std::string_view, 4>
                names{"directional_lights", "point_lights", "spot_lights", "light_counts"};
            constexpr std::array<std::uint32_t, 4> sizes{32, 48, 64, 16};
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                ports.storage.scene.push_back(
                    {std::string(names[i]),
                     EFeaturePort::OUTPUT,
                     GraphBufferDesc{sizes[i]},
                     EGraphOrigin::IMPORTED,
                     {GraphBufferRange{0, sizes[i]}},
                     StorageRequirement{StorageName(kDirectLightingStorageType.name()), config.lights}}
                );
            }
            return {};
        }

        RenderResult<void> buildShadows(const void* value, std::string_view, FeatureBuildContext& context) noexcept
        {
            const auto& config = *static_cast<const ShadowMapFeatureConfig*>(value);
            const std::array<GraphResourceId, 4> outputs{
                context.resource("views").resource,
                context.resource("slices").resource,
                context.resource("light_ranges").resource,
                context.resource("raster_states").resource
            };
            constexpr std::array<std::string_view, 4>
                names{"directional_lights", "point_lights", "spot_lights", "light_counts"};
            for (std::uint32_t i = 0; i < names.size(); ++i)
            {
                context.arguments.push_back(StorageArgument::make(
                    context.resource(names[i]).resource,
                    DirectLightingBinding{static_cast<EDirectLightingBuffer>(i)}
                ));
            }
            const auto lights = context.resource("light_counts").resource;
            auto description = ConfigValue::make(detail::ShadowProjectionBinding::Description{config, outputs, lights});
            if (!description)
            {
                return cxx::unexpected(description.error());
            }
            context.bindings.push_back(
                {{outputs.begin(), outputs.end()},
                 std::move(*description),
                 &detail::ShadowProjectionBinding::create,
                 {lights},
                 config.camera_offset + sizeof(ShadowCamera)}
            );
            return {};
        }
    } // namespace

    RenderResult<std::size_t> ConfigValueTraits<ShadowMapFeatureConfig>::retainedBytes(
        const ShadowMapFeatureConfig& value
    ) noexcept
    {
        auto bytes = sizeof(value) + value.casters.capacity() * sizeof(ShadowLightSelection);
        const auto address = reinterpret_cast<std::uintptr_t>(value.lights.data());
        const auto begin = reinterpret_cast<std::uintptr_t>(&value.lights);
        if (address < begin || address >= begin + sizeof(value.lights))
        {
            bytes += value.lights.capacity() + 1;
        }
        for (const auto& caster : value.casters)
        {
            bytes += caster.cascade_far_distances.capacity() * sizeof(float);
        }
        return bytes;
    }

    const FeatureDescriptor ShadowMapFeatureDescriptor::descriptor{
        name,
        cppType<Config>(),
        "Explicit shadow light selection, projections and dense-light mapping; no raster work.",
        describeShadows
    };

    template <>
    RenderResult<void> RenderServer::prepareFeature<ShadowMapFeatureDescriptor>(
        RegistrationBatch& batch,
        std::string_view
    ) noexcept
    {
        if (auto result = prepareStorage<DirectLightingStorageDescriptor>(batch, {}, true, {}); !result)
        {
            return result;
        }
        static const FeatureGraphOps graph{buildShadows};
        batch.addFeature({&ShadowMapFeatureDescriptor::descriptor, &graph});
        return {};
    }

    template LUX_RENDER_PUBLIC RenderResult<FeatureId> RenderServer::registerFeature<ShadowMapFeatureDescriptor>(
        std::string_view
    ) noexcept;
} // namespace lux::render
