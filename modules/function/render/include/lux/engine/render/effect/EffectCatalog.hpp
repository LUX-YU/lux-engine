#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/effect/EffectCapability.hpp>
#include <memory>
#include <ranges>
#include <span>
#include <vector>

namespace lux::render
{
    struct ImplementationEntry;
    namespace detail { class CapabilityMetadata; }

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

        [[nodiscard]] RenderResult<void>
        addCapability(const EffectCapability& definition) noexcept;

        [[nodiscard]] RenderResult<void>
        addImplementation(std::unique_ptr<ImplementationEntry> implementation) noexcept;

        void seal() noexcept { sealed_ = true; }

        [[nodiscard]] bool sealed() const noexcept { return sealed_; }

        [[nodiscard]] auto listCapabilities() const & noexcept
        {
            return std::views::transform(capabilities_, [](const auto& definition) noexcept -> const EffectCapability&
            {
                return descriptor(*definition);
            });
        }

        void listCapabilities() const && = delete;

        [[nodiscard]] std::vector<ImplementationOption>
        listImplementations(graph::NodeTypeId capability, std::uint32_t version) const noexcept;

        // Cold compilation lookup. Ordinary graph/editor users only need list APIs.
        [[nodiscard]] const EffectCapability*
        findCapability(graph::NodeTypeId capability, std::uint32_t version) const & noexcept;

        const EffectCapability*
        findCapability(graph::NodeTypeId, std::uint32_t) const && = delete;

        [[nodiscard]] const ImplementationEntry* findImplementation(
            graph::NodeTypeId capability,
            std::uint32_t capability_version,
            const ImplementationSelection& selection
        ) const & noexcept;

        const ImplementationEntry*
        findImplementation(graph::NodeTypeId, std::uint32_t, const ImplementationSelection&) const && = delete;

    private:
        static const EffectCapability& descriptor(const detail::CapabilityMetadata&) noexcept;
        std::vector<std::unique_ptr<const detail::CapabilityMetadata>> capabilities_;
        std::vector<std::unique_ptr<const ImplementationEntry>> implementations_;
        bool sealed_ = false;
    };
}
