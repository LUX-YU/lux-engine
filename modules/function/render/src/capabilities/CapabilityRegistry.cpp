#include <algorithm>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/capabilities/CapabilityRegistry.hpp>
#include <set>

namespace lux::render
{
    RenderResult<void> CapabilityRegistry::prepare(std::size_t implementations) noexcept
    {
        if (!implementations_.prepareInsert(implementations) || !capabilities_.prepareInsert(implementations))
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        return {};
    }

    RenderResult<void> CapabilityRegistry::validate(const CapabilityDescription& description) const noexcept
    {
        if (!description.type.isValid())
        {
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        }
        std::set<std::string_view> commands;
        for (const auto* command : description.commands)
        {
            const bool is_invalid_command = command == nullptr || !command->data.isValid() ||
                                            !command->capability_command || !validateCommand(*command);
            if (is_invalid_command || !commands.emplace(command->name.name()).second)
            {
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
            }
        }
        std::set<TypeKey> types;
        types.emplace(description.type.hash(), description.type.name());
        for (auto prerequisite : description.prerequisites)
        {
            if (!prerequisite.isValid() || !types.emplace(prerequisite.hash(), prerequisite.name()).second)
            {
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
            }
        }
        std::set<std::string_view> resource_types;
        for (const auto* resource : description.resources)
        {
            const bool is_invalid_resource = !resource || !validateStorage(*resource);
            if (is_invalid_resource) return cxx::unexpected(RenderError{kCapabilityDeclaration});
            if (!resource_types.emplace(resource->type.name()).second)
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
            for (const auto* command : resource->commands)
            {
                if (commands.contains(command->name.name()))
                    return cxx::unexpected(RenderError{kCapabilityDeclaration});
            }
        }
        // Equality is type identity only. External registration must still preserve the
        // already published static contract; this cold check never runs during dispatch.
        if (const auto* existing = get(find(description.type)))
        {
            const bool changed_resources = !std::ranges::equal(
                existing->resources, description.resources,
                [](const auto* left, const auto* right) noexcept { return left->hasSameContract(*right); }
            );
            const bool changed_prerequisites = !std::ranges::equal(existing->prerequisites, description.prerequisites);
            const bool changed_commands = !std::ranges::equal(
                existing->commands,
                description.commands,
                [](const auto* left, const auto* right) noexcept
                { return compatibleCommands(*left, *right).has_value(); }
            );
            if (changed_resources || changed_prerequisites || changed_commands)
            {
                return cxx::unexpected(RenderError{kCapabilityConflict});
            }
        }
        return {};
    }

    CapabilityImplementationId
    CapabilityRegistry::publish(CapabilityDescription declaration, CapabilityImplementation implementation) noexcept
    {
        auto id = find(declaration.type);
        if (id.isNull())
        {
            id = capabilities_.insert(CapabilityRecord{declaration, {}});
            capabilities_by_type_.emplace(TypeKey{declaration.type.hash(), declaration.type.name()}, id);
        }
        const auto inserted = implementations_.insert(std::move(implementation));
        capabilities_.tryGet(id)->implementations.push_back(inserted);
        return inserted;
    }

