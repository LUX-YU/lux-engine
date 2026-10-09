#include <lux/engine/material/MaterialNodeCatalog.hpp>

#include "node_plugin_state.hpp"

#include <cstdlib>

namespace
{
    using namespace lux;
    using namespace lux::material;

    NodePluginState* plugin_state{};

    struct ExternalPayload final
    {
        ExternalPayload() noexcept
        {
            ++plugin_state->constructed;
        }

        ~ExternalPayload()
        {
            if (plugin_state->unloaded)
            {
                std::abort();
            }
            ++plugin_state->destroyed;
        }
    };

    MaterialNodeResult<std::unique_ptr<ExternalPayload>> clone(const ExternalPayload&) noexcept
    {
        ++plugin_state->clone_returns;
        if (plugin_state->reject_clone)
        {
            return cxx::unexpected(MaterialCompileFailure{EMaterialCompileError::INVALID_RESULT, "DLL clone rejected"});
        }
        return std::make_unique<ExternalPayload>();
    }
} // namespace

#if defined(_WIN32)
#define MATERIAL_TEST_EXPORT __declspec(dllexport)
#else
#define MATERIAL_TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" MATERIAL_TEST_EXPORT void materialNodeRegistration(
    lux::material::MaterialNodeRegistration& result,
    const lux::object::CodeLease& code,
    NodePluginState& state
) noexcept
{
    using namespace lux;
    using namespace lux::material;
    plugin_state = &state;
    result.identity = {graph::nodeTypeId("external.material.constant.v1"), "external.material.constant.v1", 1};
    result.payload_type = cxx::typeToken<ExternalPayload>();
    result.code = code;
    result.create = [](const object::CodeLease& code) noexcept
    { return MaterialNodePayload::make<ExternalPayload, &clone>(code); };
    result.validate = [](const MaterialNodePayload& payload) noexcept -> MaterialNodeResult<void>
    {
        if (!payload.get<ExternalPayload>())
        {
            return cxx::unexpected(MaterialCompileFailure{EMaterialCompileError::INVALID_GRAPH, "wrong DLL payload"});
        }
        return {};
    };
    result.describe_pins =
        [](const MaterialNodePayload&) noexcept -> MaterialNodeResult<std::vector<MaterialPinDeclaration>>
    { return std::vector<MaterialPinDeclaration>{{graph::PinSemanticId{1}, "value", graph::EPinDirection::OUTPUT}}; };
    result.compile = [](const MaterialNodePayload&, std::span<const std::uint32_t>, shadergen::ShaderIR& candidate
                     ) noexcept -> MaterialNodeResult<std::vector<std::uint32_t>>
    {
        const auto index = static_cast<std::uint32_t>(candidate.values.size());
        shadergen::ShaderIRValue value{shadergen::EOp::CONSTANT, shadergen::EValueType::FLOAT};
        value.constant[0] = 0.625F;
        candidate.values.push_back(value);
        return std::vector<std::uint32_t>{index};
    };
}
