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

    namespace detail
    {
        struct VertexRegistration;
    }

    /// CPU registration owner, not a GPU retirement ticket. All access is on the render owner thread.
    /// Registry or source destruction invalidates surviving leases. Normal owners place the lease
    /// after source backing, so revocation happens before backing destruction at the original GPU safe point.
    class LUX_FUNCTION_PUBLIC VertexSourceRegistration final
    {
    public:
        VertexSourceRegistration() noexcept;
        ~VertexSourceRegistration() noexcept;
        VertexSourceRegistration(VertexSourceRegistration&&) noexcept;
        VertexSourceRegistration& operator=(VertexSourceRegistration&&) noexcept;
        VertexSourceRegistration(const VertexSourceRegistration&) = delete;
        VertexSourceRegistration& operator=(const VertexSourceRegistration&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] std::uint32_t poolId() const noexcept;
        void refresh() noexcept;

    private:
        friend class VertexPoolRegistry;
        explicit VertexSourceRegistration(std::unique_ptr<detail::VertexRegistration> registration) noexcept;
        std::unique_ptr<detail::VertexRegistration> registration_;
    };

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

        ~VertexPoolRegistry() noexcept;
        VertexPoolRegistry(const VertexPoolRegistry&) = delete;
        VertexPoolRegistry& operator=(const VertexPoolRegistry&) = delete;
        VertexPoolRegistry(VertexPoolRegistry&&) = delete;
        VertexPoolRegistry& operator=(VertexPoolRegistry&&) = delete;

        /// Assign the lowest free slot and write every frame target. Rejection publishes nothing.
        /// The returned lease is the only revocation authority; copied GPU pool ids are not leases.
        [[nodiscard]] Expected<VertexSourceRegistration> registerSource(IVertexSource& source) noexcept;

        [[nodiscard]] bool isRegistered(std::uint32_t pool_id) const noexcept;

    private:
        friend struct detail::VertexRegistration;
        friend class VertexSourceRegistration;

        VertexPoolRegistry(VkDevice device, DomainWriteTarget domain, RenderErrorSink* error_sink) noexcept;
        void writeDescriptor(std::uint32_t pool_id, IVertexSource& source) noexcept;

        VkDevice device_;
        DomainWriteTarget domain_;
        RenderErrorSink* error_sink_;
        std::array<detail::VertexRegistration*, kVertexPoolMaxCount> slots_{};
    };
} // namespace lux::render
