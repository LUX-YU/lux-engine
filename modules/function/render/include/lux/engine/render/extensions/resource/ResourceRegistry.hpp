#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/commands/CommandBindingLocation.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/extensions/detail/CommandDispatchContext.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <lux/engine/render/core/FinalBufferVersion.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <array>
#include <deque>
#include <map>
#include <optional>

namespace lux::render
{

    class ResourceSet;
    class RenderServer;
    namespace detail
    {
        class RasterMeshInstanceStorage;
        class RenderGraphInstance;
        struct TargetReservation;
        struct GraphBindingTag;
    }
    namespace runtime_detail { struct BoundStorageProjection; }

    class LUX_RENDER_PUBLIC ResourceRegistry final
    {
    public:
        // Device-owner notification. UINT32_MAX replaces every FIF slice of the projection.
        [[nodiscard]] RenderResult<std::size_t> publishGraphInput(
            ResourceInstanceId storage,
            std::uint32_t projection,
            std::uint32_t frame,
            const detail::ResourceInput& value
        ) noexcept;

        ResourceRegistry() noexcept = default;
        ~ResourceRegistry() noexcept;
        ResourceRegistry(const ResourceRegistry&) = delete;
        ResourceRegistry& operator=(const ResourceRegistry&) = delete;

        // Configuration/device-thread cold transaction; no instance creation or GPU side effect.
        [[nodiscard]] RenderResult<ResourceTypeId>
        installFactory(ResourceRegistration factory, ConfigValue configuration = {}) noexcept;

        template <class Config>
        [[nodiscard]] RenderResult<ResourceTypeId>
        installFactory(ResourceRegistration factory, Config configuration) noexcept
        {
            auto value = ConfigValue::make(std::move(configuration));
            if (!value)
            {
                return cxx::unexpected(value.error());
            }
            return installFactory(std::move(factory), std::move(*value));
        }

        // Cold observation of the sole type directory; the visitor must not mutate it.
        template <class Visitor> void forEachType(Visitor&& visitor) const noexcept
        {
            for (const auto& [name, id] : types_by_name_)
            {
                visitor(id, *types_.find(id)->factory.descriptor);
            }
        }

        [[nodiscard]] ResourceTypeId typeId(ResourceNameView name) const noexcept;
        [[nodiscard]] const ResourceRegistration* factory(ResourceTypeId type) const noexcept;
        [[nodiscard]] const ResourceRegistration* factory(ResourceNameView type) const noexcept;
        [[nodiscard]] const ConfigValue* configuration(ResourceTypeId type) const noexcept;
        [[nodiscard]] RenderResult<const ResourceRegistration*>
        qualify(ResourceTypeId, EResourceDomain, const ResourceCreateContext&) const noexcept;

        [[nodiscard]] ResourceTypeId declarationOf(ResourceInstanceId) const noexcept;

        [[nodiscard]] const detail::DeviceSession* deviceOf(ResourceInstanceId) const noexcept;
        [[nodiscard]] bool hasExclusiveCommand(ResourceInstanceId) const noexcept;
        [[nodiscard]] bool canReference(ResourceInstanceId owner, ResourceInstanceId dependency) const noexcept;
        [[nodiscard]] const CommandBindingLocation* commandBinding(ResourceTypeId, CommandId) const noexcept;

        // Boundary lookup; the instance index borrows, never owns a Implementation object.
        template <class T>
            requires(!std::is_void_v<typename T::Ops>)
        [[nodiscard]] RenderResult<typename T::Access> tryGet(ResourceInstanceId id) noexcept
        {
            auto* entry = entries_.find(id);
            if (!entry || !entry->instance)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            const auto& factory = types_.find(entry->type)->factory;
            const bool is_wrong_descriptor = factory.descriptor->type != T::descriptor.type;
            const auto& metadata = *factory.storage();
            const bool is_wrong_operations = metadata.ops_type != cxx::make_basic_type_info<typename T::Ops>();
            const bool is_wrong_interface = is_wrong_descriptor || is_wrong_operations;
            if (is_wrong_interface)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            const auto& operations = *static_cast<const typename T::Ops*>(metadata.ops.get());
            return typename T::Access(entry->instance, operations);
        }

