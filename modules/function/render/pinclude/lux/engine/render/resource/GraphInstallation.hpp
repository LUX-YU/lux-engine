#pragma once

#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>

namespace lux::render
{
    // Lives on the owner stack until Start/Rebind publishes or discards the candidate.
    class ResourceRegistry::GraphInstallation final
    {
    public:
        GraphInstallation(ResourceRegistry&, runtime_detail::JobExecution&) noexcept;
        ~GraphInstallation() noexcept;
        GraphInstallation(const GraphInstallation&) = delete;
        GraphInstallation& operator=(const GraphInstallation&) = delete;

        [[nodiscard]] runtime_detail::JobExecution& execution() const noexcept { return execution_; }
        [[nodiscard]] RenderResult<void> resolve(runtime_detail::BoundStorageProjection&, bool may_access) noexcept;
        [[nodiscard]] bool holds(ResourceInstanceId) const noexcept;
        [[nodiscard]] RenderResult<void> reservePublication() noexcept;
        void publish() noexcept;

    private:
        struct StorageChanges final
        {
            ResourceInstanceId storage;
            ProjectionMap projections;
            std::vector<BufferProduct> products;
            std::vector<ProductBufferUse> product_buffers;
        };
        RenderResult<ProjectionFamily>
        resolveBacking(const runtime_detail::BoundStorageProjection&, const GraphBindingUse&) noexcept;
        StorageChanges& changesFor(ResourceInstanceId) noexcept;
        void prepareProducts(
            StorageChanges&, const runtime_detail::BoundStorageProjection&, const ProjectionFamily&
        ) noexcept;

        ResourceRegistry& registry_;
        runtime_detail::JobExecution& execution_;
        std::vector<StorageChanges> changes_;
    };
}
