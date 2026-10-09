#include <lux/engine/flowforge/FlowNodePayload.hpp>

#include "node_payload_plugin_state.hpp"

#include <cstdlib>
#include <string>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    struct ExternalPayload final
    {
        FlowPayloadPluginState* state;
        std::string semantic_value;

        ExternalPayload(FlowPayloadPluginState& observed, std::string value) noexcept
            : state(&observed), semantic_value(std::move(value))
        {
            ++state->constructed;
        }

        ~ExternalPayload()
        {
            if (state->unloaded)
            {
                std::abort();
            }
            ++state->destroyed;
        }
    };

    FlowForgeResult<std::unique_ptr<ExternalPayload>> clone(const ExternalPayload& original) noexcept
    {
        ++original.state->clone_calls;
        if (original.state->reject_clone)
        {
            return cxx::unexpected(FlowForgeFailure{
                EFlowForgeError::INVALID_DESCRIPTION,
                "DLL clone rejected: " + original.semantic_value,
                11,
                19
            });
        }
        return std::make_unique<ExternalPayload>(*original.state, original.semantic_value);
    }
} // namespace

#if defined(_WIN32)
#define FLOW_TEST_EXPORT __declspec(dllexport)
#else
#define FLOW_TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" FLOW_TEST_EXPORT void createFlowPayload(
    lux::flowforge::FlowNodePayload& output,
    const lux::object::CodeLease& code,
    FlowPayloadPluginState& state
) noexcept
{
    auto value = FlowNodePayload::make<ExternalPayload, &clone>(code, state, std::string{"external semantic data"});
    if (!value)
    {
        std::abort();
    }
    output = std::move(*value);
}
