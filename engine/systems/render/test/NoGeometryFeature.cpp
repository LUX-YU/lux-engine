// Extension-author composition TU. The ordinary Scene consumer does not include author interfaces.
#include <lux/engine/render/capabilities/CapabilityCatalog.hpp>
#include <lux/engine/render/runtime/commands/Registration.hpp>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>

namespace
{
    using namespace lux;
    using namespace render;

    template <bool Alternate> struct NoGeometryFeature final
    {
        static constexpr FeatureName name{Alternate ? "test.scene.no_geometry.b" : "test.scene.no_geometry.a"};
        using Config = void;

        static const FeatureGraphOps& graph(bool) noexcept
        {
            static const FeatureGraphOps operations{
                [](const void*, std::string_view, FeaturePorts& ports) noexcept -> RenderResult<void>
                {
                    GraphImageDesc color{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM};
                    color.extent.source = EImageExtentSource::TARGET;
                    ports.resources.push_back({"color", EFeaturePort::OUTPUT, color});
                    return {};
                },
                [](
                    const void*, std::string_view, FeatureBuildContext& context,
                    const lux::render::detail::EnabledDeviceFeatures&
                ) noexcept -> RenderResult<void>
                {
                    auto pass = context.graph.addTransferPass(
                        "clear",
                        TransferWork{
                            {ClearImage{{context.resource("color")}, {0.2F, Alternate ? 0.4F : 0.3F, 0.1F, 1.F}}}
                        }
                    );
                    if (!pass)
                    {
                        return cxx::unexpected(pass.error());
                    }
                    return context.publish("color", pass->written.front());
                }
            };
            return operations;
        }
    };

    template <bool Surface> struct NoGeometryCapability final
    {
        using PrerequisiteCapabilities = std::tuple<>;
        using PerFrameData = std::tuple<>;
    };

    template <bool Alternate, bool Surface> struct NoGeometryImplementation final
    {
        using Capability = NoGeometryCapability<Surface>;
        using Config = void;
        using Features = std::tuple<NoGeometryFeature<Alternate>>;
        using FrameBindings = std::tuple<>;
        using Storages = std::tuple<>;
        using Bindings = std::tuple<>;

        static CapabilityImplementation implementation() noexcept
        {
            CapabilityImplementation value;
            value.implementation = NoGeometryFeature<Alternate>::name.name();
            auto& capability = value.graph;
            capability.ports = {
                {"color",
                 ECapabilityPortDirection::OUTPUT,
                 "test.color",
                 CapabilityImageShape{rdesc::ETextureFormat::RGBA8_UNORM, 1, 1},
                 "viewport"}
            };
            capability.target_formats = {rdesc::ETextureFormat::RGBA8_UNORM};
            capability.target_samples = {1};
            capability.target_kinds = {Surface ? ETargetKind::SURFACE : ETargetKind::OFFSCREEN};
            return value;
        }
    };
} // namespace

lux::render::RenderResult<void> registerNoGeometryCapabilities(void*, lux::render::RenderServer& server) noexcept
{
    auto first = server.registerCapabilityImplementation<NoGeometryImplementation<false, false>>();
    if (!first)
    {
        return lux::cxx::unexpected(first.error());
    }
    auto second = server.registerCapabilityImplementation<NoGeometryImplementation<true, false>>();
    if (!second)
    {
        return lux::cxx::unexpected(second.error());
    }
    auto surface = server.registerCapabilityImplementation<NoGeometryImplementation<false, true>>();
    if (!surface)
    {
        return lux::cxx::unexpected(surface.error());
    }
    auto alternate_surface = server.registerCapabilityImplementation<NoGeometryImplementation<true, true>>();
    return alternate_surface ? lux::render::RenderResult<void>{} : lux::cxx::unexpected(alternate_surface.error());
}

lux::cxx::type_info noGeometryCapabilityType(bool surface) noexcept
{
    return surface ? lux::cxx::make_basic_type_info<NoGeometryCapability<true>>()
                   : lux::cxx::make_basic_type_info<NoGeometryCapability<false>>();
}
