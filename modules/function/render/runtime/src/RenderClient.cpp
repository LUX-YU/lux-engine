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

        void refund(void* owner, std::size_t bytes) noexcept
        {
            static_cast<Budget*>(owner)->retained.fetch_sub(bytes, std::memory_order_relaxed);
        }

        struct Input final
        {
            std::vector<std::byte> header;
            std::array<RenderPayload, ipc::IpcMessage::attachment_capacity> payloads;
            std::array<std::span<const std::byte>, ipc::IpcMessage::attachment_capacity> views;
            std::shared_ptr<Budget> budget;
            std::size_t charged = 0;
            ~Input() noexcept
            {
                payloads = {};
                std::vector<std::byte>().swap(header);
                if (charged) refund(budget.get(), charged);
            }
        };

        struct Pending final
        {
            RequestId id;
            SessionIdentity session{};
            std::optional<DataSchemaDescriptor> reply;
            std::size_t small_charge = 0;
            bool hello = false, abandoned = false;
        };
    }

    struct RenderClient::Impl final
    {
        [[nodiscard]] RenderResult<RequestId> hello() noexcept;
        [[nodiscard]] RenderResult<OperationBinding> bind(const OperationDescriptor& descriptor) const noexcept;
        [[nodiscard]] RenderResult<RequestId> submitEncoded(const OperationBinding&, std::size_t, const void*,
            RenderResult<void> (*)(const void*, cxx::BinarySpanWriter&) noexcept,
            std::span<const RenderPayload>, std::size_t) noexcept;
        [[nodiscard]] RenderResult<ERenderReceive> tryReceive(RenderCompletion&) noexcept;
        [[nodiscard]] RenderResult<void> abandon(RequestId) noexcept;
        [[nodiscard]] std::size_t retainedBytes() const noexcept;
        ipc::IpcRequester requester;
        cxx::SlotMap<Pending, RequestTag> requests;
        std::shared_ptr<Budget> budget;
        OwnedWireBytes manifest;
        SessionIdentity session{};
        std::size_t capacity;
        bool bound = false, ready_sent = false, closed = false;
        Impl(ipc::IpcRequester input, RenderClientConfig config) : requester(std::move(input)),
            budget(std::make_shared<Budget>(config.retained_byte_limit)), capacity(config.ring_capacity)
        {
            requests.reserve(capacity);
        }
        ~Impl() noexcept
        {
            requester.reset();
            for (const auto& request : requests) refund(budget.get(), request.small_charge);
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


    RenderClient::RenderClient(std::unique_ptr<Impl> state) noexcept : impl_(std::move(state)) {}
    RenderClient::~RenderClient() noexcept = default;

    RenderResult<std::unique_ptr<RenderClient>> RenderClient::create(
        std::string_view name, RenderClientConfig config
    ) noexcept
    {
        if (!config.retained_byte_limit) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto requester = ipc::IpcCore::instance().createRequester(name, {config.ring_capacity, config.ring_capacity});
        if (!requester) return cxx::unexpected(RenderError{kRuntimeState});
        return std::unique_ptr<RenderClient>(new RenderClient(std::make_unique<Impl>(std::move(*requester), config)));
    }

    SessionIdentity RenderClient::session() const noexcept { return impl_->session; }

    RenderResult<OperationBinding> RenderClient::Impl::bind(const OperationDescriptor& descriptor) const noexcept
    {
        if (!bound) return cxx::unexpected(RenderError{kRuntimeState});
        if (auto valid = validateOperation(descriptor); !valid) return cxx::unexpected(valid.error());
        auto id = bindCapability(manifest.view(), descriptor);
        if (!id) return cxx::unexpected(id.error());
        return OperationBinding{session, *id, descriptor};
    }

    RenderResult<RequestId> RenderClient::Impl::hello() noexcept
    {
        if (ready_sent) return cxx::unexpected(RenderError{kRuntimeState});
        auto result = submitEncoded({}, 0, nullptr,
            [](const void*, cxx::BinarySpanWriter&) noexcept -> RenderResult<void> { return {}; }, {}, 64 * 1024);
        if (result) ready_sent = true;
        return result;
    }

    RenderResult<RequestId> RenderClient::Impl::submitEncoded(const OperationBinding& binding, std::size_t size,
        const void* value, RenderResult<void> (*encode)(const void*, cxx::BinarySpanWriter&) noexcept,
        std::span<const RenderPayload> attachments, std::size_t reply_capacity) noexcept
    {
        auto& s = *this;
        const bool hello = binding.operation.isNull();
        const bool invalid_state = s.closed || (hello ? s.ready_sent : !s.bound);
        if (invalid_state) return cxx::unexpected(RenderError{kRuntimeState});
        const bool invalid_capacity = s.requests.size() == s.capacity ||
            attachments.size() > ipc::IpcMessage::attachment_capacity ||
            size > wire_limits.max_wire_bytes - request_size || reply_capacity < reply_size;
        if (invalid_capacity) return cxx::unexpected(RenderError{kRuntimeCapacity});
        const auto total = request_size + size;
        const bool small = total <= ipc::IpcMessage::inline_capacity && attachments.empty() && reply_capacity <= 128;
        std::size_t charged = total;
        const auto add = [&](std::size_t bytes) {
            if (bytes > s.budget->limit || charged > s.budget->limit - bytes) return false;
            charged += bytes;
            return true;
        };
        if (!add(reply_capacity) || (!small && !add(sizeof(Input))))
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        for (const auto& payload : attachments)
            if (!add(payload.retainedBytes())) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto current = s.budget->retained.load(std::memory_order_relaxed);
        do
        {
            if (current > s.budget->limit - charged) return cxx::unexpected(RenderError{kRuntimeCapacity});
        } while (!s.budget->retained.compare_exchange_weak(current, current + charged, std::memory_order_relaxed));

        std::array<std::byte, 128> local;
        std::unique_ptr<Input> input;
        std::span<std::byte> storage(local.data(), small ? total : 0);
        std::array<std::span<const std::byte>, ipc::IpcMessage::attachment_capacity> views;
        for (std::size_t i = 0; i < attachments.size(); ++i) views[i] = attachments[i].bytes().view();
        if (!small)
        {
            input = std::make_unique<Input>();
            input->budget = s.budget;
            input->charged = charged;
            input->header.resize(total);
            storage = input->header;
            for (std::size_t i = 0; i < attachments.size(); ++i)
            {
                input->payloads[i] = attachments[i];
                input->views[i] = views[i];
            }
        }
        const auto request = s.requests.emplace(Pending{{}, binding.session, binding.descriptor_.reply,
            small ? charged : 0, hello});
        s.requests.find(request)->id = request;
        const auto rollback = [&] {
            s.requests.erase(request);
            if (small) refund(s.budget.get(), charged);
        };
        cxx::BinarySpanWriter writer(storage);
        writeRequest(writer, {binding.session, request, binding.operation, reply_capacity}, static_cast<std::uint32_t>(size));
        auto encoded = encode(value, writer);
        if (!encoded || !writer.good() || writer.size() != total)
        {
            rollback();
            return cxx::unexpected(encoded ? RenderError{kInvalidWire} : encoded.error());
        }
        if (!hello)
        {
            auto valid = validateBoundWire(BorrowedWireView{binding.descriptor_.payload, storage.subspan(request_size),
                std::span(views).first(attachments.size())}, wire_limits);
            if (!valid) { rollback(); return cxx::unexpected(valid.error()); }
        }
        ipc::IpcMessage message;
        if (small) { if (!message.tryAssignInline(storage)) std::terminate(); }
        else if (!message.tryAdopt(input, input->header, std::span(input->views).first(attachments.size())))
            std::terminate();
        if (auto sent = s.requester.trySend(message); !sent)
        {
            rollback();
            return cxx::unexpected(RenderError{sent.error() == ipc::EIpcError::WOULD_BLOCK ?
                kRuntimeCapacity : kRuntimeState});
        }
        return request;
    }

    RenderCompletion::~RenderCompletion() noexcept
    {
        backing_ = {};
        if (charged_) refund_(budget_.get(), charged_);
    }

    RenderCompletion::RenderCompletion(RenderCompletion&& other) noexcept { *this = std::move(other); }

    RenderCompletion& RenderCompletion::operator=(RenderCompletion&& other) noexcept
    {
        if (this == &other) return *this;
        backing_ = {};
        if (charged_) refund_(budget_.get(), charged_);
        header = std::exchange(other.header, {});
        session = other.session;
        schema = other.schema;
        backing_ = std::move(other.backing_);
        budget_ = std::move(other.budget_);
        refund_ = other.refund_;
        charged_ = std::exchange(other.charged_, 0);
        payload_size_ = std::exchange(other.payload_size_, 0);
        return *this;
    }

    std::span<const std::byte> RenderCompletion::bytes() const noexcept
    {
        return payload_size_ ? backing_.message.bytes().subspan(reply_size, payload_size_) : std::span<const std::byte>{};
    }

    std::span<const std::span<const std::byte>> RenderCompletion::attachments() const noexcept
    {
        return backing_.message.attachments();
    }

    RenderResult<ERenderReceive> RenderClient::Impl::tryReceive(RenderCompletion& output) noexcept
    {
        if (output.active()) return cxx::unexpected(RenderError{kRuntimeState});
        auto& s = *this;
        for (unsigned i = 0; i < 8; ++i)
        {
            const auto status = s.closed ? ipc::EIpcReceive::CLOSED : s.requester.tryReceive(output.backing_);
            if (status == ipc::EIpcReceive::EMPTY) return ERenderReceive::EMPTY;
            RequestId id;
            ReplyFrame frame;
            if (status == ipc::EIpcReceive::CLOSED)
            {
                s.closed = true;
                if (s.requests.empty()) return ERenderReceive::CLOSED;
                id = s.requests.begin()->id;
                frame.header = {id, EOperationOutcome::OUTCOME_UNKNOWN, {kRuntimeState}};
            }
            else if (output.backing_.delivery == ipc::EIpcDelivery::ABORTED)
            {
                auto original = readRequest(output.backing_.original_request.bytes());
                if (!original) { output = {}; s.closed = true; return cxx::unexpected(original.error()); }
                id = original->request;
                frame.header = {id, EOperationOutcome::OUTCOME_UNKNOWN, {kRuntimeState}};
            }
            else
            {
                auto decoded = readReply(output.backing_.message);
                if (!decoded) { output = {}; s.closed = true; return cxx::unexpected(decoded.error()); }
                frame = *decoded;
                id = frame.header.request;
            }
            auto* found = s.requests.find(id);
            if (!found) { output = {}; return cxx::unexpected(RenderError{kRuntimeIdentity}); }
            const auto pending = *found;
            s.requests.erase(id);
            output.budget_ = s.budget;
            output.refund_ = refund;
            output.charged_ = pending.small_charge;
            if (pending.abandoned) { output = {}; continue; }
            output.header = frame.header;
            output.session = frame.session;
            output.schema = pending.reply;
            output.payload_size_ = frame.payload.size();
            if (frame.header.outcome == EOperationOutcome::SUCCESS)
            {
                if (pending.hello)
                {
                    s.manifest = own(std::vector<std::byte>(frame.payload.begin(), frame.payload.end()));
                    s.session = frame.session;
                    s.bound = true;
                }
                else if (frame.session != pending.session)
                    output.header = {id, EOperationOutcome::FAILED, {kRuntimeIdentity}};
                else if (pending.reply)
                {
                    auto valid = validateBoundWire(BorrowedWireView{*pending.reply, frame.payload,
                        output.backing_.message.attachments()}, wire_limits);
                    if (!valid) output.header = {id, EOperationOutcome::FAILED, valid.error()};
                }
                else if (!frame.payload.empty()) output.header = {id, EOperationOutcome::FAILED, {kProtocolContract}};
            }
            return ERenderReceive::RECEIVED;
        }
        return ERenderReceive::EMPTY;
    }

    RenderResult<void> RenderClient::Impl::abandon(RequestId request) noexcept
    {
        auto* pending = requests.find(request);
        if (!pending || pending->abandoned) return cxx::unexpected(RenderError{kRuntimeIdentity});
        pending->abandoned = true;
        return {};
    }

    std::size_t RenderClient::Impl::retainedBytes() const noexcept
    {
        return budget->retained.load(std::memory_order_relaxed);
    }

    RenderResult<RequestId> RenderClient::hello() noexcept { return impl_->hello(); }

    RenderResult<OperationBinding> RenderClient::bind(const OperationDescriptor& descriptor) const noexcept
    {
        return impl_->bind(descriptor);
    }

    RenderResult<RequestId> RenderClient::submitEncoded(const OperationBinding& operation, std::size_t size,
        const void* value, RenderResult<void> (*encode)(const void*, cxx::BinarySpanWriter&) noexcept,
        std::span<const RenderPayload> attachments, std::size_t reply_capacity) noexcept
    {
        return impl_->submitEncoded(operation, size, value, encode, attachments, reply_capacity);
    }

    RenderResult<ERenderReceive> RenderClient::tryReceive(RenderCompletion& output) noexcept
    {
        return impl_->tryReceive(output);
    }

    RenderResult<void> RenderClient::abandon(RequestId request) noexcept { return impl_->abandon(request); }

    std::size_t RenderClient::retainedBytes() const noexcept { return impl_->retainedBytes(); }
}
