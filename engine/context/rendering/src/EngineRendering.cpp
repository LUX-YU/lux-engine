#include <lux/engine/EngineContext.hpp>
#include <lux/engine/EngineRendering.hpp>

namespace lux::engine
{
    namespace detail
    {
        struct EngineRenderAccess final
        {
            static void adopt(EngineContext& engine, std::unique_ptr<RenderContext> context) noexcept
            {
                engine.rendering_ =
                    EngineContext::RenderOwner{context.release(), [](RenderContext* value) noexcept { delete value; }};
            }
        };
    } // namespace detail

    RenderContext::Result initializeRendering(
        EngineContext& engine,
        std::span<const char* const> instance_extensions,
        render::RendererConfig configuration
    ) noexcept
    {
        if (engine.renderContext())
        {
            return lux::cxx::unexpected(
                RenderContext::VFailure{render::RendererFailure{render::ERendererError::INVALID_ARGUMENT}}
            );
        }
        for (const auto* extension : instance_extensions)
        {
            if (!extension)
            {
                return lux::cxx::unexpected(
                    RenderContext::VFailure{render::RendererFailure{render::ERendererError::INVALID_ARGUMENT}}
                );
            }
            configuration.instance_extensions.emplace_back(extension);
        }
        auto context = RenderContext::create(engine.execution(), std::move(configuration));
        if (!context)
        {
            return lux::cxx::unexpected(context.error());
        }
        detail::EngineRenderAccess::adopt(engine, std::move(*context));
        return {};
    }
} // namespace lux::engine
