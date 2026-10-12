#include "RuntimeChannel.hpp"

namespace lux::render
{
    RuntimeRequest::RuntimeRequest(RenderReplyInbox inbox, RenderReplyTicket<RuntimeReply> ticket) noexcept
        : inbox_(std::move(inbox)), ticket_(ticket)
    {
    }

    RuntimeRequest::~RuntimeRequest() noexcept
    {
        if (ticket_ && !inbox_.abandon(*ticket_))
        {
            std::terminate();
        }
    }

    RuntimeRequest::RuntimeRequest(RuntimeRequest&& other) noexcept
        : inbox_(std::move(other.inbox_)), ticket_(std::exchange(other.ticket_, std::nullopt))
    {
    }

    RuntimeRequest& RuntimeRequest::operator=(RuntimeRequest&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        if (ticket_ && !inbox_.abandon(*ticket_))
        {
            std::terminate();
        }
        inbox_ = std::move(other.inbox_);
        ticket_ = std::exchange(other.ticket_, std::nullopt);
        return *this;
    }

    RenderResult<std::optional<RuntimeReply>> RuntimeRequest::poll() noexcept
    {
        if (!ticket_)
        {
            return cxx::unexpected(RenderError{kTransportReply});
        }
        auto result = inbox_.poll(*ticket_);
        if ((result && result->has_value()) || (!result && result.error().type != kTransportBusy))
        {
            ticket_.reset();
        }
        return result;
    }

    RenderResult<void> RuntimeRequest::abandon() noexcept
    {
        if (!ticket_)
        {
            return cxx::unexpected(RenderError{kTransportReply});
        }
        auto result = inbox_.abandon(*ticket_);
        if (result)
        {
            ticket_.reset();
        }
        return result;
    }

    RenderRuntime::RenderRuntime(std::shared_ptr<detail::RuntimeChannel> channel) noexcept
        : channel_(std::move(channel)), program_(channel_->transport->makeProgramPacket()),
          controls_(channel_->transport->makeControlPacket()), owner_thread_(detail::transportThreadIdentity())
    {
    }

    RenderRuntime::~RenderRuntime() noexcept
    {
        requestStop();
    }

    RenderResult<RuntimeRequest> RenderRuntime::control(RuntimeControl request) noexcept
    {
        if (owner_thread_ != detail::transportThreadIdentity())
        {
            return cxx::unexpected(RenderError{kTransportWrongThread});
        }
        auto ticket = controls_.request(channel_->control, request);
        if (!ticket)
        {
            return cxx::unexpected(ticket.error());
        }
        RuntimeRequest result{channel_->transport->replyInbox(), *ticket};
        auto submitted = channel_->transport->submit(controls_);
        if (!submitted)
        {
            if (!controls_.reset())
            {
                std::terminate();
            }
            return cxx::unexpected(submitted.error());
        }
        channel_->signal();
        return result;
    }

    namespace
    {
        RuntimeControl viewControl(RuntimeView view, ERuntimeControl operation) noexcept
        {
            RuntimeControl request;
            request.owner = view.owner;
            request.scene_index = view.local.scene.index;
            request.scene_generation = view.local.scene.gen;
            request.index = view.local.view.index;
            request.generation = view.local.view.gen;
            request.operation = operation;
            return request;
        }

        template <typename Key>
        RuntimeControl objectControl(RuntimeAddress<Key> object, ERuntimeControl operation) noexcept
        {
            RuntimeControl request;
            request.owner = object.owner;
            request.index = object.local.index;
            request.generation = object.local.gen;
            request.operation = operation;
            return request;
        }
    } // namespace

    RenderResult<RuntimeRequest> RenderRuntime::createScene() noexcept
    {
        RuntimeControl request;
        request.owner = channel_->owner;
        request.operation = ERuntimeControl::CREATE_SCENE;
        return control(request);
    }

    RenderResult<RuntimeRequest> RenderRuntime::createTarget(std::uint32_t width, std::uint32_t height) noexcept
    {
        RuntimeControl request;
        request.owner = channel_->owner;
        request.operation = ERuntimeControl::CREATE_TARGET;
        request.first = width;
        request.second = height;
        return control(request);
    }

    RenderResult<RuntimeRequest> RenderRuntime::createView(RuntimeScene scene) noexcept
    {
        RuntimeControl request;
        request.owner = scene.owner;
        request.scene_index = scene.local.index;
        request.scene_generation = scene.local.gen;
        request.operation = ERuntimeControl::CREATE_VIEW;
        return control(request);
    }

    RenderResult<RuntimeRequest> RenderRuntime::bindingControl(
        RuntimeBinding binding,
        ERuntimeControl operation,
        RuntimeControl request
    ) noexcept
    {
        const bool wrong_owner =
            binding.effect.owner != binding.view.owner || binding.target.owner != binding.view.owner;
        if (wrong_owner)
        {
            return cxx::unexpected(RenderError{kTransportWrongOwner});
        }
        request.owner = binding.view.owner;
        request.scene_index = binding.view.local.scene.index;
        request.scene_generation = binding.view.local.scene.gen;
        request.index = binding.view.local.view.index;
        request.generation = binding.view.local.view.gen;
        request.effect_index = binding.effect.local.index;
        request.effect_generation = binding.effect.local.gen;
        request.target_index = binding.target.local.index;
        request.target_generation = binding.target.local.gen;
        request.operation = operation;
        return control(request);
    }

