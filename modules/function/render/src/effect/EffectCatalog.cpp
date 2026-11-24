#include <lux/engine/render/effect/EffectCatalog.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/runtime/commands/DiscoveryCommands.hpp>
#include <algorithm>
#include <tuple>
#include <utility>

namespace lux::render
{
    namespace detail
    {
        // Records stay at a stable address across catalog sorting and discovery response moves.
        // Reserve the exact name count before publishing views, including short strings held inline.
        class CapabilityMetadata final
        {
        public:
            explicit CapabilityMetadata(const EffectCapability& source)
                : descriptor(source), ports_(source.ports.begin(), source.ports.end()),
                  resources_(source.resources.begin(), source.resources.end())
            {
                names_.reserve(2 + 2 * ports_.size() + 2 * resources_.size());
                descriptor.canonical_name = names_.emplace_back(source.canonical_name);
                descriptor.display_name = names_.emplace_back(source.display_name);
                for (auto& port : ports_)
                {
                    port.name = names_.emplace_back(port.name);
                    port.type.canonical_name = names_.emplace_back(port.type.canonical_name);
                }
                for (auto& resource : resources_)
                {
                    resource.binding = names_.emplace_back(resource.binding);
                    resource.type = ResourceNameView{names_.emplace_back(resource.type.name())};
                }
                descriptor.ports = ports_;
                descriptor.resources = resources_;
            }

            CapabilityMetadata(const CapabilityMetadata&) = delete;
            CapabilityMetadata& operator=(const CapabilityMetadata&) = delete;

            EffectCapability descriptor;

        private:
            std::vector<std::string> names_;
            std::vector<EffectPortDefinition> ports_;
            std::vector<ResourceRequirement> resources_;
        };
    }

    EffectCapabilityInfo::EffectCapabilityInfo(
        const EffectCapability& source, std::vector<ImplementationOption> options, std::vector<CommandId> ids
    ) noexcept
        : implementations(std::move(options)), commands(std::move(ids)),
          metadata_(std::make_unique<detail::CapabilityMetadata>(source))
    {
    }

    EffectCapabilityInfo::~EffectCapabilityInfo() noexcept = default;

    EffectCapabilityInfo::EffectCapabilityInfo(EffectCapabilityInfo&&) noexcept = default;

    EffectCapabilityInfo& EffectCapabilityInfo::operator=(EffectCapabilityInfo&&) noexcept = default;

    EffectCapabilityInfo::EffectCapabilityInfo(const EffectCapabilityInfo& other)
        : EffectCapabilityInfo(other.definition(), other.implementations, other.commands)
    {
    }

    EffectCapabilityInfo& EffectCapabilityInfo::operator=(const EffectCapabilityInfo& other)
    {
        if (this != &other) *this = EffectCapabilityInfo(other);
        return *this;
    }

    const EffectCapability& EffectCapabilityInfo::definition() const & noexcept
    {
        static constexpr EffectCapability empty{};
        return metadata_ ? metadata_->descriptor : empty;
    }

    const EffectCapability& EffectCatalog::descriptor(const detail::CapabilityMetadata& metadata) noexcept
    {
        return metadata.descriptor;
    }

    namespace
    {
        [[nodiscard]] bool validPorts(std::span<const EffectPortDefinition> ports) noexcept
        {
            std::vector<graph::PinSemanticId> identities;
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
                identities.push_back(port.semantic);
            }
            std::ranges::sort(identities);
            // Duplicate semantics include hash collisions between distinct names.
            return std::adjacent_find(identities.begin(), identities.end()) == identities.end();
        }

        [[nodiscard]] auto capabilityKey(const EffectCapability& definition) noexcept
        {
            return std::pair{definition.id.value, definition.version};
        }

        [[nodiscard]] auto implementationKey(const ImplementationEntry& implementation) noexcept
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

    RenderResult<void> EffectCatalog::addCapability(const EffectCapability& definition) noexcept
    {
        if (sealed_) return cxx::unexpected(RenderError{kCapabilityDeclaration});
        const auto& identity = definition;
        const bool is_invalid_identity = !identity.id.valid() || identity.canonical_name.empty() ||
            identity.version == 0 || identity.id != graph::nodeTypeId(identity.canonical_name);
        if (is_invalid_identity || !validPorts(definition.ports))
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        for (std::size_t i = 0; i < definition.resources.size(); ++i)
        {
            const auto& resource = definition.resources[i];
            const auto earlier = std::span(definition.resources).first(i);
            const bool duplicate = std::ranges::find(earlier, resource.binding, &ResourceRequirement::binding) != earlier.end();
            if (!resource.isValid() || duplicate)
                return cxx::unexpected(RenderError{kCapabilityDeclaration});
        }
        for (const auto& existing : capabilities_)
        {
            if (existing->descriptor.id != identity.id) continue;
            const bool is_collision = existing->descriptor.canonical_name != identity.canonical_name;
            const bool is_duplicate_version = existing->descriptor.version == identity.version;
            if (is_collision || is_duplicate_version) return cxx::unexpected(RenderError{kCapabilityConflict});
        }
        const auto key = capabilityKey(definition);
        const auto position = std::ranges::lower_bound(capabilities_, key, {}, [](const auto& item) noexcept
        {
            return capabilityKey(item->descriptor);
        });
        capabilities_.insert(position, std::make_unique<const detail::CapabilityMetadata>(definition));
        return {};
    }

    RenderResult<void> EffectCatalog::addImplementation(
        std::unique_ptr<ImplementationEntry> implementation
    ) noexcept
    {
        if (sealed_ || !implementation) return cxx::unexpected(RenderError{kCapabilityDeclaration});
        const auto capability = findCapability(implementation->capability, implementation->capability_version);
        const bool is_invalid_identity = implementation->identity.canonical_name.empty() ||
            implementation->identity.version == 0;
        const bool is_invalid_code = !implementation->create;
        if (!capability || is_invalid_identity || is_invalid_code)
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        auto& supported = implementation->supported_outputs;
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

    const EffectCapability*
    EffectCatalog::findCapability(graph::NodeTypeId capability, std::uint32_t version) const & noexcept
    {
        const auto key = std::pair{capability.value, version};
        const auto found = std::ranges::lower_bound(capabilities_, key, {}, [](const auto& item) noexcept
        {
            return capabilityKey(item->descriptor);
        });
        if (found == capabilities_.end() || capabilityKey((*found)->descriptor) != key) return nullptr;
        return &(*found)->descriptor;
    }

    const ImplementationEntry* EffectCatalog::findImplementation(
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
        const auto key = std::pair{capability.value, version};
        const auto entries = std::ranges::equal_range(implementations_, key, {}, [](const auto& entry) noexcept
        {
            return std::pair{entry->capability.value, entry->capability_version};
        });
        std::vector<ImplementationOption> result;
        result.reserve(entries.size());
        for (const auto& entry : entries) result.push_back({entry->identity, entry->display_name});
        return result;
    }
}
