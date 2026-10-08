#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::render
{
    class DeviceContext;
    class IVertexSource;
    class RenderErrorSink;

    /// Per-scene source slots. Descriptor sets are borrowed from the scene domain owner.
    /// A published registry always has a complete write target; it owns no duplicate layout.
    class LUX_FUNCTION_PUBLIC VertexPoolRegistry final
    {
    public:
        using CreateResult = Expected<std::unique_ptr<VertexPoolRegistry>>;

        [[nodiscard]] static CreateResult create(
            DeviceContext& device,
            std::span<const VkDescriptorSet> sets,
            std::uint32_t binding_offset,
            RenderErrorSink* error_sink = nullptr
        ) noexcept;

        ~VertexPoolRegistry() = default;
        VertexPoolRegistry(const VertexPoolRegistry&) = delete;
        VertexPoolRegistry& operator=(const VertexPoolRegistry&) = delete;
        VertexPoolRegistry(VertexPoolRegistry&&) = delete;
        VertexPoolRegistry& operator=(VertexPoolRegistry&&) = delete;

        /// Assign the lowest free bindless slot and write every frame target.
        /// Returns ~0u at capacity. The source and buffer must outlive the registration.
        std::uint32_t registerSource(IVertexSource& source) noexcept;

        /// Revoke the source slot. The caller first retires all uses of the pool id.
        /// Unused descriptors stay untouched under PARTIALLY_BOUND/UPDATE_AFTER_BIND.
        void unregisterSource(std::uint32_t pool_id) noexcept;

        /// Refresh an existing source after its backing buffer changes.
        void refreshSource(std::uint32_t pool_id) noexcept;
        [[nodiscard]] bool isRegistered(std::uint32_t pool_id) const noexcept;

    private:
        VertexPoolRegistry(VkDevice device, DomainWriteTarget domain, RenderErrorSink* error_sink) noexcept;
        void writeDescriptor(std::uint32_t pool_id, IVertexSource& source) noexcept;

        VkDevice device_;
        DomainWriteTarget domain_;
        RenderErrorSink* error_sink_;
        std::array<IVertexSource*, kVertexPoolMaxCount> slots_{};
    };
} // namespace lux::render