    RenderResult<RuntimeRequest> RenderRuntime::attachEffect(
        RuntimeEffect effect,
        RuntimeView view,
        RuntimeTarget target
    ) noexcept
    {
        return bindingControl({effect, view, target}, ERuntimeControl::ATTACH_EFFECT);
    }

    RenderResult<RuntimeRequest> RenderRuntime::detachEffect(
        RuntimeEffect effect,
        RuntimeView view,
        RuntimeTarget target
    ) noexcept
    {
        return bindingControl({effect, view, target}, ERuntimeControl::DETACH_EFFECT);
    }

    RenderResult<RuntimeRequest> RenderRuntime::remove(RuntimeView view) noexcept
    {
        return control(viewControl(view, ERuntimeControl::REMOVE_VIEW));
    }

    RenderResult<RuntimeRequest> RenderRuntime::remove(RuntimeScene scene) noexcept
    {
        return control(objectControl(scene, ERuntimeControl::REMOVE_SCENE));
    }

    RenderResult<RuntimeRequest> RenderRuntime::remove(RuntimeTarget target) noexcept
    {
        return control(objectControl(target, ERuntimeControl::REMOVE_TARGET));
    }

    RenderResult<RuntimeRequest> RenderRuntime::remove(RuntimeEffect effect) noexcept
    {
        return control(objectControl(effect, ERuntimeControl::REMOVE_EFFECT));
    }

    RenderResult<RuntimeRequest> RenderRuntime::graph(RuntimeBinding binding) noexcept
    {
        return bindingControl(binding, ERuntimeControl::QUERY_GRAPH);
    }

    RenderResult<RuntimeRequest> RenderRuntime::setActive(RuntimeView view, bool active) noexcept
    {
        auto request = viewControl(view, ERuntimeControl::SET_VIEW_ACTIVE);
        request.first = active;
        return control(request);
    }

    RenderResult<RuntimeRequest> RenderRuntime::readback(
        RuntimeBinding binding,
        GraphPassId pass,
        std::uint32_t input,
        std::uint64_t offset,
        std::uint32_t bytes
    ) noexcept
    {
        RuntimeControl request;
        request.first = pass.value();
        request.second = input;
        request.offset = offset;
        request.bytes = bytes;
        return bindingControl(binding, ERuntimeControl::READBACK, request);
    }

    RenderResult<void> RenderRuntime::publishBindingPass(
        RuntimeBinding binding,
        GraphPassId pass,
        std::uint64_t revision,
        std::span<const std::byte> scalars
    ) noexcept
    {
        if (owner_thread_ != detail::transportThreadIdentity())
        {
            return cxx::unexpected(RenderError{kTransportWrongThread});
        }
        const bool wrong_owner =
            binding.effect.owner != binding.view.owner || binding.target.owner != binding.view.owner;
        if (wrong_owner)
        {
            return cxx::unexpected(RenderError{kTransportWrongOwner});
        }
        if (retained_program_)
        {
            return cxx::unexpected(RenderError{kTransportBusy});
        }
        auto written = program_.writeBlob(
            channel_->publication,
            RuntimeBindingPublication{
                binding.view.owner,
                revision,
                binding.view.local.scene.index,
                binding.view.local.scene.gen,
                binding.view.local.view.index,
                binding.view.local.view.gen,
                pass.value(),
                binding.effect.local.index,
                binding.effect.local.gen,
                binding.target.local.index,
                binding.target.local.gen
            },
            scalars
        );
        if (!written)
        {
            return written;
        }
        retained_program_ = true;
        return retryPublication();
    }

    RenderResult<RuntimeRequest> RenderRuntime::readbackTarget(
        RuntimeBinding binding,
        std::uint64_t offset,
        std::uint32_t bytes
    ) noexcept
    {
        RuntimeControl request;
        request.offset = offset;
        request.bytes = bytes;
        return bindingControl(binding, ERuntimeControl::READBACK_TARGET, request);
    }

    RenderResult<void> RenderRuntime::retryPublication() noexcept
    {
        if (owner_thread_ != detail::transportThreadIdentity())
        {
            return cxx::unexpected(RenderError{kTransportWrongThread});
        }
        if (!retained_program_)
        {
            return {};
        }
        auto result = channel_->transport->submit(program_);
        if (result)
        {
            retained_program_ = false;
            channel_->signal();
        }
        return result;
    }

    void RenderRuntime::requestStop() noexcept
    {
        channel_->stop();
    }

    ERuntimeState RenderRuntime::state() const noexcept
    {
        return channel_->state.load(std::memory_order_acquire);
    }

    RuntimeProgress RenderRuntime::progress() const noexcept
    {
        std::array<std::uint64_t, 6> values;
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            values[i] = channel_->counters[i].load(std::memory_order_relaxed);
        }
        return {values[0], values[1], values[2], values[3], values[4], values[5]};
    }

    std::optional<RenderError> RenderRuntime::terminalError() const noexcept
    {
        return state() == ERuntimeState::FAILED ? channel_->failure : std::nullopt;
    }

    std::optional<RenderError> RenderRuntime::pollDiagnostic() noexcept
    {
        if (owner_thread_ != detail::transportThreadIdentity())
        {
            return RenderError{kTransportWrongThread};
        }
        if (!channel_->diagnostics.tryAcquireRead())
        {
            return std::nullopt;
        }
        return channel_->diagnostics.currentRead();
    }

    std::uint64_t RenderRuntime::ownerIdentity() const noexcept
    {
        return channel_->owner;
    }
} // namespace lux::render
