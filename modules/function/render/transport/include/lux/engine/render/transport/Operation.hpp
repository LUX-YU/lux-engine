#pragma once

#include <memory>
#include <optional>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/data/Wire.hpp>

namespace lux::render
{
    struct OperationTag;
    struct RequestTag;
    struct OperationTypeTag;
    using OperationId = cxx::SlotKey<OperationTag>;
    using RequestId = cxx::SlotKey<RequestTag>;
    using OperationTypeIdView = cxx::SchemaIdView<OperationTypeTag>;

    enum class ECompletionContract : std::uint8_t { STATE_COMMITTED, RESOURCE_GPU_READY, RESULT_AVAILABLE };
    enum class EDelivery : std::uint8_t { COMMAND, PRIORITY_COMMAND, BULK };
    enum class EOperationOutcome : std::uint8_t { SUCCESS, FAILED, CANCELLED };

    struct OperationDescriptor final
    {
        OperationTypeIdView type;
        cxx::SchemaVersion version;
        DataSchemaDescriptor payload;
        std::optional<DataSchemaDescriptor> reply;
        ECompletionContract completion = ECompletionContract::STATE_COMMITTED;
    };

    template <WireData Payload, typename Reply = void>
    requires (std::is_void_v<Reply> || WireData<Reply>)
    [[nodiscard]] OperationDescriptor operationDescriptor(
        std::string_view name,
        cxx::SchemaVersion version = {1, 0},
        ECompletionContract completion = ECompletionContract::STATE_COMMITTED
    ) noexcept
    {
        OperationDescriptor result{OperationTypeIdView(name), version, DataSchema<Payload>::describe(), {}, completion};
        if constexpr (!std::is_void_v<Reply>) result.reply = DataSchema<Reply>::describe();
        return result;
    }

    inline constexpr auto kTransportCapacity = error::errorId("lux.render.transport.capacity");
    inline constexpr auto kTransportContract = error::errorId("lux.render.transport.contract");
    inline constexpr auto kTransportOwner = error::errorId("lux.render.transport.owner");
    inline constexpr auto kTransportStale = error::errorId("lux.render.transport.stale");
    inline constexpr auto kTransportStopped = error::errorId("lux.render.transport.stopped");
    inline constexpr auto kTransportCancelled = error::errorId("lux.render.transport.cancelled");
    inline constexpr auto kTransportBusy = error::errorId("lux.render.transport.busy");
    inline constexpr auto kTransportThread = error::errorId("lux.render.transport.thread");
    inline constexpr auto kTransportBudget = error::errorId("lux.render.transport.budget");

    [[nodiscard]] std::span<const error::ErrorDescriptor> renderTransportErrorDescriptors() noexcept;

    namespace detail
    {
        struct EndpointIdentity final {};
        struct TransportState;
        struct Routes;
        struct Binding final
        {
            std::shared_ptr<const EndpointIdentity> owner;
            OperationId operation;
        };
    }

    class RenderRouteTable;
    class TransportSender;
    class CommandBatch;

    template <WireData Payload, typename Reply = void>
    class BoundOperation final
    {
    public:
        [[nodiscard]] OperationId localId() const noexcept { return binding_.operation; }

    private:
        friend class RenderRouteTable;
        friend class TransportSender;
        friend class CommandBatch;
        explicit BoundOperation(detail::Binding binding) noexcept : binding_(std::move(binding)) {}
        detail::Binding binding_;
    };
}
