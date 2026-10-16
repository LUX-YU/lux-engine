#include "Request.hpp"
#include "Deferred.hpp"

namespace lux::render
{
    namespace ipc = platform::ipc;
    using namespace runtime_detail;

    RenderRequest::RenderRequest(ipc::ReceivedRequest input, ipc::IpcResponder& responder,
        SessionIdentity session, RequestId request, std::size_t capacity, const CommandDescriptor* descriptor,
        const CommandTransport* transport, const ipc::IpcBoundType<ValueEnvelope<RenderError>>& failure_type,
        std::shared_ptr<const void> code_pin) noexcept
        : code_pin_(std::move(code_pin)), input_(std::move(input)), responder_(&responder), session_(session),
          request_(request), reply_capacity_(capacity), descriptor_(descriptor), transport_(transport),
          failure_type_(&failure_type) {}

    BudgetLease RenderRequest::splitBudget(std::size_t bytes) noexcept
    {
        if (!transport_) return {}; // An untrusted raw peer has no Client budget credential.
        // ReceivedRequest owns a non-const message; only its public inspection interface is const.
        auto& input = const_cast<ipc::IpcMessage&>(input_.message());
        return transport_->request_budget(input).split(bytes);
    }

    RenderResult<std::size_t> RenderRequest::prepareResponse(void* response, ipc::IpcMessage& output) noexcept
    {
        if (!active() || unreplied_) return cxx::unexpected(RenderError{kRuntimeState});
        auto bytes = transport_->response_bytes(response);
        if (!bytes) return cxx::unexpected(bytes.error());
        if (*bytes > reply_capacity_) return cxx::unexpected(RenderError{kRuntimeCapacity});
        const auto overhead = transport_->response->size() - descriptor_->response.size;
        transport_->package_response(output, response, {}, code_pin_);
        return *bytes + overhead; // Client checked this bounded sum before moving the input.
    }

    RenderResult<void> RenderRequest::complete(void* response) noexcept
    {
        if (gpuReady()) return cxx::unexpected(RenderError{kProtocolContract});
        ipc::IpcMessage output;
        auto bytes = prepareResponse(response, output);
        if (!bytes) return cxx::unexpected(bytes.error());
        return publishPrepared(output, *bytes);
    }

    RenderResult<void> RenderRequest::publishPrepared(ipc::IpcMessage& message, std::size_t charge) noexcept
    {
        transport_->response_budget(message) = splitBudget(charge);
        return publish(message, ECommandOutcome::SUCCESS);
    }

    RenderResult<void> RenderRequest::publishError(ECommandOutcome outcome, RenderError error) noexcept
    {
        if (!active() || unreplied_) return cxx::unexpected(RenderError{kRuntimeState});
        ipc::IpcMessage message;
        message.emplace(*failure_type_, splitBudget(sizeof(ValueEnvelope<RenderError>)), std::move(error));
        return publish(message, outcome);
    }

    RenderResult<void> RenderRequest::fail(RenderError error) noexcept
    {
        return publishError(ECommandOutcome::FAILED, error);
    }

    RenderResult<void> RenderRequest::cancel() noexcept
    {
        return publishError(ECommandOutcome::CANCELLED, {});
    }

    RenderResult<void> RenderRequest::unknown() noexcept
    {
        return publishError(ECommandOutcome::OUTCOME_UNKNOWN, {});
    }

