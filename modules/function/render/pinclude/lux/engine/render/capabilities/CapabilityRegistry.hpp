#pragma once

#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/extensions/CapabilityImplementation.hpp>
#include <map>

namespace lux::render
{
    // RenderThread's capability directory. Register at a control safe point, before dispatch resumes.
    // Declarations and their callback code must outlive the Server; this class owns no plugin or GPU object.
    class CapabilityRegistry final
    {
    public:
        // Reserve the entire publication before modifying any registration directory.
        [[nodiscard]] RenderResult<void> prepare(std::size_t implementations) noexcept;

        // Compare immutable declarations before publishing an external implementation.
        [[nodiscard]] RenderResult<void> validate(const CapabilityDescription& description) const noexcept;

        // Called only after qualification and successful prepare(). Publishing invokes no user code.
        CapabilityImplementationId
        publish(CapabilityDescription capability, CapabilityImplementation implementation) noexcept;

        template <CapabilityConcept C> [[nodiscard]] RenderResult<CapabilityId> add() noexcept
        {
            return add(describeCapability<C>());
        }

        [[nodiscard]] RenderResult<CapabilityId> add(const CapabilityDescription& description) noexcept;

        [[nodiscard]] RenderResult<CapabilityImplementationId>
        addImplementation(CapabilityId capability, const CapabilityImplementation& declaration) noexcept;

        [[nodiscard]] const CapabilityImplementation* find(CapabilityImplementationId id) const noexcept;

        [[nodiscard]] CapabilityId find(cxx::type_info type) const noexcept;

        [[nodiscard]] const CapabilityDescription* get(CapabilityId id) const noexcept;

        [[nodiscard]] std::span<const CapabilityImplementationId> implementations() const noexcept;

        [[nodiscard]] std::span<const CapabilityImplementationId>
        implementations(CapabilityId capability) const noexcept;

        // Cold reverse lookup over the directory's sole capability membership lists.
        [[nodiscard]] CapabilityId capabilityOf(CapabilityImplementationId implementation) const noexcept;

        // Catalog mutations only. RenderServer additionally rejects changes while execution is active.
        // Replacement returns a fresh generation; it never changes the meaning of an old input binding.
        [[nodiscard]] RenderResult<CapabilityImplementationId> replaceImplementation(
            CapabilityImplementationId previous,
            const CapabilityImplementation& declaration
        ) noexcept;

        [[nodiscard]] RenderResult<void> removeImplementation(CapabilityImplementationId id) noexcept;

        [[nodiscard]] RenderResult<void> remove(CapabilityId id) noexcept;

        // Cold declaration consistency; this does not choose implementations or order Graph passes.
        [[nodiscard]] RenderResult<void> validateDeclarations() const noexcept;

    private:
        using TypeKey = std::pair<std::size_t, std::string_view>;

        struct CapabilityRecord final
        {
            CapabilityDescription description;
            std::vector<CapabilityImplementationId> implementations;
        };

        cxx::SlotKeyAutoSparseSet<CapabilityId, CapabilityRecord> capabilities_;
        cxx::SlotKeyAutoSparseSet<CapabilityImplementationId, CapabilityImplementation> implementations_;
        std::map<TypeKey, CapabilityId> capabilities_by_type_;
    };
} // namespace lux::render
