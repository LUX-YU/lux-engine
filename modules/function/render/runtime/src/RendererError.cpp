#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>
#include <lux/engine/render/RendererConfig.hpp>

namespace lux::render
{
    error::Error toError(const RendererFailure& failure) noexcept
    {
        if (!failure.render_error.ok())
        {
            return toError(failure.render_error);
        }
        using enum error::ERecovery;
        struct Definition
        {
            std::string_view name;
            error::ERecovery recovery;
        };
        constexpr Definition definitions[]{
            {"invalid_argument", NEEDS_INPUT},
            {"wrong_thread", BUG},
            {"not_ready", RETRYABLE},
            {"busy", RETRYABLE},
            {"stopping", PERMANENT},
            {"capacity", RETRYABLE},
            {"device_failure", PERMANENT},
            {"stale_view", PERMANENT},
            {"stale_image", PERMANENT},
            {"incomplete_frame_references", BUG},
            {"external_failure", PERMANENT},
            {"contract_failure", BUG}
        };
        static_assert(std::size(definitions) == static_cast<std::size_t>(ERendererError::CONTRACT_FAILURE) + 1);
        const auto index = static_cast<std::size_t>(failure.code);
        if (index >= std::size(definitions))
        {
            return error::makeError(
                {"lux.render.unknown_code", "Unknown Renderer code {0}", BUG, {error::EArgument::UNSIGNED}},
                {index}
            );
        }
        const std::string name = "lux.render.runtime." + std::string(definitions[index].name);
        return error::makeError(
            {name,
             "Render request {0}; backend status present {1}, value {2}",
             definitions[index].recovery,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {failure.request, failure.backend_status.has_value(), failure.backend_status.value_or(0)}
        );
    }
} // namespace lux::render
