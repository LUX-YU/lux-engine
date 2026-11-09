#pragma once

#include <algorithm>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/capabilities/CapabilityCatalog.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include <lux/engine/render/extensions/features/CapabilityLowering.hpp>
#include <lux/engine/render/extensions/storage/StorageDescriptor.hpp>
#include <map>

namespace lux::render
{
    enum class EStorageScope : std::uint8_t
    {
        GLOBAL,
        SCENE,
        VIEW,
        ATTACHMENT
    };

    // An implementation's actual professional Storage dependency. It is never an application
    // payload contract. StorageSet owns the instance; this declaration owns no resource or scope.
    struct CapabilityStorageDependency final
    {
        EStorageScope scope = EStorageScope::GLOBAL;
        StorageNameView storage_type;
        std::string_view storage_key;
        // Optional cold projection from implementation configuration to this Storage's concrete configuration.
        // The returned value is adopted as immutable Attach input, never a second Storage owner.
        RenderResult<ConfigValue> (*configuration)(const void*) noexcept = nullptr;
        bool operator==(const CapabilityStorageDependency&) const noexcept = default;
    };

    // Actual Command call plus its optional resource destination within this implementation.
    // Related create/update/release methods name the same dependency; their logical handles
    // are qualified by that actual Storage rather than redirected to a foreign instance.
    struct CapabilityCommandBinding final
    {
        CommandBindingLocation location;
        std::uint32_t storage_dependency = UINT32_MAX;
        bool operator==(const CapabilityCommandBinding&) const noexcept = default;
    };

    // One implementation can contribute several independent Features to the same Effect.
    // IDs borrow registered graph factories; this record owns no Feature or GPU instance.
    struct CapabilityImplementation final
    {
        std::vector<FeatureId> features;
        std::string implementation;
        std::string description;
        CapabilityGraphDescription graph;
        RenderConfigurationFormat configuration;
        cxx::type_info config_type;
        // Composition and configuration belong to the implementation, never to a leaf Feature.
        std::span<const CapabilityStorageDependency> storage_dependencies;
        RenderResult<ConfigValue> (*make_default_configuration)() noexcept = nullptr;
        RenderResult<ConfigValue> (*decode_configuration)(std::span<const std::byte>) noexcept = nullptr;
        RenderResult<CapabilityGraphContribution> (*build_graph)(const CapabilityBuildInput&) noexcept = nullptr;
        FrameParameterProjection project_parameters = nullptr;
        // Exact calls already owned by CommandRegistry, in the implementation's declared method order.
        std::vector<CapabilityCommandBinding> command_bindings;
        std::vector<FrameDataCall> frame_calls;

        bool operator==(const CapabilityImplementation& other) const noexcept
        {
            return features == other.features && implementation == other.implementation &&
                   description == other.description && graph == other.graph && configuration == other.configuration &&
                   config_type == other.config_type &&
                   std::ranges::equal(storage_dependencies, other.storage_dependencies) &&
                   make_default_configuration == other.make_default_configuration &&
                   decode_configuration == other.decode_configuration && build_graph == other.build_graph &&
                   project_parameters == other.project_parameters && command_bindings == other.command_bindings &&
                   frame_calls == other.frame_calls;
        }
    };

    // RenderThread's capability directory. Register at a control safe point, before dispatch resumes.
    // Declarations and their callback code must outlive the Server; this class owns no plugin or GPU object.
    class LUX_RENDER_PUBLIC CapabilityRegistry final
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
