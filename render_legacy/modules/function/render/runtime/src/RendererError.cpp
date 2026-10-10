#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>
#include <lux/engine/render/RendererConfig.hpp>

namespace lux::render
{
    namespace
    {
        constexpr error::ErrorDescriptor ErrorDescriptors[]{
            {"lux.render.runtime.invalid_argument",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::NEEDS_INPUT,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.runtime.wrong_thread",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::BUG,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.runtime.not_ready",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::RETRYABLE,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.runtime.busy",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::RETRYABLE,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.runtime.stopping",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.runtime.capacity",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::RETRYABLE,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.runtime.device_failure",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.runtime.stale_view",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.runtime.stale_image",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.runtime.incomplete_frame_references",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::BUG,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.runtime.external_failure",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::PERMANENT,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.runtime.contract_failure",
             "Render request {0}; backend status present {1}, value {2}",
             error::ERecovery::BUG,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.render.unknown_code", "Unknown Renderer code {0}", error::ERecovery::BUG, {error::EArgument::UNSIGNED}
            }
        };
        constexpr auto ErrorDescriptorsIds = []
        {
            std::array<error::ErrorId, std::size(ErrorDescriptors)> result{};
            for (std::size_t i{}; i < result.size(); ++i)
            {
                result[i] = error::errorId(ErrorDescriptors[i].name);
            }
            return result;
        }();
    } // namespace
    cxx::expected<void, error::Error> registerRendererErrors() noexcept
    {
        // Built-in and dynamically admitted backend definitions share the same conversion boundary.
        (void)renderErrorRegistry();
        return error::ErrorRegistry::instance().registerTypes(ErrorDescriptors);
    }
    error::Error toError(const RendererFailure& failure) noexcept
    {
        if (!failure.render_error.ok())
        {
            return toError(failure.render_error);
        }
        constexpr auto count = static_cast<std::size_t>(ERendererError::CONTRACT_FAILURE) + 1;
        static_assert(std::size(ErrorDescriptors) == count + 1);
        const auto index = static_cast<std::size_t>(failure.code);
        if (index >= count)
        {
            return {ErrorDescriptorsIds[count], {index}};
        }
        return {
            ErrorDescriptorsIds[index],
            {failure.request, failure.backend_status.has_value(), failure.backend_status.value_or(0)}
        };
    }
} // namespace lux::render
