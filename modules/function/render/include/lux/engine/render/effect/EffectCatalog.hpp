#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/effect/CapabilityDefinition.hpp>
#include <memory>
#include <span>
#include <vector>

namespace lux::render
{
    struct EffectImplementation;

    // UI data. No shader, build callback, resource factory or command-handler address.
    struct ImplementationOption final
    {
        ImplementationSelection identity;
        std::string display_name;
    };

    // Owner-thread assembly, then seal and publish as shared_ptr<const EffectCatalog>.
    // There is no registration/mutation on the worker or frame hot path.
    class LUX_RENDER_PUBLIC EffectCatalog final
    {
    public:
        EffectCatalog() noexcept = default;
        EffectCatalog(const EffectCatalog&) = delete;
        EffectCatalog& operator=(const EffectCatalog&) = delete;
        EffectCatalog(EffectCatalog&&) noexcept = default;
        EffectCatalog& operator=(EffectCatalog&&) noexcept = default;

        [[nodiscard]] RenderResult<void> addCapability(std::unique_ptr<CapabilityDefinition> definition) noexcept;
        [[nodiscard]] RenderResult<void>
        addImplementation(std::unique_ptr<EffectImplementation> implementation) noexcept;
        void seal() noexcept { sealed_ = true; }
        [[nodiscard]] bool sealed() const noexcept { return sealed_; }

        [[nodiscard]] std::span<const std::shared_ptr<const CapabilityDefinition>> listCapabilities() const & noexcept
        {
            return capabilities_;
        }

        std::span<const std::shared_ptr<const CapabilityDefinition>> listCapabilities() const && = delete;

        [[nodiscard]] std::vector<ImplementationOption>
        listImplementations(graph::NodeTypeId capability, std::uint32_t version) const noexcept;

        // Cold compilation lookup. Ordinary graph/editor users only need list APIs.
        [[nodiscard]] std::shared_ptr<const CapabilityDefinition>
        findCapability(graph::NodeTypeId capability, std::uint32_t version) const noexcept;
        [[nodiscard]] std::shared_ptr<const EffectImplementation> findImplementation(
            graph::NodeTypeId capability,
            std::uint32_t capability_version,
            const ImplementationSelection& selection
        ) const noexcept;

    private:
        std::vector<std::shared_ptr<const CapabilityDefinition>> capabilities_;
        std::vector<std::shared_ptr<const EffectImplementation>> implementations_;
        bool sealed_ = false;
    };
}
