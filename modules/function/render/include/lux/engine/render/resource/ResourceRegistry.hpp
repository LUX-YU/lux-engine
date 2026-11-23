#pragma once

#include <lux/engine/render/runtime/JobId.hpp>
#include <lux/engine/render/visibility.h>

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/resource/StorageMeta.hpp>
#include <lux/engine/render/graph/BufferBoundary.hpp>
#include <lux/engine/render/resource/ResourceBinding.hpp>
#include <lux/engine/render/core/FinalBufferVersion.hpp>
#include <lux/engine/render/device/SubmissionFrontiers.hpp>
#include <array>
#include <deque>
#include <map>
#include <optional>

namespace lux::render
{

    class ResourceRegistry;
    class ResourceSet;
    class RenderServer;
    namespace detail
    {
        class RasterMeshInstanceStorage;
        struct GraphBindingTag;
    }
    namespace runtime_detail
    {
        LUX_RENDER_PUBLIC void finishResourceCommand(ResourceRegistry&, ResourceInstanceId) noexcept;
        struct BoundStorageProjection;
        struct JobExecution;
        struct RenderJob;
    }

    struct GraphImagePublication final
    {
        std::uint32_t projection;
        std::uint32_t frame; // UINT32_MAX addresses the installed projection's complete FIF family.
        nri::Texture* image;
        detail::ResourceQueueUses uses{};
    };

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

        // Device-owner transaction. All installed projections change together after every reader qualifies.
        [[nodiscard]] RenderResult<std::size_t>
        publishGraphImages(ResourceInstanceId storage, std::span<const GraphImagePublication> updates) noexcept;

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

        // Boundary lookup; the instance index borrows, never owns a Implementation object.
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

        class GraphInstallation;
        const GraphInstallation* installation_ = nullptr;
        void unbindGraphProjection(
            const runtime_detail::BoundStorageProjection&, const detail::ExecutionBindingId&
        ) noexcept;

        struct GraphBindingUse final
        {
            detail::ExecutionBindingId binding;
            GraphResourceId resource;
            std::optional<GraphBufferRange> writer;
            bool may_access;
            detail::BufferBoundary boundary;
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
            detail::BufferBoundary selected;
        };

        struct BufferProduct final
        {
            std::uint32_t projection;
            GraphBufferRange range;
            std::array<ProductSample, 3> samples;
        };

        struct ProjectionFamily final
        {
            std::uint32_t frame_count = 0;
            bool image_projection = false;
            std::array<ProjectionBuffer, 3> buffers{};
            std::array<nri::Texture*, 3> images{};
            std::array<detail::ResourceQueueUses, 3> queue_uses{};
            bool operator==(const ProjectionFamily&) const noexcept = default;
        };

        struct ProjectionBindings final
        {
            ProjectionFamily backing;
            cxx::SlotMap<GraphBindingUse, detail::GraphBindingTag> users;
            std::size_t retired_slots = 0; // SlotMap permanently retires an index when its generation is exhausted.
        };

        using ProjectionMap = std::map<std::uint32_t, ProjectionBindings>;

        [[nodiscard]] RenderResult<void*> beginCommand(ResourceInstanceId, bool exclusive) noexcept;
        void finishCommand(ResourceInstanceId) noexcept;

        [[nodiscard]] bool hasBorrows(ResourceInstanceId) const noexcept;
        [[nodiscard]] bool hasDependents(ResourceInstanceId) const noexcept;
        [[nodiscard]] RenderResult<ResourceInstanceId>
        requireDuringConstruction(ResourceCreateContext&, ResourceInstanceId, ResourceTypeId) noexcept;
        [[nodiscard]] RenderResult<void> retainExecution(ResourceInstanceId) noexcept;
        void releaseExecution(ResourceInstanceId) noexcept;

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
            std::vector<cxx::basic_type_info> frame_inputs;
            std::vector<FrameChannel> frame_channels;
            ResourceRegistration factory;
            ConfigValue configuration;
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
            ProjectionMap graph_bindings{};
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
        void reserveProductBuffer(ResourceInstanceId, nri::Buffer*) noexcept;
        void pruneBufferProducts(ResourceInstanceId) noexcept;
        RenderResult<void> replaceProductBacking(
            ResourceInstanceId, std::uint32_t projection, std::uint32_t first, std::span<const ProjectionBuffer>
        ) noexcept;

        cxx::StableSlotMap<Type, ResourceTypeTag> types_;
        const cxx::StableSlotMap<runtime_detail::RenderJob, RenderJobTag>* jobs_ = nullptr;
        [[nodiscard]] runtime_detail::JobExecution& installedReader(detail::ExecutionBindingId) const noexcept;
        static RenderResult<void>
        qualifyGraphImport(const ProjectionMap&, std::uint32_t, const ProjectionFamily&, const GraphBindingUse&) noexcept;
        static RenderResult<void> qualifyBufferUses(
            const ProjectionMap&, std::uint32_t projection, std::span<const ProjectionBuffer>, const GraphBindingUse&
        ) noexcept;
        cxx::SlotMap<Entry, ResourceInstanceTag> entries_;
        std::map<std::string, ResourceTypeId, std::less<>> types_by_name_;
        // Derived non-owning index of the existing Device/Scene/View sets.
        std::uint64_t creations_ = 0;
    };
} // namespace lux::render