        // One qualified boundary lookup for typed callers and registered command thunks alike.
        [[nodiscard]] RenderResult<void*> borrow(ResourceInstanceId id, cxx::basic_type_info implementation) const noexcept
        {
            auto* entry = entries_.find(id);
            if (!entry || !entry->instance)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            if (types_.find(entry->type)->factory.storage()->implementation_type != implementation)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            return entry->instance;
        }

        template <class Implementation>
        [[nodiscard]] RenderResult<Implementation*> borrow(ResourceInstanceId id) const noexcept
        {
            constexpr auto implementation = cxx::make_basic_type_info<std::remove_const_t<Implementation>>();
            auto result = borrow(id, implementation);
            if (!result)
            {
                return cxx::unexpected(result.error());
            }
            return static_cast<Implementation*>(*result);
        }

        // Non-owning boundary lookup; valid until its ResourceSet removes the instance.
        [[nodiscard]] void* resourceInstance(ResourceInstanceId id) const noexcept;

        [[nodiscard]] std::size_t size() const noexcept
        {
            return entries_.size();
        }

        [[nodiscard]] std::uint64_t creations() const noexcept
        {
            return creations_;
        }

    private:
        friend class ResourceSet;
        friend class detail::RasterMeshInstanceStorage;
        friend class RenderServer;
        friend void runtime_detail::finishResourceCommand(ResourceRegistry&, ResourceInstanceId) noexcept;

        [[nodiscard]] RenderResult<cxx::SlotKey<detail::GraphBindingTag>> bindGraphProjection(
            const runtime_detail::BoundStorageProjection&,
            const detail::TargetReservation&,
            detail::RenderGraphInstance&,
            bool may_access
        ) noexcept;
        void unbindGraphProjection(
            const runtime_detail::BoundStorageProjection&, const detail::TargetReservation&
        ) noexcept;

        struct GraphBindingUse final
        {
            RenderJobId job;
            std::uint64_t start_serial;
            GraphResourceId resource;
            detail::RenderGraphInstance* instance;
            std::optional<GraphBufferRange> writer;
            bool may_access;
            std::uint32_t canonical_queue;
        };

        struct ProjectionBuffer final
        {
            nri::Buffer* buffer = nullptr;
            GraphBufferRange bytes{};
            bool operator==(const ProjectionBuffer&) const noexcept = default;
            [[nodiscard]] ProjectionBuffer part(GraphBufferRange range) const noexcept;
            [[nodiscard]] bool overlaps(ProjectionBuffer other) const noexcept;
        };

        struct ProductSample final
        {
            ProjectionBuffer backing;
            RenderJobId writer;
            std::uint64_t frame_index = 0;
        };

        struct ProductBufferUse final
        {
            nri::Buffer* buffer;
            detail::SubmissionFrontiers writes, reads;
            std::uint32_t prepared_readers = 0, prepared_writers = 0;
            std::uint32_t read_queue = kGraphInvalidIndex;
            std::uint32_t read_projection = kGraphInvalidIndex;
        };

        struct BufferProduct final
        {
            std::uint32_t projection;
            GraphBufferRange range;
            std::array<ProductSample, 3> samples;
        };

        struct ProjectionBindings final
        {
            std::array<ProjectionBuffer, 3> buffers{};
            cxx::SlotMap<GraphBindingUse, detail::GraphBindingTag> users;
        };

        struct ResolvedCommand final
        {
            CommandBindingLocation binding;
            cxx::basic_type_info implementation;
        };

        [[nodiscard]] RenderResult<ResolvedCommand>
        beginCommand(ResourceInstanceId, CommandId, bool exclusive) noexcept;
        void finishCommand(ResourceInstanceId) noexcept;

        [[nodiscard]] bool hasBorrows(ResourceInstanceId) const noexcept;
        [[nodiscard]] bool hasDependents(ResourceInstanceId) const noexcept;
        [[nodiscard]] RenderResult<ResourceInstanceId>
        requireDuringConstruction(ResourceCreateContext&, ResourceInstanceId, ResourceTypeId) noexcept;
        [[nodiscard]] RenderResult<void> retainExecution(ResourceInstanceId) noexcept;
        void releaseExecution(ResourceInstanceId) noexcept;
        [[nodiscard]] RenderResult<void> bindCommand(ResourceTypeId, CommandBindingLocation) noexcept;

