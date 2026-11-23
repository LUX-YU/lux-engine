#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/effect/EffectNodeDefinition.hpp>
#include <memory>
#include <ranges>
#include <span>
#include <vector>

namespace lux::render
{
    struct EffectImplementation;

    // Owner-thread assembly followed by sealing. Cold lookups borrow immutable definitions
    // from this Catalog while the Server protects their lifetime.
    class LUX_RENDER_PUBLIC EffectCatalog final
    {
    public:
        EffectCatalog() noexcept;
        ~EffectCatalog() noexcept;
        EffectCatalog(const EffectCatalog&) = delete;
        EffectCatalog& operator=(const EffectCatalog&) = delete;
        EffectCatalog(EffectCatalog&&) noexcept;
        EffectCatalog& operator=(EffectCatalog&&) noexcept;

        [[nodiscard]] RenderResult<void> addCapability(std::unique_ptr<EffectNodeDefinition> definition) noexcept;
        [[nodiscard]] RenderResult<void>
        addImplementation(std::unique_ptr<EffectImplementation> implementation) noexcept;
        void seal() noexcept { sealed_ = true; }
        [[nodiscard]] bool sealed() const noexcept { return sealed_; }

        [[nodiscard]] auto listCapabilities() const & noexcept
        {
            return std::views::transform(capabilities_, [](const auto& definition) noexcept -> const EffectNodeDefinition&
            {
                return *definition;
            });
        }

        void listCapabilities() const && = delete;

        [[nodiscard]] std::vector<ImplementationOption>
        listImplementations(graph::NodeTypeId capability, std::uint32_t version) const noexcept;

        // Cold compilation lookup. Ordinary graph/editor users only need list APIs.
        [[nodiscard]] const EffectNodeDefinition*
        findCapability(graph::NodeTypeId capability, std::uint32_t version) const & noexcept;
        const EffectNodeDefinition* findCapability(graph::NodeTypeId, std::uint32_t) const && = delete;

        [[nodiscard]] const EffectImplementation* findImplementation(
            graph::NodeTypeId capability,
            std::uint32_t capability_version,
            const ImplementationSelection& selection
        ) const & noexcept;
        const EffectImplementation*
        findImplementation(graph::NodeTypeId, std::uint32_t, const ImplementationSelection&) const && = delete;

    private:
        std::vector<std::unique_ptr<const EffectNodeDefinition>> capabilities_;
        std::vector<std::unique_ptr<const EffectImplementation>> implementations_;
        bool sealed_ = false;
    };
}
