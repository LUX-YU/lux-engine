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

        // Qualify a new capability declaration once at registration.
        [[nodiscard]] RenderResult<void> validate(const CapabilityDescription& description) const noexcept;

        // Called only after qualification and successful prepare(). Publishing invokes no user code.
        CapabilityImplementationId
        publish(CapabilityId capability, CapabilityImplementation implementation) noexcept;

        [[nodiscard]] RenderResult<CapabilityId>
        add(const CapabilityDescription& description, std::vector<ResourceTypeId> resources) noexcept;

        [[nodiscard]] std::span<const ResourceTypeId> resources(CapabilityId) const noexcept;

        [[nodiscard]] RenderResult<CapabilityImplementationId>
        addImplementation(CapabilityId capability, const CapabilityImplementation& declaration) noexcept;

        [[nodiscard]] const CapabilityImplementation* find(CapabilityImplementationId id) const noexcept;

        [[nodiscard]] CapabilityId find(cxx::basic_type_info type) const noexcept;

        [[nodiscard]] const CapabilityDescription* get(CapabilityId id) const noexcept;

        [[nodiscard]] std::span<const CapabilityImplementationId> implementations() const noexcept;

        [[nodiscard]] std::span<const CapabilityImplementationId>
        implementations(CapabilityId capability) const noexcept;

        // Parent membership is part of the registered record, not a reverse directory scan.
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
            std::vector<ResourceTypeId> resources;
            std::vector<CapabilityImplementationId> implementations;
        };

        struct ImplementationRecord final
        {
            CapabilityId capability;
            CapabilityImplementation declaration;
        };

        cxx::SlotKeyAutoSparseSet<CapabilityId, CapabilityRecord> capabilities_;
        cxx::SlotKeyAutoSparseSet<CapabilityImplementationId, ImplementationRecord> implementations_;
        std::map<TypeKey, CapabilityId> capabilities_by_type_;
    };
} // namespace lux::render
