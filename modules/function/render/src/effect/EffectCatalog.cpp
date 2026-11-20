#include <lux/engine/render/effect/EffectCatalog.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <algorithm>
#include <tuple>
#include <utility>

namespace lux::render
{
    namespace
    {
        [[nodiscard]] bool validPorts(std::span<const EffectPortDefinition> ports) noexcept
        {
            std::vector<std::pair<graph::PinSemanticId, std::string_view>> identities;
            identities.reserve(ports.size());
            for (const auto& port : ports)
            {
                const bool is_invalid_identity = !port.semantic.valid() || port.name.empty() ||
                    port.semantic != effectPortSemantic(port.name);
                const bool is_invalid_type = port.type.canonical_name.empty() || port.type.version == 0;
                const bool is_invalid_direction = port.direction != graph::EPinDirection::INPUT &&
                    port.direction != graph::EPinDirection::OUTPUT;
                const bool is_invalid_cardinality = port.fan_cap == 0 ||
                    (port.required && port.direction != graph::EPinDirection::INPUT);
                if (is_invalid_identity || is_invalid_type || is_invalid_direction || is_invalid_cardinality)
                    return false;
                identities.emplace_back(port.semantic, port.name);
            }
            std::ranges::sort(identities);
            for (std::size_t index = 1; index < identities.size(); ++index)
            {
                // Equal hashes of different names are rejected, not treated as aliases.
                if (identities[index - 1].first == identities[index].first) return false;
            }
            return true;
        }

        [[nodiscard]] auto capabilityKey(const CapabilityDefinition& definition) noexcept
        {
            return std::pair{definition.identity.id.value, definition.identity.version};
        }

        [[nodiscard]] auto implementationKey(const EffectImplementation& implementation) noexcept
        {
            return std::tuple{
                implementation.capability.value, implementation.capability_version,
                std::string_view{implementation.identity.canonical_name}, implementation.identity.version
            };
        }
    }

    EffectCatalog::EffectCatalog() noexcept = default;

    EffectCatalog::~EffectCatalog() noexcept = default;

    EffectCatalog::EffectCatalog(EffectCatalog&&) noexcept = default;

    EffectCatalog& EffectCatalog::operator=(EffectCatalog&&) noexcept = default;

    RenderResult<void> EffectCatalog::addCapability(std::unique_ptr<CapabilityDefinition> definition) noexcept
    {
        if (sealed_ || !definition) return cxx::unexpected(RenderError{kCapabilityDeclaration});
        const auto& identity = definition->identity;
        const bool is_invalid_identity = !identity.id.valid() || identity.canonical_name.empty() ||
            identity.version == 0 || identity.id != graph::nodeTypeId(identity.canonical_name);
        if (is_invalid_identity || !validPorts(definition->ports))
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        for (const auto& existing : capabilities_)
        {
            if (existing->identity.id != identity.id) continue;
            const bool is_collision = existing->identity.canonical_name != identity.canonical_name;
            const bool is_duplicate_version = existing->identity.version == identity.version;
            if (is_collision || is_duplicate_version) return cxx::unexpected(RenderError{kCapabilityConflict});
        }
        auto commands = definition->commands;
        std::ranges::sort(commands, {}, [](CommandId command) noexcept { return commandTag(command); });
        if (std::adjacent_find(commands.begin(), commands.end()) != commands.end())
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        const auto key = capabilityKey(*definition);
        const auto position = std::ranges::lower_bound(capabilities_, key, {}, [](const auto& item) noexcept
        {
            return capabilityKey(*item);
        });
        capabilities_.insert(position, std::move(definition));
        return {};
    }

    RenderResult<void> EffectCatalog::addImplementation(
        std::unique_ptr<EffectImplementation> implementation
    ) noexcept
    {
        if (sealed_ || !implementation) return cxx::unexpected(RenderError{kCapabilityDeclaration});
        const auto capability = findCapability(implementation->capability, implementation->capability_version);
        const bool is_invalid_identity = implementation->identity.canonical_name.empty() ||
            implementation->identity.version == 0;
        const bool is_invalid_code = !implementation->initialize || !implementation->build;
        if (!capability || is_invalid_identity || is_invalid_code)
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        auto supported = implementation->supported_outputs;
        std::ranges::sort(supported);
        if (std::adjacent_find(supported.begin(), supported.end()) != supported.end())
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        for (const auto semantic : supported)
        {
            const auto port = std::ranges::find(capability->ports, semantic, &EffectPortDefinition::semantic);
            if (port == capability->ports.end()) return cxx::unexpected(RenderError{kCapabilityDeclaration});
            if (port->direction != graph::EPinDirection::OUTPUT)
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
        }
        std::vector<CommandId> provided;
        provided.reserve(implementation->command_bindings.size());
        for (const auto& binding : implementation->command_bindings) provided.push_back(binding.command);
        auto required = capability->commands;
        const auto command_key = [](CommandId command) noexcept { return commandTag(command); };
        std::ranges::sort(provided, {}, command_key);
        std::ranges::sort(required, {}, command_key);
        if (provided != required) return cxx::unexpected(RenderError{kCapabilityDeclaration});
        implementation->supported_outputs = std::move(supported);
        const auto key = implementationKey(*implementation);
        const auto position = std::ranges::lower_bound(implementations_, key, {}, [](const auto& item) noexcept
        {
            return implementationKey(*item);
        });
        if (position != implementations_.end() && implementationKey(**position) == key)
            return cxx::unexpected(RenderError{kCapabilityConflict});
        implementations_.insert(position, std::move(implementation));
        return {};
    }

    const CapabilityDefinition*
    EffectCatalog::findCapability(graph::NodeTypeId capability, std::uint32_t version) const & noexcept
    {
        const auto key = std::pair{capability.value, version};
        const auto found = std::ranges::lower_bound(capabilities_, key, {}, [](const auto& item) noexcept
        {
            return capabilityKey(*item);
        });
        if (found == capabilities_.end() || capabilityKey(**found) != key) return nullptr;
        return found->get();
    }

    const EffectImplementation* EffectCatalog::findImplementation(
        graph::NodeTypeId capability,
        std::uint32_t capability_version,
        const ImplementationSelection& selection
    ) const & noexcept
    {
        const auto key = std::tuple{
            capability.value, capability_version, std::string_view{selection.canonical_name}, selection.version
        };
        const auto found = std::ranges::lower_bound(implementations_, key, {}, [](const auto& item) noexcept
        {
            return implementationKey(*item);
        });
        if (found == implementations_.end() || implementationKey(**found) != key) return nullptr;
        return found->get();
    }

    std::vector<ImplementationOption>
    EffectCatalog::listImplementations(graph::NodeTypeId capability, std::uint32_t version) const noexcept
    {
        std::vector<ImplementationOption> result;
        for (const auto& implementation : implementations_)
        {
            const bool is_match = implementation->capability == capability &&
                implementation->capability_version == version;
            if (is_match) result.push_back({implementation->identity, implementation->display_name});
        }
        return result;
    }
}
