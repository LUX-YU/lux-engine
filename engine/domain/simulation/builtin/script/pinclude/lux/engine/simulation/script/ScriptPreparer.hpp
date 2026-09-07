#pragma once

#include <lux/engine/simulation/script/ScriptInstances.hpp>
#include <array>

namespace lux::simulation::script::detail
{
    class ScriptPreparer final
    {
    public:
        using Result = lux::cxx::expected<void, EScriptSystemError>;
        ScriptPreparer() = default;
        ScriptPreparer(const ScriptPreparer&) = delete;
        ScriptPreparer& operator=(const ScriptPreparer&) = delete;
        ScriptPreparer(ScriptPreparer&&) = delete;
        ScriptPreparer& operator=(ScriptPreparer&&) = delete;

        [[nodiscard]] Result prepareCatalog(
            ScriptArtifactResolver artifacts,
            std::span<const ScriptBackendDescriptor> backends,
            std::span<const ScriptApiCapabilityPublication> capabilities,
            const ScriptApiCapabilityPublication& delay
        ) noexcept;
        [[nodiscard]] Result prepareMount(
            ScriptInstances& instances,
            std::uint32_t slot,
            const ScriptBindings& bindings,
            const SimulationDescription& simulation,
            const ScriptRuntimeLimits& limits
        ) noexcept;
        void releaseCatalog() noexcept;

    private:
        [[nodiscard]] const ScriptBackendDescriptor* backend(lux::rdesc::Script::Kind kind) const noexcept;
        [[nodiscard]] const PreparedScriptApiCapability*
        capability(const lux::script::ScriptApiContractId&) const noexcept;
        [[nodiscard]] static bool validBeginPlay(const lux::rdesc::ScriptFunction& function) noexcept;
        [[nodiscard]] static bool validEndPlay(const lux::rdesc::ScriptFunction& function) noexcept;
        [[nodiscard]] static bool eventMatches(const lux::script::ScriptEventSourceDescription& requirement,
            const SimulationEventView& described, const ScriptEventEndpointDescriptor& endpoint) noexcept;

        ScriptArtifactResolver artifacts_;
        std::array<ScriptBackendDescriptor, 7U> backends_{};
        std::vector<PreparedScriptApiCapability> capabilities_;
    };
}