    RenderResult<void> RenderRequest::publish(ipc::IpcMessage& message, ECommandOutcome outcome) noexcept
    {
        message.setMetadata(ReplyMetadata{session_, request_, outcome});
        if (!responder_->tryReply(input_, message))
        {
            unreplied_ = std::make_unique<ipc::IpcMessage>(std::move(message));
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        return {};
    }

    CommandInvocation RenderRequest::invocation(GpuAdmission& admission, void* extension) noexcept
    {
        static constexpr CommandCompletionFns completion{
            [](void* owner, ECommandOutcome outcome, RenderError error, void* response) noexcept {
                auto& request = static_cast<GpuAdmission*>(owner)->request;
                switch (outcome)
                {
                case ECommandOutcome::SUCCESS: return request.complete(response);
                case ECommandOutcome::FAILED: return request.fail(error);
                case ECommandOutcome::CANCELLED: return request.cancel();
                default: return request.unknown();
                }
            },
            [](const void* owner) noexcept { return static_cast<const GpuAdmission*>(owner)->request.active(); },
            [](void* owner, void* sender, const CommandAsyncOps& operations) noexcept -> RenderResult<void> {
                auto& admission = *static_cast<GpuAdmission*>(owner);
                return admission.defer(admission.server, admission.request, sender, operations);
            }
        };
        return {transport_->request_value(input_.message()), &admission, &completion, extension};
    }
}

namespace lux::render::runtime_detail
{
    const vulkan::GpuCompletionFns gpu_completion_functions{
        [](void* owner, vulkan::SubmissionQueue& queue, void* response) noexcept -> RenderResult<void*> {
            auto& admission = *static_cast<GpuAdmission*>(owner);
            auto& native = admission.native;
            const bool is_session_queue = &queue == &native.queue(vulkan::EQueueRole::GRAPHICS) ||
                &queue == &native.queue(vulkan::EQueueRole::COMPUTE) ||
                &queue == &native.queue(vulkan::EQueueRole::TRANSFER);
            const bool invalid = !is_session_queue || admission.reserved || !admission.request.gpuReady();
            if (invalid) return cxx::unexpected(RenderError{kProtocolContract});
            if (admission.slots.size() >= admission.slot_limit)
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            platform::ipc::IpcMessage output;
            auto charge = admission.request.prepareResponse(response, output);
            if (!charge) return cxx::unexpected(charge.error());
            const auto id = admission.slots.tryEmplacePrepared();
            if (!id) return cxx::unexpected(RenderError{kRuntimeCapacity});
            auto& slot = *admission.slots.find(*id);
            slot.id = *id;
            slot.session = admission.session;
            slot.events = &admission.events;
            slot.publishers = &admission.publishers;
            slot.owner = &admission.slots;
            auto& gpu = slot.condition.emplace<GpuCompletion>();
            gpu.queue = &queue;
            gpu.submitted_before_reserve = queue.submitted();
            gpu.reply = std::move(output);
            gpu.reply_charge = *charge;
            gpu.reserved_request = &admission.request;
            gpu.native = &native;
            gpu.implementation_pin = &admission.implementation_pin;
            admission.reserved = true;
            return &slot;
        },
        [](void* value, void* response) noexcept -> RenderResult<void> {
            auto& gpu = std::get<GpuCompletion>(static_cast<DeferredCompletion*>(value)->condition);
            platform::ipc::IpcMessage output;
            auto charge = gpu.reserved_request->prepareResponse(response, output);
            if (!charge) return cxx::unexpected(charge.error());
            gpu.reply = std::move(output);
            gpu.reply_charge = *charge;
            return {};
        },
        [](void* value, vulkan::GpuWork& work) noexcept -> RenderResult<void> {
            auto& slot = *static_cast<DeferredCompletion*>(value);
            auto& gpu = std::get<GpuCompletion>(slot.condition);
            if (!work.unstartedIn(*gpu.native)) return cxx::unexpected(RenderError{kProtocolContract});
            work.setCompletionPost(slot.post(), *gpu.implementation_pin, gpu.reserved_request->codePin());
            gpu.work = std::move(work);
            slot.request.emplace(std::move(*gpu.reserved_request));
            gpu.reserved_request = nullptr;
            return {};
        },
        [](void* value, const vulkan::SubmissionTicket& ticket, void* owner, void (*destroy)(void*) noexcept,
            RenderResult<void> (*ready)(void*, vulkan::DeviceSession&) noexcept) noexcept -> RenderResult<void> {
            auto& slot = *static_cast<DeferredCompletion*>(value);
            auto& gpu = std::get<GpuCompletion>(slot.condition);
            const bool invalid = !gpu.queue->owns(ticket) || ticket.serial() <= gpu.submitted_before_reserve;
            if (invalid) return cxx::unexpected(RenderError{kProtocolContract});
            gpu.work = vulkan::GpuWork::accepted(*gpu.native, ticket, owner, destroy, ready);
            slot.request.emplace(std::move(*gpu.reserved_request));
            gpu.reserved_request = nullptr;
            return {};
        },
        [](void* value) noexcept {
            auto& slot = *static_cast<DeferredCompletion*>(value);
            auto* owner = slot.owner;
            const auto id = slot.id;
            if (!owner->erase(id)) std::terminate();
        }
    };
}
