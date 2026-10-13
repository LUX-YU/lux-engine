#pragma once

#include <lux/engine/render/transport/Operation.hpp>

namespace lux::render
{
    class ReplyPromise;
    using OperationHandler = void (*)(void*, ReplyPromise) noexcept;

    // Cold assembly owns descriptor snapshots. Handler context is borrowed until
    // the receiver has stopped and all active calls have joined.
    class RenderRouteTable final
    {
    public:
        explicit RenderRouteTable(std::uint32_t capacity) noexcept;
        ~RenderRouteTable();
        RenderRouteTable(RenderRouteTable&&) noexcept;
        RenderRouteTable& operator=(RenderRouteTable&&) noexcept;
        RenderRouteTable(const RenderRouteTable&) = delete;
        RenderRouteTable& operator=(const RenderRouteTable&) = delete;

        [[nodiscard]] RenderResult<OperationId>
        registerOperation(const OperationDescriptor& descriptor, void* context, OperationHandler handler) noexcept;
        [[nodiscard]] RenderResult<void> remove(OperationId id) noexcept;
        [[nodiscard]] RenderResult<void> clear() noexcept;

        template <WireData Payload, typename Reply = void>
        [[nodiscard]] RenderResult<BoundOperation<Payload, Reply>>
        bind(const OperationDescriptor& descriptor) const noexcept
        {
            auto typed = operationDescriptor<Payload, Reply>(
                descriptor.type.name(), descriptor.version, descriptor.completion
            );
            auto binding = bindErased(descriptor, typed);
            if (!binding) return cxx::unexpected(binding.error());
            return BoundOperation<Payload, Reply>(std::move(*binding));
        }

    private:
        friend class RenderTransport;
        [[nodiscard]] RenderResult<detail::Binding>
        bindErased(const OperationDescriptor& descriptor, const OperationDescriptor& typed) const noexcept;
        std::shared_ptr<detail::Routes> routes_;
    };
}