    RenderResult<CapabilityId> CapabilityRegistry::add(const CapabilityDescription& description) noexcept
    {
        if (auto valid = validate(description); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (auto existing = find(description.type); !existing.isNull())
        {
            return existing;
        }
        if (!capabilities_.prepareInsert(1))
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        const auto id = capabilities_.insert(CapabilityRecord{description, {}});
        capabilities_by_type_.emplace(TypeKey{description.type.hash(), description.type.name()}, id);
        return id;
    }

    RenderResult<CapabilityImplementationId>
    CapabilityRegistry::addImplementation(CapabilityId capability, const CapabilityImplementation& declaration) noexcept
    {
        if (!get(capability))
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        for (auto id : implementations(capability))
        {
            const auto& previous = *find(id);
            if (previous == declaration)
            {
                return id;
            }
            if (previous.implementation == declaration.implementation)
            {
                return cxx::unexpected(RenderError{kCapabilityConflict});
            }
        }
        // The supplied declaration may borrow an existing directory entry. Copy before
        // reserve/erase can move or destroy that entry, then publish the owned candidate.
        auto candidate = declaration;
        if (auto prepared = prepare(1); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        return publish(*get(capability), std::move(candidate));
    }

    CapabilityId CapabilityRegistry::find(cxx::type_info type) const noexcept
    {
        const auto found = capabilities_by_type_.find({type.hash(), type.name()});
        return found == capabilities_by_type_.end() ? CapabilityId{} : found->second;
    }

    const CapabilityDescription* CapabilityRegistry::get(CapabilityId id) const noexcept
    {
        const auto* record = capabilities_.tryGet(id);
        return record ? &record->description : nullptr;
    }

    const CapabilityImplementation* CapabilityRegistry::find(CapabilityImplementationId id) const noexcept
    {
        return implementations_.tryGet(id);
    }

    std::span<const CapabilityImplementationId>
    CapabilityRegistry::implementations(CapabilityId capability) const noexcept
    {
        const auto* record = capabilities_.tryGet(capability);
        return record ? std::span<const CapabilityImplementationId>(record->implementations)
                      : std::span<const CapabilityImplementationId>{};
    }

    CapabilityId CapabilityRegistry::capabilityOf(CapabilityImplementationId implementation) const noexcept
    {
        for (const auto id : capabilities_.keys())
        {
            const auto& members = capabilities_.tryGet(id)->implementations;
            if (std::ranges::find(members, implementation) != members.end())
            {
                return id;
            }
        }
        return {};
    }

    RenderResult<CapabilityImplementationId> CapabilityRegistry::replaceImplementation(
        CapabilityImplementationId previous,
        const CapabilityImplementation& declaration
    ) noexcept
    {
        const auto capability = capabilityOf(previous);
        if (capability.isNull())
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        for (const auto id : implementations(capability))
        {
            if (id != previous && find(id)->implementation == declaration.implementation)
            {
                return cxx::unexpected(RenderError{kCapabilityConflict});
            }
        }
        // The supplied declaration may borrow an existing directory entry. Copy before
        // reserve/erase can move or destroy that entry, then publish the owned candidate.
        auto candidate = declaration;
        if (auto prepared = prepare(1); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        auto& members = capabilities_.tryGet(capability)->implementations;
        members.reserve(members.size() + 1);
        std::erase(members, previous);
        implementations_.erase(previous);
        // Keep membership in the same capability; only the implementation generation changes.
        return publish(*get(capability), std::move(candidate));
    }

    RenderResult<void> CapabilityRegistry::removeImplementation(CapabilityImplementationId id) noexcept
    {
        if (!implementations_.contains(id))
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        const auto capability = capabilityOf(id);
        if (auto* record = capabilities_.tryGet(capability))
        {
            std::erase(record->implementations, id);
        }
        implementations_.erase(id);
        return {};
    }

    RenderResult<void> CapabilityRegistry::remove(CapabilityId id) noexcept
    {
        const auto* description = get(id);
        if (!description)
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        if (!implementations(id).empty())
        {
            return cxx::unexpected(RenderError{kCapabilityInUse});
        }
        for (const auto& dependent : capabilities_.values())
        {
            for (auto prerequisite : dependent.description.prerequisites)
            {
                if (prerequisite == description->type && prerequisite.name() == description->type.name())
                {
                    return cxx::unexpected(RenderError{kCapabilityInUse});
                }
            }
        }
        capabilities_by_type_.erase(TypeKey{description->type.hash(), description->type.name()});
        capabilities_.erase(id);
        return {};
    }

    std::span<const CapabilityImplementationId> CapabilityRegistry::implementations() const noexcept
    {
        return implementations_.keys();
    }

    RenderResult<void> CapabilityRegistry::validateDeclarations() const noexcept
    {
        const auto& declarations = capabilities_.values();
        std::map<TypeKey, std::size_t> indices;
        for (std::size_t i = 0; i < declarations.size(); ++i)
        {
            const auto& declaration = declarations[i].description;
            if (auto valid = validate(declaration); !valid)
            {
                return valid;
            }
            indices.emplace(TypeKey{declaration.type.hash(), declaration.type.name()}, i);
        }
        std::vector<std::size_t> pending(declarations.size());
        std::vector<std::vector<std::size_t>> dependents(declarations.size());
        std::vector<std::size_t> ready;
        ready.reserve(declarations.size());
        for (std::size_t i = 0; i < declarations.size(); ++i)
        {
            for (auto prerequisite : declarations[i].description.prerequisites)
            {
                const auto found = indices.find({prerequisite.hash(), prerequisite.name()});
                if (found == indices.end())
                {
                    return cxx::unexpected(RenderError{kCapabilityPrerequisite, {i}});
                }
                dependents[found->second].push_back(i);
                ++pending[i];
            }
            if (pending[i] == 0)
            {
                ready.push_back(i);
            }
        }
        for (std::size_t consumed = 0; consumed < ready.size(); ++consumed)
        {
            for (auto dependent : dependents[ready[consumed]])
            {
                if (--pending[dependent] == 0)
                {
                    ready.push_back(dependent);
                }
            }
        }
        if (ready.size() != declarations.size())
        {
            return cxx::unexpected(RenderError{kCapabilityCycle});
        }
        return {};
    }

} // namespace lux::render
