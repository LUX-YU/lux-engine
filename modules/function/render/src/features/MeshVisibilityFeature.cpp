#include <lux/engine/render/features/MeshVisibilityFeature.hpp>

#include "MeshCompact.hpp"
#include "MeshFrustumCull.hpp"
#include "MeshFrustumCullPaged.hpp"
#include "MeshView.hpp"
#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/FeatureDefinition.hpp>
#include <lux/engine/render/features/ParameterLayout.hpp>
#include <lux/engine/render/features/RasterMeshDraws.hpp>
#include <lux/engine/render/runtime/Registration.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMaterialTableCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceCommandHandler.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<void> describeVisibility(const void* value, std::string_view, FeaturePorts& ports) noexcept
        {
            const auto& config = *static_cast<const MeshVisibilityFeatureConfig*>(value);
            const bool invalid = !config.view_count || !config.candidate_capacity || config.groups.empty() ||
                                 config.candidate_capacity > UINT32_MAX - 68 || config.instance_storage.empty() ||
                                 config.mesh_storage.empty();
            const bool oversized_stream =
                !invalid && meshVisibleStride(config.candidate_capacity) / 4 >
                                UINT32_MAX / (std::uint64_t(config.view_count) * kMeshWindingCount);
            const bool invalid_view_source = config.view_parameters && config.view_count != 1;
            if (invalid || oversized_stream || invalid_view_source)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            FeaturePort views{
                "views",
                config.view_parameters ? EFeaturePort::OUTPUT : EFeaturePort::INPUT,
                GraphBufferDesc{std::uint64_t(config.view_count) * sizeof(MeshVisibilityView)}
            };
            views.semantic = kMeshVisibilityViews;
            ports.resources.push_back(std::move(views));
            const auto storage = [&](auto& destination,
                                     std::string name,
                                     std::uint64_t size,
                                     StorageNameView type,
                                     const std::string& key)
            {
                destination.push_back(
                    {std::move(name),
                     EFeaturePort::OUTPUT,
                     GraphBufferDesc{size},
                     EGraphOrigin::IMPORTED,
                     {GraphBufferRange{0, size}},
                     StorageRequirement{StorageName(type.name()), key}}
                );
            };
            storage(
                ports.storage.scene,
                "transforms",
                sizeof(TransformMatrix),
                kMeshInstanceStorageType,
                config.instance_storage
            );
            storage(
                ports.storage.scene,
                "alive",
                sizeof(std::uint64_t),
                kMeshInstanceStorageType,
                config.instance_storage
            );
            for (std::size_t g = 0; g < config.groups.size(); ++g)
            {
                const auto prefix = "g" + std::to_string(g) + ".";
                storage(
                    ports.storage.global,
                    prefix + "bounds",
                    sizeof(MeshGpuBounds),
                    kMeshStorageType,
                    config.mesh_storage
                );
                ports.resources.push_back(
                    {prefix + "candidates",
                     EFeaturePort::OUTPUT,
                     GraphBufferDesc{24},
                     EGraphOrigin::IMPORTED,
                     {GraphBufferRange{0, 24}}}
                );
                FeaturePort visible{
                    prefix + "visible",
                    EFeaturePort::OUTPUT,
                    GraphBufferDesc{
                        meshVisibleStride(config.candidate_capacity) * config.view_count * kMeshWindingCount
                    }
                };
                visible.semantic = kMeshVisibleMembers;
                ports.resources.push_back(std::move(visible));
                ports.resources.push_back(
                    {prefix + "commands",
                     EFeaturePort::OUTPUT,
                     GraphBufferDesc{std::uint64_t(config.view_count) * kMeshWindingCount * 20}}
                );
                ports.resources.push_back(
                    {prefix + "counts",
                     EFeaturePort::OUTPUT,
                     GraphBufferDesc{std::uint64_t(config.view_count) * kMeshWindingCount * 4}}
                );
            }
            return {};
        }

        RenderResult<std::uint32_t> computeProgram(
            FeatureBuildContext& context,
            std::span<const std::uint32_t> words,
            std::span<const std::byte> reflection
        ) noexcept
        {
            rdesc::SpirvArtifact artifact;
            artifact.stages.push_back({rdesc::EShaderType::COMPUTE, "main", {words.begin(), words.end()}});
            if (!rdesc::ShaderInfo::deserialize(reflection, artifact.interface))
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            return context.graph.addProgram(std::move(artifact));
        }

        template <bool Paged>
        RenderResult<void> buildVisibility(
            const void* value,
            std::string_view key,
            FeatureBuildContext& context
        ) noexcept
        {
            const auto& config = *static_cast<const MeshVisibilityFeatureConfig*>(value);
            auto cull_program = Paged ? computeProgram(context, kMeshFrustumCullPaged, kMeshFrustumCullPagedReflection)
                                      : computeProgram(context, kMeshFrustumCull, kMeshFrustumCullReflection);
            auto compact_program = computeProgram(context, kMeshCompact, kMeshCompactReflection);
            if (!cull_program || !compact_program)
            {
                return cxx::unexpected(!cull_program ? cull_program.error() : compact_program.error());
            }
            auto views = context.resource("views");
            if (config.view_parameters)
            {
                auto program = computeProgram(context, kMeshView, kMeshViewReflection);
                if (!program)
                {
                    return cxx::unexpected(program.error());
                }
                ComputeWork work;
                work.interface.program = *program;
                work.interface.resources = {{"views", {views, {}, EGraphAccess::WRITE}}};
                work.interface.parameters.instance_layout = *config.view_parameters;
                work.interface.parameters.values.resize(sizeof(MeshViewParameters));
                work.interface.parameters.fields = {{"view", {}, "view"}, {"projection", {}, "projection"}};
                work.dispatch = DispatchArguments{{1, 1, 1}};
                work.allow_async_compute = true;
                auto built = context.graph.addComputePass(
                    featureKey(MeshVisibilityFeatureDescriptor::name, key, "view"),
                    std::move(work)
                );
                if (!built)
                {
                    return cxx::unexpected(built.error());
                }
                views = built->written.front();
                if (auto published = context.publish("views", views); !published)
                {
                    return published;
                }
            }
            context.arguments.push_back(StorageArgument::make(
                context.resource("transforms").resource,
                MeshInstanceBinding{EMeshInstanceBinding::TRANSFORMS}
            ));
            context.arguments.push_back(StorageArgument::make(
                context.resource("alive").resource,
                MeshInstanceBinding{EMeshInstanceBinding::ALIVE}
            ));
            for (std::size_t g = 0; g < config.groups.size(); ++g)
            {
                const auto prefix = "g" + std::to_string(g) + ".";
                const auto resource = [&](std::string_view name)
                { return context.resource(prefix + std::string(name)); };
                const auto candidates = resource("candidates");
                auto description = ConfigValue::make(detail::MeshDrawConfiguration{
                    context.resource("transforms").resource,
                    config.groups[g],
                    candidates.resource,
                    config.candidate_capacity
                });
                if (!description)
                {
                    return cxx::unexpected(description.error());
                }
                context.bindings.push_back(
                    {{candidates.resource}, std::move(*description), &detail::RasterMeshDraws::create}
                );
                context.arguments.push_back(StorageArgument::make(
                    resource("bounds").resource,
                    MeshBinding{config.groups[g].mesh, EMeshBuffer::BOUNDS, sizeof(MeshGpuBounds)}
                ));
                auto visible = resource("visible");
                // Initialize the declared stream. Only the count-delimited prefix is consumed by Draw.
                TransferWork reset;
                reset.operations.push_back(FillBuffer{{visible}, 0});
                auto cleared = context.graph.addTransferPass(
                    featureKey(MeshVisibilityFeatureDescriptor::name, key, prefix + "reset"),
                    std::move(reset)
                );
                if (!cleared)
                {
                    return cxx::unexpected(cleared.error());
                }
                visible = cleared->written.front();
                ComputeWork cull;
                cull.interface.program = *cull_program;
                cull.interface.resources = {
                    {"transforms", {context.resource("transforms")}},
                    {"alive", {context.resource("alive")}},
                    {"views", {views}},
                    {"candidates", {candidates}},
                    {"bounds", {resource("bounds")}},
                    {"visible", {visible, {}, EGraphAccess::READ_WRITE}}
                };
                const std::array<std::uint32_t, 3> parameters{
                    config.candidate_capacity,
                    config.view_count,
                    config.frustum_culling
                };
                const auto bytes = std::as_bytes(std::span(parameters));
                cull.interface.parameters.values.assign(bytes.begin(), bytes.end());
                cull.dispatch = DispatchArguments{{(config.candidate_capacity + 63) / 64, config.view_count, 1}};
                cull.allow_async_compute = true;
                auto culled = context.graph.addComputePass(
                    featureKey(MeshVisibilityFeatureDescriptor::name, key, prefix + "frustum"),
                    std::move(cull)
                );
                if (!culled)
                {
                    return cxx::unexpected(culled.error());
                }
                visible = culled->written.front();
                ComputeWork compact;
                compact.interface.program = *compact_program;
                compact.interface.resources = {
                    {"candidates", {candidates}},
                    {"visible", {visible}},
                    {"commands", {resource("commands"), {}, EGraphAccess::WRITE}},
                    {"counts", {resource("counts"), {}, EGraphAccess::WRITE}}
                };
                compact.interface.parameters.values.assign(bytes.begin(), bytes.begin() + 8);
                compact.dispatch = DispatchArguments{{(config.view_count * kMeshWindingCount + 63) / 64, 1, 1}};
                compact.allow_async_compute = true;
                auto completed = context.graph.addComputePass(
                    featureKey(MeshVisibilityFeatureDescriptor::name, key, prefix + "compact"),
                    std::move(compact)
                );
                if (!completed)
                {
                    return cxx::unexpected(completed.error());
                }
                if (auto result = context.publish(prefix + "visible", visible); !result)
                {
                    return result;
                }
                if (auto result = context.publish(prefix + "commands", completed->written[0]); !result)
                {
                    return result;
                }
                if (auto result = context.publish(prefix + "counts", completed->written[1]); !result)
                {
                    return result;
                }
            }
            return {};
        }
    } // namespace

    RenderResult<std::size_t> ConfigValueTraits<MeshVisibilityFeatureConfig>::retainedBytes(
        const MeshVisibilityFeatureConfig& value
    ) noexcept
    {
        return sizeof(value) + value.groups.capacity() * sizeof(MeshInstanceDrawGroup) +
               value.instance_storage.capacity() + value.mesh_storage.capacity() + 2 +
               (value.view_parameters ? detail::parameterLayoutBytes(*value.view_parameters) : 0);
    }

    const FeatureDescriptor MeshVisibilityFeatureDescriptor::descriptor{
        name,
        cppType<Config>(),
        "GPU frustum visibility and indexed indirect counts over explicit geometric views.",
        describeVisibility
    };

    template <>
    RenderResult<void> RenderServer::prepareFeature<MeshVisibilityFeatureDescriptor>(
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
        static const FeatureGraphOps contiguous{buildVisibility<false>}, paged{buildVisibility<true>};
        batch.addFeature({&MeshVisibilityFeatureDescriptor::descriptor, pagedInstances() ? &paged : &contiguous});
        return {};
    }

    template LUX_RENDER_PUBLIC RenderResult<FeatureId> RenderServer::registerFeature<MeshVisibilityFeatureDescriptor>(
        std::string_view
    ) noexcept;
} // namespace lux::render
