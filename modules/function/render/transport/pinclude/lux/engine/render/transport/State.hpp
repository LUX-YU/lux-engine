#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/cxx/concurrent/AdmissionGate.hpp>
#include <lux/cxx/concurrent/BoundedSpscFrameRing.hpp>
#include <lux/cxx/concurrent/BudgetGate.hpp>
#include <lux/engine/render/transport/Transport.hpp>

namespace lux::render::detail
{
    struct OwnedSchema final
    {
        explicit OwnedSchema(const DataSchemaDescriptor& source) noexcept;
        DataTypeId name;
        std::vector<std::string> names;
        std::vector<std::vector<std::uint64_t>> enums;
        std::vector<DataField> fields;
        DataSchemaDescriptor descriptor;
    };

    struct RouteEntry final
    {
        RouteEntry(const OperationDescriptor& source, void* context, OperationHandler handler) noexcept;
        cxx::SchemaId<OperationTypeTag> name;
        OwnedSchema payload;
        std::unique_ptr<OwnedSchema> reply;
        OperationDescriptor descriptor;
        OperationId id;
        void* context;
        OperationHandler handler;
    };

    struct Routes final
    {
        explicit Routes(std::uint32_t count) noexcept : capacity(count) { entries.reserve(count); }
        std::shared_ptr<const EndpointIdentity> identity = std::make_shared<const EndpointIdentity>();
        cxx::SlotMap<std::unique_ptr<RouteEntry>, OperationTag> entries;
        std::uint32_t capacity;
        bool frozen = false;
    };

    [[nodiscard]] RenderResult<void>
    compatibleOperations(const OperationDescriptor& left, const OperationDescriptor& right) noexcept;

    [[nodiscard]] RenderResult<Binding>
    bindOperation(
        const Routes& routes, const OperationDescriptor& descriptor, const OperationDescriptor& typed
    ) noexcept;

    struct RequestCell final
    {
        OperationId operation;
        bool client = true;
        bool work_done = false;
        bool terminal = false;
        EOperationOutcome outcome = EOperationOutcome::CANCELLED;
        RenderError error{};
        explicit RequestCell(OperationId id) noexcept : operation(id) {}
    };

    // Aux survives slot recycling, preserving attachment vector capacity.
    struct RequestBacking final
    {
        OwnedWireBytes input;
        std::vector<OwnedWireBytes> attachments;
        OwnedWireBytes output;
        cxx::BudgetReservation<> budget;
    };

    struct TransportState final : std::enable_shared_from_this<TransportState>
    {
        using Ring  = cxx::BoundedSpscFrameRing<std::vector<RequestId>, 4>;
        using Cells = cxx::StableSlotMap<RequestCell, RequestTag, RequestBacking>;

        TransportState(std::shared_ptr<Routes> routes, TransportCapacity capacity) noexcept;
        [[nodiscard]] const RouteEntry* route(OperationId id) const noexcept;
        [[nodiscard]] RenderResult<RequestId> reserve(
            OperationId id, const OwnedWireBytes& bytes, std::span<const OwnedWireBytes> attachments, bool bulk
        ) noexcept;

        void rollback(RequestId id) noexcept;
        void releaseWork(RequestId id) noexcept;

        [[nodiscard]] RenderResult<void>
        finish(RequestId id, EOperationOutcome outcome, RenderError error, OwnedWireBytes bytes = {}) noexcept;

        [[nodiscard]] RenderResult<std::optional<TerminalReply>>
        take(RequestId id) noexcept;

        [[nodiscard]] RenderResult<void>
        abandon(RequestId id) noexcept;

        [[nodiscard]] RenderResult<WireView>
        input(RequestId id) noexcept;

        void stop() noexcept;
        void notify() noexcept;

        std::shared_ptr<Routes>     routes;
        TransportCapacity           capacity;
        const std::thread::id       producer = std::this_thread::get_id();
        cxx::AdmissionGate<>        admission;
        cxx::BudgetGate<>           budget;
        Cells                       cells;
        std::mutex                  reply_mutex;
        std::atomic<std::size_t>    work_count{};
        std::atomic<std::size_t>    readers{};
        Ring                        commands;
        Ring                        priority;
        std::mutex                  consumer_mutex;
        std::thread::id             consumer;
        bool                        spsc_reading = false;
        std::mutex                  bulk_mutex;
        std::vector<RequestId>      bulk;
        std::size_t                 bulk_head = 0;
        std::atomic<std::size_t>    bulk_count{};
        std::atomic<std::uint64_t>  wake_epoch{};
        std::mutex                  wake_mutex;
        std::condition_variable_any wake;
    };
}
