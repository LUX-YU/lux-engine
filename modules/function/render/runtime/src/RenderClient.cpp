#include "TypedProtocol.hpp"
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <atomic>
#include <optional>

namespace lux::render::runtime_detail
{
    struct BudgetState final
    {
        std::atomic<std::size_t> retained{0};
        std::size_t limit;
        explicit BudgetState(std::size_t value) noexcept : limit(value) {}
    };

    BudgetLease::~BudgetLease() noexcept
    {
        if (bytes_) state_->retained.fetch_sub(bytes_, std::memory_order_relaxed);
    }

    BudgetLease::BudgetLease(BudgetLease&& other) noexcept
        : state_(std::move(other.state_)), bytes_(std::exchange(other.bytes_, 0)) {}

    BudgetLease& BudgetLease::operator=(BudgetLease&& other) noexcept
    {
        if (this == &other) return *this;
        if (bytes_) state_->retained.fetch_sub(bytes_, std::memory_order_relaxed);
        state_ = std::move(other.state_);
        bytes_ = std::exchange(other.bytes_, 0);
        return *this;
    }

    BudgetLease BudgetLease::split(std::size_t bytes) noexcept
    {
        bytes_ -= bytes;
        return BudgetLease{state_, bytes};
    }
}

namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    namespace
    {
        struct Pending final
        {
            RequestId id;
            SessionIdentity session{};
            CommandId command;
            const ipc::IpcTypeOps* response = nullptr;
            bool abandoned = false;
        };

        bool addBytes(std::size_t& total, std::size_t amount) noexcept
        {
            if (amount > SIZE_MAX - total) return false;
            total += amount;
            return true;
        }
    }

    struct RenderClient::Impl final
    {
        ipc::IpcRequester requester;
        cxx::SlotMap<Pending, RequestTag> requests;
        std::shared_ptr<BudgetState> budget;
        std::vector<Capability> manifest;
        std::optional<ipc::IpcBoundType<ValueEnvelope<RenderError>>> failure_type;
        SessionIdentity session{};
        std::size_t capacity;
        bool bound = false, ready_sent = false, closed = false;

        Impl(ipc::IpcRequester input, RenderClientConfig config)
            : requester(std::move(input)), budget(std::make_shared<BudgetState>(config.retained_byte_limit)),
              capacity(config.ring_capacity)
        {
            requests.reserve(capacity);
        }

        ~Impl() noexcept { requester.reset(); }
    };

    RenderResult<std::size_t> CommandValueTraits<CreateEffectRequest>::retainedBytes(
        const CreateEffectRequest& value) noexcept
    {
        std::size_t bytes = sizeof(value);
        const bool feature_overflow = value.features.capacity() > SIZE_MAX / sizeof(FeatureSelection);
        const bool attachment_overflow = value.attachments.capacity() > SIZE_MAX / sizeof(RenderPayload);
        if (feature_overflow || attachment_overflow)
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        if (!addBytes(bytes, value.features.capacity() * sizeof(FeatureSelection)) ||
            !addBytes(bytes, value.attachments.capacity() * sizeof(RenderPayload)))
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        for (const auto& selection : value.features)
        {
            // Include the terminating character in heap string backing; SSO is conservatively charged too.
            if (!addBytes(bytes, selection.type.capacity()) || !addBytes(bytes, 1) ||
                !addBytes(bytes, selection.instance.capacity()) || !addBytes(bytes, 1) ||
                !addBytes(bytes, selection.config.retainedBytes()))
                return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        for (const auto& attachment : value.attachments)
            if (!addBytes(bytes, attachment.retainedBytes()))
                return cxx::unexpected(RenderError{kRuntimeCapacity});
        return bytes;
    }

    std::span<const error::ErrorDescriptor> renderRuntimeErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.runtime.state", "Invalid runtime state"},
            {"lux.render.runtime.capacity", "Runtime capacity exhausted"},
            {"lux.render.runtime.identity", "Stale or foreign runtime identity"},
            {"lux.render.runtime.unsupported", "Unsupported runtime command"}
        };
        return descriptors;
    }

    RenderClient::RenderClient(std::unique_ptr<Impl> state) noexcept : impl_(std::move(state)) {}
    RenderClient::~RenderClient() noexcept = default;

    void RenderClient::operator delete(void* value) noexcept { ::operator delete(value); }

    RenderResult<std::unique_ptr<RenderClient>> RenderClient::create(
        std::string_view name, RenderClientConfig config) noexcept
    {
        if (!config.retained_byte_limit) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto requester = ipc::IpcCore::instance().createRequester(name, {config.ring_capacity, config.ring_capacity});
        if (!requester) return cxx::unexpected(RenderError{kRuntimeState});
        return std::unique_ptr<RenderClient>(new RenderClient(std::make_unique<Impl>(std::move(*requester), config)));
    }

    SessionIdentity RenderClient::session() const noexcept { return impl_->session; }

    RenderResult<RenderClient::Binding> RenderClient::bindCommand(const CommandDescriptor& descriptor) const noexcept
    {
        if (!impl_->bound) return cxx::unexpected(RenderError{kRuntimeState});
        if (auto valid = validateCommand(descriptor); !valid) return cxx::unexpected(valid.error());
        for (const auto& capability : impl_->manifest)
        {
            if (capability.name != descriptor.name.name()) continue;
            if (auto same = compatibleCommands(descriptor, capability.descriptor()); !same)
                return cxx::unexpected(same.error());
            return Binding{impl_->session, capability.id, capability.request_ops, capability.response_ops,
                capability.code_pin};
        }
        return cxx::unexpected(RenderError{kRuntimeUnsupported});
    }

    RenderResult<std::pair<RequestId, BudgetLease>> RenderClient::reserve(
        SessionIdentity session, CommandId command, const ipc::IpcTypeOps* response,
        std::size_t request_bytes, std::size_t reply_capacity, std::size_t envelope_bytes) noexcept
    {
        auto& state = *impl_;
        const bool hello = command.isNull();
        const bool is_invalid_state = state.closed || (hello ? state.ready_sent : !state.bound);
        if (is_invalid_state) return cxx::unexpected(RenderError{kRuntimeState});
        if (!hello && session != state.session) return cxx::unexpected(RenderError{kRuntimeIdentity});
        const bool is_full = state.requests.size() >= state.capacity;
        const bool is_small_reply = reply_capacity < sizeof(RenderError);
        if (is_full || is_small_reply) return cxx::unexpected(RenderError{kRuntimeCapacity});
        std::size_t charge = request_bytes;
        if (!addBytes(charge, reply_capacity) || !addBytes(charge, envelope_bytes) || charge > state.budget->limit)
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto current = state.budget->retained.load(std::memory_order_relaxed);
        do
        {
            if (current > state.budget->limit - charge) return cxx::unexpected(RenderError{kRuntimeCapacity});
        } while (!state.budget->retained.compare_exchange_weak(current, current + charge, std::memory_order_relaxed));
        const auto id = state.requests.emplace(Pending{{}, session, command, response});
        state.requests.find(id)->id = id;
        return std::pair{id, BudgetLease{state.budget, charge}};
    }

    RenderResult<void> RenderClient::publish(RequestId request, ipc::IpcMessage& message,
        SessionIdentity session, CommandId command, std::size_t reply_capacity) noexcept
    {
        message.setMetadata(RequestMetadata{session, request, command, reply_capacity});
        const auto sent = impl_->requester.trySend(message);
        if (!sent)
        {
            impl_->requests.erase(request);
            return cxx::unexpected(RenderError{sent.error() == ipc::EIpcError::WOULD_BLOCK ?
                kRuntimeCapacity : kRuntimeState});
        }
        return {};
    }

    RenderResult<RequestId> RenderClient::hello() noexcept
    {
        using Input = ValueEnvelope<HelloRequest>;
        using Output = ValueEnvelope<HelloResponse>;
        constexpr std::size_t reply_capacity = 64 * 1024;
        constexpr auto overhead = sizeof(Input) - sizeof(HelloRequest) + sizeof(Output) - sizeof(HelloResponse);
        auto reservation = reserve({}, {}, &ipc::IpcMessage::type<Output>(), sizeof(HelloRequest),
            reply_capacity, overhead);
        if (!reservation) return cxx::unexpected(reservation.error());
        ipc::IpcMessage message;
        message.emplace<Input>(std::move(reservation->second), HelloRequest{});
        auto sent = publish(reservation->first, message, {}, {}, reply_capacity);
        if (!sent) return cxx::unexpected(sent.error());
        impl_->ready_sent = true;
        return reservation->first;
    }

    RenderCompletion::RenderCompletion(RenderCompletion&& other) noexcept { *this = std::move(other); }

    RenderCompletion& RenderCompletion::operator=(RenderCompletion&& other) noexcept
    {
        if (this == &other) return *this;
        backing_ = std::move(other.backing_);
        header = std::exchange(other.header, {});
        session = other.session;
        command_ = std::exchange(other.command_, {});
        return *this;
    }

    RenderResult<ERenderReceive> RenderClient::tryReceive(RenderCompletion& output) noexcept
    {
        if (output.active()) return cxx::unexpected(RenderError{kRuntimeState});
        auto& state = *impl_;
        for (unsigned consumed = 0; consumed != 8; ++consumed)
        {
            const auto status = state.closed ? ipc::EIpcReceive::CLOSED : state.requester.tryReceive(output.backing_);
            if (status == ipc::EIpcReceive::EMPTY) return ERenderReceive::EMPTY;
            ReplyMetadata metadata;
            RenderError failure;
            if (status == ipc::EIpcReceive::CLOSED)
            {
                state.closed = true;
                if (state.requests.empty()) return ERenderReceive::CLOSED;
                const auto& pending = *state.requests.begin();
                metadata = {pending.session, pending.id, ECommandOutcome::OUTCOME_UNKNOWN};
                failure = {kRuntimeState};
            }
            else if (output.backing_.delivery == ipc::EIpcDelivery::ABORTED)
            {
                const auto original = output.backing_.original_request.metadata<RequestMetadata>();
                metadata = {original.session, original.request, ECommandOutcome::OUTCOME_UNKNOWN};
                failure = {kRuntimeState};
            }
            else
            {
                metadata = output.backing_.message.metadata<ReplyMetadata>();
            }
            auto* found = state.requests.find(metadata.request);
            if (!found)
            {
                output = {};
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
            const auto pending = *found;
            state.requests.erase(metadata.request);
            if (pending.abandoned) { output = {}; continue; }
            output.header = {metadata.request, metadata.outcome, failure};
            output.session = metadata.session;
            output.command_ = pending.command;
            const bool is_remote_reply = status == ipc::EIpcReceive::RECEIVED &&
                output.backing_.delivery != ipc::EIpcDelivery::ABORTED;
            if (is_remote_reply)
            {
                const bool is_wrong_session = !pending.command.isNull() && metadata.session != pending.session;
                const bool is_valid_outcome = metadata.outcome == ECommandOutcome::SUCCESS ||
                    metadata.outcome == ECommandOutcome::FAILED || metadata.outcome == ECommandOutcome::CANCELLED ||
                    metadata.outcome == ECommandOutcome::OUTCOME_UNKNOWN;
                if (is_wrong_session || !is_valid_outcome)
                {
                    output.header = {metadata.request, ECommandOutcome::FAILED,
                        {is_wrong_session ? kRuntimeIdentity : kProtocolContract}};
                    return ERenderReceive::RECEIVED;
                }
            }
            if (metadata.outcome == ECommandOutcome::SUCCESS)
            {
                if (pending.command.isNull())
                {
                    const auto* actual = output.backing_.message.operations();
                    auto qualified = actual ? ipc::IpcMessage::bindType<ValueEnvelope<HelloResponse>>(*actual) :
                        ipc::Result<ipc::IpcBoundType<ValueEnvelope<HelloResponse>>>(
                            cxx::unexpected(ipc::EIpcError::INVALID_ARGUMENT));
                    if (!qualified)
                    {
                        output.header.outcome = ECommandOutcome::FAILED;
                        output.header.error = {kProtocolContract};
                        return ERenderReceive::RECEIVED;
                    }
                    const auto& hello = output.backing_.message.get<ValueEnvelope<HelloResponse>>().value;
                    auto failure_type = hello.failure_ops ? ipc::IpcMessage::bindType<ValueEnvelope<RenderError>>(
                        *hello.failure_ops, hello.failure_code_pin) :
                        ipc::Result<ipc::IpcBoundType<ValueEnvelope<RenderError>>>(
                            cxx::unexpected(ipc::EIpcError::INVALID_ARGUMENT));
                    if (!failure_type)
                    {
                        output.header = {metadata.request, ECommandOutcome::FAILED, {kProtocolContract}};
                        return ERenderReceive::RECEIVED;
                    }
                    // One cold copy owns names and module pins; no Request/Response business data is copied.
                    state.manifest = hello.capabilities;
                    for (std::size_t index = 0; index != state.manifest.size(); ++index)
                    {
                        const auto& capability = state.manifest[index];
                        const bool is_invalid = capability.id.isNull() || !capability.request_ops ||
                            !capability.response_ops || !validateCommand(capability.descriptor());
                        bool is_duplicate = false;
                        for (std::size_t previous = 0; previous != index; ++previous)
                        {
                            const auto& earlier = state.manifest[previous];
                            if (earlier.id == capability.id || earlier.name == capability.name)
                            {
                                is_duplicate = true;
                                break;
                            }
                        }
                        if (is_invalid || is_duplicate)
                        {
                            state.manifest.clear();
                            output.header.outcome = ECommandOutcome::FAILED;
                            output.header.error = {kProtocolContract};
                            return ERenderReceive::RECEIVED;
                        }
                    }
                    state.session = metadata.session;
                    state.failure_type = *failure_type;
                    state.bound = true;
                }
                else
                {
                    const bool is_wrong_type = output.backing_.message.operations() != pending.response;
                    if (is_wrong_type)
                        output.header = {metadata.request, ECommandOutcome::FAILED, {kProtocolContract}};
                }
            }
            else if (!failure.type && status == ipc::EIpcReceive::RECEIVED)
            {
                const auto* actual = output.backing_.message.operations();
                const bool qualified = state.failure_type ? actual == &state.failure_type->operations() :
                    (actual && bool(ipc::IpcMessage::bindType<ValueEnvelope<RenderError>>(*actual)));
                if (qualified) output.header.error = output.backing_.message.get<ValueEnvelope<RenderError>>().value;
                else output.header = {metadata.request, ECommandOutcome::FAILED, {kProtocolContract}};
            }
            return ERenderReceive::RECEIVED;
        }
        return ERenderReceive::EMPTY;
    }

    RenderResult<void> RenderClient::abandon(RequestId request) noexcept
    {
        auto* pending = impl_->requests.find(request);
        if (!pending || pending->abandoned) return cxx::unexpected(RenderError{kRuntimeIdentity});
        pending->abandoned = true;
        return {};
    }

    std::size_t RenderClient::retainedBytes() const noexcept
    {
        return impl_->budget->retained.load(std::memory_order_relaxed);
    }
}
