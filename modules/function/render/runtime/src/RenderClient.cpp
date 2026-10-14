#include "Wire.hpp"
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <atomic>

namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    namespace
    {
        struct Budget final
        {
            std::atomic<std::size_t> retained{0};
            std::size_t limit;
            explicit Budget(std::size_t value) : limit(value) {}
        };

        struct Input final
        {
            OwnedWireBytes header;
            std::array<RenderPayload, ipc::IpcMessage::attachment_capacity> payloads;
            std::array<std::span<const std::byte>, ipc::IpcMessage::attachment_capacity> views;
            std::shared_ptr<Budget> budget;
            std::size_t charged = 0;
            ~Input() noexcept
            {
                payloads = {};
                header = {};
                if (charged) budget->retained.fetch_sub(charged, std::memory_order_relaxed);
            }
        };

        struct Pending final
        {
            ERenderCommand operation;
            RequestId id;
            bool abandoned = false;
        };
    }

    struct RenderClient::State final
    {
        ipc::IpcRequester requester;
        cxx::SlotMap<Pending, RequestTag> requests;
        std::shared_ptr<Budget> budget;
        std::array<OperationId, operation_count> operations{};
        std::size_t capacity;
        bool bound = false, ready_sent = false, closed = false;
        State(ipc::IpcRequester input, RenderClientConfig config) : requester(std::move(input)),
            budget(std::make_shared<Budget>(config.retained_byte_limit)), capacity(config.ring_capacity)
        {
            requests.reserve(capacity);
        }
    };

    RenderResult<RenderPayload> RenderPayload::fromVector(
        std::shared_ptr<const std::vector<std::byte>> owner, std::size_t offset, std::size_t size
    ) noexcept
    {
        const bool invalid = !owner || !size || offset > owner->size() || size > owner->size() - offset;
        if (invalid) return cxx::unexpected(RenderError{kInvalidDataReference});
        RenderPayload result;
        result.retained_bytes_ = owner->capacity();
        result.bytes_ = OwnedWireBytes::fromOwner(owner, std::span(*owner).subspan(offset, size));
        return result;
    }

    RenderClient::RenderClient(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
    RenderClient::~RenderClient() noexcept = default;

    RenderResult<std::unique_ptr<RenderClient>> RenderClient::create(
        std::string_view name, RenderClientConfig config
    ) noexcept
    {
        if (!config.retained_byte_limit) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto requester = ipc::IpcCore::instance().createRequester(name, {config.ring_capacity, config.ring_capacity});
        if (!requester) return cxx::unexpected(RenderError{kRuntimeState});
        return std::unique_ptr<RenderClient>(new RenderClient(std::make_unique<State>(std::move(*requester), config)));
    }

    RenderResult<RequestId> RenderClient::submit(const RenderCommand& command) noexcept
    {
        auto& s = *state_;
        const auto opcode = static_cast<std::size_t>(command.operation);
        const bool is_ready = command.operation == ERenderCommand::READY;
        const bool invalid = opcode >= operation_count || s.closed || command.reply_capacity < record_size ||
            (is_ready ? s.ready_sent : !s.bound);
        if (invalid) return cxx::unexpected(RenderError{kRuntimeState});
        if (s.requests.size() == s.capacity || command.attachments.size() > ipc::IpcMessage::attachment_capacity)
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        Record record;
        record.session = command.session;
        record.operation = is_ready ? OperationId{} : s.operations[opcode];
        putKey(std::span(record.args).subspan(0), command.scene);
        putKey(std::span(record.args).subspan(2), command.view);
        putKey(std::span(record.args).subspan(4), command.target);
        putKey(std::span(record.args).subspan(6), command.effect);
        record.args[8] = command.description.width;
        record.args[9] = command.description.height;
        record.args[10] = static_cast<std::uint32_t>(command.description.format);
        record.args[11] = command.description.samples;
        record.args[12] = static_cast<std::uint32_t>(command.description.kind);
        record.color = command.color;
        record.reply_capacity = command.reply_capacity;
        auto body = selections(command.features);
        if (!body) return cxx::unexpected(body.error());
        record.body = own(std::move(*body));
        std::size_t charged = sizeof(Input) + record_size + record.body.size();
        const auto add = [&](std::size_t size) {
            if (size > s.budget->limit || charged > s.budget->limit - size) return false;
            charged += size;
            return true;
        };
        if (!add(command.reply_capacity)) return cxx::unexpected(RenderError{kRuntimeCapacity});
        for (const auto& payload : command.attachments)
            if (!add(payload.retainedBytes())) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto current = s.budget->retained.load(std::memory_order_relaxed);
        do
        {
            if (current > s.budget->limit - charged) return cxx::unexpected(RenderError{kRuntimeCapacity});
        } while (!s.budget->retained.compare_exchange_weak(current, current + charged, std::memory_order_relaxed));

        auto input = std::make_unique<Input>();
        input->budget = s.budget;
        input->charged = charged;
        record.request = s.requests.emplace(Pending{command.operation});
        s.requests.find(record.request)->id = record.request;
        auto encoded = encodeWire(record, {}, wire_limits);
        if (!encoded) { s.requests.erase(record.request); return cxx::unexpected(encoded.error()); }
        input->header = std::move(*encoded);
        for (std::size_t i = 0; i < command.attachments.size(); ++i)
        {
            input->payloads[i] = command.attachments[i];
            input->views[i] = input->payloads[i].bytes().view();
        }
        ipc::IpcMessage message;
        if (!message.tryAdopt(input, input->header.view(),
            std::span(input->views).first(command.attachments.size()))) std::terminate(); // established fixed bound + non-null unique owner
        if (auto result = s.requester.trySend(message); !result)
        {
            s.requests.erase(record.request);
            return cxx::unexpected(RenderError{result.error() == ipc::EIpcError::WOULD_BLOCK ?
                kRuntimeCapacity : kRuntimeState});
        }
        if (is_ready) s.ready_sent = true;
        return record.request;
    }

    RenderCompletion::RenderCompletion(RenderCompletion&& other) noexcept { *this = std::move(other); }

    RenderCompletion& RenderCompletion::operator=(RenderCompletion&& other) noexcept
    {
        if (this == &other) return *this;
        header = std::exchange(other.header, {});
        session = other.session;
        scene = other.scene;
        view = other.view;
        target = other.target;
        effect = other.effect;
        target_epoch = other.target_epoch;
        backing_ = std::move(other.backing_);
        return *this;
    }

    std::span<const std::byte> RenderCompletion::bytes() const noexcept
    {
        const auto attachments = backing_.message.attachments();
        return attachments.empty() ? backing_.message.bytes() : attachments[0];
    }

    RenderResult<ERenderReceive> RenderClient::tryReceive(RenderCompletion& output) noexcept
    {
        if (output.active()) return cxx::unexpected(RenderError{kRuntimeState});
        auto& s = *state_;
        for (unsigned i = 0; i < 8; ++i)
        {
            const auto status = s.closed ? ipc::EIpcReceive::CLOSED : s.requester.tryReceive(output.backing_);
            if (status == ipc::EIpcReceive::EMPTY) return ERenderReceive::EMPTY;
            if (status == ipc::EIpcReceive::CLOSED)
            {
                s.closed = true;
                if (s.requests.empty()) return ERenderReceive::CLOSED;
                const auto pending = *s.requests.begin();
                s.requests.erase(pending.id);
                if (pending.abandoned) continue;
                output.header = {pending.id, EOperationOutcome::OUTCOME_UNKNOWN, {kRuntimeState}};
                return ERenderReceive::RECEIVED;
            }
            const bool aborted = output.backing_.delivery == ipc::EIpcDelivery::ABORTED;
            const auto& message = aborted ? output.backing_.original_request : output.backing_.message;
            std::span<const std::byte> body;
            auto record = decodeRecord(message.bytes(), body);
            if (!record)
            {
                output = {};
                s.closed = true;
                return cxx::unexpected(record.error());
            }
            auto* pending = s.requests.find(record->request);
            if (!pending)
            {
                output = {};
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
            const auto metadata = *pending;
            s.requests.erase(record->request);
            if (metadata.abandoned) { output = {}; continue; }
            output.header = {record->request, aborted ? EOperationOutcome::OUTCOME_UNKNOWN :
                static_cast<EOperationOutcome>(record->args[13]), record->error};
            if (record->args[13] > static_cast<unsigned>(EOperationOutcome::OUTCOME_UNKNOWN))
                output.header = {record->request, EOperationOutcome::FAILED, {kProtocolContract}};
            output.session = record->session;
            output.scene = key<SceneId>(std::span(record->args).subspan(0));
            output.view = key<ViewId>(std::span(record->args).subspan(2));
            output.target = key<TargetId>(std::span(record->args).subspan(4));
            output.effect = key<EffectId>(std::span(record->args).subspan(6));
            output.target_epoch = std::uint64_t(record->args[14]) | (std::uint64_t(record->args[15]) << 32);
            if (metadata.operation == ERenderCommand::READY && output.header.outcome == EOperationOutcome::SUCCESS)
            {
                auto bound = bindCapabilities(body);
                if (!bound) output.header = {record->request, EOperationOutcome::FAILED, bound.error()};
                else { s.operations = *bound; s.bound = true; }
            }
            return ERenderReceive::RECEIVED;
        }
        return ERenderReceive::EMPTY;
    }

    RenderResult<void> RenderClient::abandon(RequestId request) noexcept
    {
        auto* pending = state_->requests.find(request);
        if (!pending || pending->abandoned) return cxx::unexpected(RenderError{kRuntimeIdentity});
        pending->abandoned = true;
        return {};
    }

    std::size_t RenderClient::retainedBytes() const noexcept
    {
        return state_->budget->retained.load(std::memory_order_relaxed);
    }
}