        // Internal registration primitives: built-ins publish known declarations directly;
        // external registration validates the complete closure before calling publishFactory().
        [[nodiscard]] RenderResult<void> validateFactory(const ResourceRegistration&) const noexcept;
        [[nodiscard]] RenderResult<void>
        validateFactory(const ResourceRegistration&, const ConfigValue&) const noexcept;
        ResourceTypeId publishFactory(ResourceRegistration, ConfigValue = {}) noexcept;

        // ResourceSet owns instances; this index alone mutates their identities and borrowed locations.
        [[nodiscard]] ResourceInstanceId reserve(ResourceSet&, ResourceTypeId) noexcept;
        [[nodiscard]] RenderResult<StorageOwner>
        create(ResourceCreateContext&, ResourceInstanceId, const void* configuration) noexcept;
        void publish(ResourceInstanceId, std::size_t position, void* instance) noexcept;
        void relocate(ResourceInstanceId, std::size_t position) noexcept;
        void revoke(ResourceInstanceId) noexcept;
        [[nodiscard]] std::optional<std::size_t> position(ResourceInstanceId, const ResourceSet&) const noexcept;

        struct Type final
        {
            Type(ResourceRegistration, ConfigValue) noexcept;
            Type(const Type&) = delete;
            Type& operator=(const Type&) = delete;

            std::deque<std::string> names;
            ResourceDefinition descriptor;
            std::vector<CommandDescriptor> command_descriptors;
            std::vector<const CommandDescriptor*> command_views;
            std::vector<CommandBinding> command_bindings;
            std::vector<cxx::basic_type_info> frame_inputs;
            std::vector<FrameChannel> frame_channels;
            ResourceRegistration factory;
            ConfigValue configuration;
            std::vector<CommandBindingLocation> commands;
        };

        struct Entry final
        {
            ResourceSet* storage_set;
            std::size_t position;
            ResourceTypeId type;
            void* instance = nullptr;
            std::uint32_t command_uses = 0;
            std::uint32_t execution_uses = 0;
            bool exclusive_command = false;
            std::uint32_t dependent_storages = 0;
            std::vector<ResourceInstanceId> dependencies{};
            std::map<std::uint32_t, ProjectionBindings> graph_bindings{};
            std::vector<BufferProduct> products{};
            std::vector<ProductBufferUse> product_buffers{};
        };

        RenderResult<detail::ResourceInput>
        selectProduct(ResourceInstanceId, std::uint32_t projection, FinalBufferVersion) const noexcept;
        RenderResult<bool> pinBufferUse(const runtime_detail::BoundStorageProjection&, std::uint32_t slot) noexcept;
        void unpinBufferUse(const runtime_detail::BoundStorageProjection&, std::uint32_t slot) noexcept;
        RenderResult<bool>
        pinBufferUses(std::span<const runtime_detail::BoundStorageProjection>, std::uint32_t slot) noexcept;
        void unpinBufferUses(std::span<const runtime_detail::BoundStorageProjection>, std::uint32_t) noexcept;
        void acceptProductUse(
            const runtime_detail::BoundStorageProjection&, std::uint32_t slot, const detail::SubmissionFrontiers&
        ) noexcept;
        void invalidateProduct(
            const runtime_detail::BoundStorageProjection&, std::uint32_t slot, GraphBufferRange
        ) noexcept;
        void publishProduct(const runtime_detail::BoundStorageProjection&, FinalBufferVersion) noexcept;
        void prepareProductBanks(
            const runtime_detail::BoundStorageProjection&, const std::array<ProjectionBuffer, 3>&
        ) noexcept;
        void reserveProductBuffer(ResourceInstanceId, nri::Buffer*) noexcept;
        void pruneBufferProducts(ResourceInstanceId) noexcept;
        RenderResult<void> replaceProductBacking(
            ResourceInstanceId, std::uint32_t projection, std::uint32_t first, std::span<const ProjectionBuffer>
        ) noexcept;

        cxx::StableSlotMap<Type, ResourceTypeTag> types_;
        static RenderResult<void> qualifyGraphBinding(
            const Entry&, std::uint32_t projection, const std::array<ProjectionBuffer, 3>&, const GraphBindingUse&
        ) noexcept;
        cxx::SlotMap<Entry, ResourceInstanceTag> entries_;
        std::map<std::string, ResourceTypeId, std::less<>> types_by_name_;
        // Derived non-owning index of the existing Device/Scene/View sets.
        std::uint64_t creations_ = 0;
    };
} // namespace lux::render
