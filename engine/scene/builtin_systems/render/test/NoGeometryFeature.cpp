// Extension-author composition TU. The ordinary Scene consumer does not include author interfaces.
#include <lux/engine/render/capabilities/RenderCapability.hpp>
#include <lux/engine/render/extensions/FeatureRegistration.hpp>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>

namespace
{
    using namespace lux;
    using namespace render;

    template <bool Alternate> struct NoGeometryFeature final
    {
        static constexpr FeatureName name{Alternate ? "test.scene.no_geometry.b" : "test.scene.no_geometry.a"};
        using Config = void;
        using Storages = std::tuple<>;
        using Commands = std::tuple<>;

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
                [](const void*, std::string_view, FeatureBuildContext& context) noexcept -> RenderResult<void>
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

        static const CapabilitySupport& capabilities() noexcept
        {
            static const CapabilitySupport support = []
            {
                CapabilitySupport value;
                value.implementation = name.name();
                value.configuration.use = ERenderPayloadUse::CONFIGURATION;
                RenderCapability capability;
                capability.name = "test.scene.no_geometry";
                capability.outputs = {"color"};
                capability.target_formats = {rdesc::ETextureFormat::RGBA8_UNORM};
                capability.target_samples = {1};
                capability.target_kinds = {ETargetKind::OFFSCREEN};
                value.capabilities.push_back(std::move(capability));
                return value;
            }();
            return support;
        }
    };
} // namespace

lux::render::RenderResult<void> registerNoGeometryCapabilities(void*, lux::render::RenderServer& server) noexcept
{
    auto first = server.registerFeature<NoGeometryFeature<false>>();
    if (!first)
    {
        return lux::cxx::unexpected(first.error());
    }
    auto second = server.registerFeature<NoGeometryFeature<true>>();
    return second ? lux::render::RenderResult<void>{} : lux::cxx::unexpected(second.error());
}
