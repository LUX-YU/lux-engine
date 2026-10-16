#pragma once

#include <memory>
#include <utility>
#include <lux/engine/platform/ipc/IpcMessage.hpp>
#include <lux/engine/render/protocol/Protocol.hpp>
#include <lux/engine/render/runtime/visibility.h>

namespace lux::render
{
    class LUX_RENDER_CLIENT_PUBLIC RenderClient;

    namespace runtime_detail
    {
        struct BudgetState;

        // A slice of one Client's admitted byte budget, never a second request or asset owner.
        class LUX_RENDER_CLIENT_PUBLIC BudgetLease final
        {
        public:
            BudgetLease() noexcept = default;
            ~BudgetLease() noexcept;
            BudgetLease(BudgetLease&& other) noexcept;
            BudgetLease& operator=(BudgetLease&& other) noexcept;
            BudgetLease(const BudgetLease&) = delete;
            BudgetLease& operator=(const BudgetLease&) = delete;
            [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
            // The completion boundary has already checked bytes <= this reservation.
            [[nodiscard]] BudgetLease split(std::size_t bytes) noexcept;

        private:
            friend class ::lux::render::RenderClient;
            BudgetLease(std::shared_ptr<BudgetState> state, std::size_t bytes) noexcept
                : state_(std::move(state)), bytes_(bytes) {}
            std::shared_ptr<BudgetState> state_;
            std::size_t bytes_ = 0;
        };

        template <class T>
        struct ValueEnvelope final
        {
            // Destruction of the actual value precedes its budget refund.
            BudgetLease lease;
            T value;

            ValueEnvelope(BudgetLease input_lease, T&& input) noexcept
                : lease(std::move(input_lease)), value(std::move(input)) {}
            ValueEnvelope(ValueEnvelope&&) noexcept = default;
            ValueEnvelope& operator=(ValueEnvelope&&) = delete;
            ValueEnvelope(const ValueEnvelope&) = delete;
            ValueEnvelope& operator=(const ValueEnvelope&) = delete;
        };

        struct CommandTransport final
        {
            const CommandDescriptor* descriptor = nullptr;
            const platform::ipc::IpcTypeOps* request = nullptr;
            const platform::ipc::IpcTypeOps* response = nullptr;
            const void* (*request_value)(const platform::ipc::IpcMessage&) noexcept = nullptr;
            BudgetLease& (*request_budget)(platform::ipc::IpcMessage&) noexcept = nullptr;
            BudgetLease& (*response_budget)(platform::ipc::IpcMessage&) noexcept = nullptr;
            RenderResult<std::size_t> (*response_bytes)(const void*) noexcept = nullptr;
            void (*package_response)(platform::ipc::IpcMessage&, void*, BudgetLease,
                std::shared_ptr<const void>) noexcept = nullptr;
        };

        template <class C>
        [[nodiscard]] CommandTransport commandTransport() noexcept
        {
            using Request = typename C::Request;
            using Response = typename C::Response;
            using Input = ValueEnvelope<Request>;
            using Output = ValueEnvelope<Response>;
            static_assert(std::is_nothrow_move_constructible_v<Request>);
            static_assert(std::is_nothrow_move_constructible_v<Response>);
            return {
                &commandDescriptor<C>(),
                &platform::ipc::IpcMessage::type<Input>(),
                &platform::ipc::IpcMessage::type<Output>(),
                [](const platform::ipc::IpcMessage& message) noexcept -> const void* {
                    return &message.get<Input>().value;
                },
                [](platform::ipc::IpcMessage& message) noexcept -> BudgetLease& { return message.get<Input>().lease; },
                [](platform::ipc::IpcMessage& message) noexcept -> BudgetLease& { return message.get<Output>().lease; },
                [](const void* response) noexcept {
                    return CommandValueTraits<Response>::retainedBytes(*static_cast<const Response*>(response));
                },
                [](platform::ipc::IpcMessage& message, void* response, BudgetLease lease,
                    std::shared_ptr<const void> pin) noexcept {
                    Output output{std::move(lease), std::move(*static_cast<Response*>(response))};
                    platform::ipc::detail::moveFrom(message, platform::ipc::IpcMessage::type<Output>(),
                        &output, std::move(pin));
                }
            };
        }
    }
}
