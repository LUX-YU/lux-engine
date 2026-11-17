#include <lux/engine/render/graph/FeatureBuildAccess.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/features/MeshVisibilityFeature.hpp>

#include "MeshCompact.hpp"
#include "MeshFrustumCull.hpp"
#include "MeshFrustumCullPaged.hpp"
#include "MeshReset.hpp"
#include "MeshView.hpp"
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/resources/material/Binding.hpp>
#include <lux/engine/render/resources/instance/Binding.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/MeshDrawGroups.hpp>
#include <lux/engine/render/features/ParameterLayout.hpp>

namespace lux::render
{
    constexpr std::array<std::string_view, 6>
        group_ports{"groups", "candidates", "chunks", "visible", "commands", "dispatch"};
    constexpr std::array<std::uint64_t, 6> group_minimum{64, 4, 8, 256, 20 * kMeshWindingCount, 24};

    RenderResult<void>
    detail::MeshVisibilityFeature::Ports::describe(const Config* value, std::string_view, FeaturePorts& ports) noexcept
    {
        const auto& config = *static_cast<const MeshVisibilityFeatureConfig*>(value);
        const bool invalid = config.view_count == 0 || config.instance_storage.empty();
        const bool invalid_view_source = config.view_parameters && config.view_count != 1;
        if (invalid || invalid_view_source)
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
        constexpr std::array names{"transforms", "properties", "alive"};
        constexpr std::array<std::uint64_t, 3> sizes{sizeof(TransformMatrix), sizeof(MeshInstanceGpuProperty), 8};
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            ports.storage.scene.push_back(
                {names[i],
                 EFeaturePort::OUTPUT,
                 GraphBufferDesc{sizes[i]},
                 EGraphOrigin::IMPORTED,
                 {GraphBufferRange{0, sizes[i]}},
                 ResourceRequirement{ResourceName(config.instance_storage)}}
            );
        }
        for (std::size_t i = 0; i < group_ports.size(); ++i)
        {
            const auto minimum = group_minimum[i] * (i == 4 ? config.view_count : 1);
            FeaturePort port{
                std::string(group_ports[i]),
                EFeaturePort::OUTPUT,
                GraphBufferDesc{minimum},
                EGraphOrigin::IMPORTED,
                {GraphBufferRange{0, minimum}}
            };
            if (i == 0)
            {
                port.semantic = "lux.mesh.draw_groups.v1";
            }
            if (i == 3)
            {
                port.semantic = kMeshVisibleMembers;
            }
            ports.resources.push_back(std::move(port));
        }
        return {};
    }

    RenderResult<std::uint32_t>
    computeProgram(FeatureBuildContext& context, GraphProgram shader) noexcept
    {
        return context.graph.addProgram(std::move(shader));
    }

    template <bool Paged>
    RenderResult<void>
    detail::MeshVisibilityFeature::build(
        const Config* value,
        std::string_view key,
        FeatureBuildContext& context
    ) noexcept
    {
        const auto& config = *static_cast<const MeshVisibilityFeatureConfig*>(value);
        auto cull_program = Paged ? computeProgram(context, GraphProgram{kMeshFrustumCullPaged})
                                  : computeProgram(context, GraphProgram{kMeshFrustumCull});
        auto compact_program = computeProgram(context, GraphProgram{kMeshCompact});
        if (!cull_program || !compact_program)
        {
            return cxx::unexpected(!cull_program ? cull_program.error() : compact_program.error());
        }
        auto views = context.resource("views");
        if (config.view_parameters)
        {
            auto program = computeProgram(context, GraphProgram{kMeshView});
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
                featureKey(detail::MeshVisibilityFeature::name, key, "view"),
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
        context.require(StorageArgument::make(
            context.resource("transforms").resource,
            MeshInstanceBinding{EMeshInstanceBinding::TRANSFORMS}
        ));
        context.require(StorageArgument::make(
            context.resource("alive").resource,
            MeshInstanceBinding{EMeshInstanceBinding::ALIVE}
        ));
        context.require(StorageArgument::make(
            context.resource("properties").resource,
            MeshInstanceBinding{EMeshInstanceBinding::PROPERTIES}
        ));
        detail::MeshDrawGroupsConfiguration groups;
        groups.instances = context.resource("transforms").resource;
        groups.views = views.resource;
        for (std::size_t i = 0; i < group_ports.size(); ++i)
        {
            groups.outputs[i] = context.resource(group_ports[i]).resource;
        }
        auto description = ConfigValue::make(groups);
        if (!description)
        {
            return cxx::unexpected(description.error());
        }
        detail::FeatureBuildAccess::bind(context,
            {{groups.outputs.begin(), groups.outputs.end()},
             std::move(*description),
             &detail::MeshDrawGroups::create,
             {views.resource}}
        );
        const auto table = context.resource("groups");
        const auto dispatch = context.resource("dispatch");
        auto visible = context.resource("visible");
        auto reset_program = computeProgram(context, GraphProgram{kMeshReset});
        if (!reset_program)
        {
            return cxx::unexpected(reset_program.error());
        }
        ComputeWork reset;
        reset.interface.program = *reset_program;
        reset.interface.resources = {{"groups", {table}}, {"visible", {visible, {}, EGraphAccess::WRITE}}};
        const std::array<std::uint32_t, 2> parameters{config.frustum_culling, config.shadow_pass};
        const auto bytes = std::as_bytes(std::span(parameters));
        reset.dispatch = IndirectDispatchArguments{dispatch, 12};
        reset.allow_async_compute = true;
        auto cleared = context.graph.addComputePass(
            featureKey(detail::MeshVisibilityFeature::name, key, "reset"),
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
            {"properties", {context.resource("properties")}},
            {"views", {views}},
            {"candidates", {context.resource("candidates")}},
            {"groups", {table}},
            {"chunks", {context.resource("chunks")}},
            {"visible", {visible, {}, EGraphAccess::READ_WRITE}}
        };
        cull.interface.parameters.values.assign(bytes.begin(), bytes.end());
        cull.dispatch = IndirectDispatchArguments{dispatch, 0};
        cull.allow_async_compute = true;
        auto culled = context.graph.addComputePass(
            featureKey(detail::MeshVisibilityFeature::name, key, "frustum"),
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
            {"groups", {table}},
            {"visible", {visible}},
            {"commands", {context.resource("commands"), {}, EGraphAccess::WRITE}}
        };
        compact.interface.parameters.values.resize(sizeof(std::uint32_t));
        compact.interface.parameters.device_fields = {{"shared_members", EDeviceParameter::INDIRECT_FIRST_INSTANCE}};
        compact.dispatch = IndirectDispatchArguments{dispatch, 12};
        compact.allow_async_compute = true;
        auto completed = context.graph.addComputePass(
            featureKey(detail::MeshVisibilityFeature::name, key, "compact"),
            std::move(compact)
        );
        if (!completed)
        {
            return cxx::unexpected(completed.error());
        }
        if (auto published = context.publish("visible", visible); !published)
        {
            return published;
        }
        if (auto published = context.publish("commands", completed->written[0]); !published)
        {
            return published;
        }
        return {};
    }

    RenderResult<std::size_t>
    ConfigValueTraits<MeshVisibilityFeatureConfig>::retainedBytes(const MeshVisibilityFeatureConfig& value) noexcept
    {
        return sizeof(value) + value.instance_storage.capacity() + 1 +
               (value.view_parameters ? detail::parameterLayoutBytes(*value.view_parameters) : 0);
    }

    template RenderResult<void>
    detail::MeshVisibilityFeature::build<false>(const Config*, std::string_view, FeatureBuildContext&) noexcept;

    template RenderResult<void>
    detail::MeshVisibilityFeature::build<true>(const Config*, std::string_view, FeatureBuildContext&) noexcept;

} // namespace lux::render
