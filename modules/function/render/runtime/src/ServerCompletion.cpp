#include "ServerImpl.hpp"
#include <lux/engine/render/native/vulkan/Readback.hpp>

namespace lux::render
{
    auto RenderServer::Impl::readBackOutput(Readback::Request&& input) noexcept
    {
        auto prepare = [&]() noexcept -> vulkan::ReadbackSender
        {
            const auto* found = attachments.find(input.attachment);
            if (!found)
            {
                return vulkan::ReadbackSender{RenderError{kRuntimeIdentity}};
            }
            auto resource = kGraphInvalidIndex;
            if (!input.output.empty())
            {
                const auto resources = found->context->code().resources();
                const auto output = std::ranges::find(resources, input.output, &vulkan::NativeResourcePlan::key);
                if (output == resources.end())
                {
                    return vulkan::ReadbackSender{RenderError{kRuntimeIdentity}};
                }
                resource = static_cast<std::uint32_t>(output - resources.begin());
            }
            if (!found->context->readbackBytes(resource))
            {
                return vulkan::ReadbackSender{RenderError{vulkan::kUnsupported}};
            }
            return vulkan::ReadbackSender{
                *found->context,
                &submissions,
                [](void* value) noexcept { ++*static_cast<std::atomic<std::uint64_t>*>(value); },
                resource
            };
        };
        return stdexec::then(
            prepare(),
            [](std::vector<std::byte> bytes) noexcept -> Readback::Response
            {
                auto owner = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
                auto payload = RenderPayload::fromVector(owner, 0, owner->size());
                if (!payload)
                {
                    std::terminate(); // exact owned vector range
                }
                return ReadbackResult{std::move(*payload)};
            }
        );
    }

    void RenderServer::Impl::installReadbackCommand() noexcept
    {
        addBuiltin<Readback, &Impl::readBackOutput>();
    }

    void runtime_detail::CommandCompletion::deliver(execution::RunLoop::Work& work) noexcept
    {
        auto& slot = static_cast<CommandCompletion&>(work);
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        auto* sample = test::completionPopped(slot.measurement);
        test::completionAdoptBegin(sample);
#endif
        slot.outbox->enqueue(slot.response_route_, std::move(slot.result_message_));
        delete &slot;
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        test::completionAdoptEnd(sample);
#endif
    }

    std::size_t RenderServer::Impl::drainCompletions(std::size_t budget) noexcept
    {
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        auto* drain_sample = test::completionDrainBegin(render_loop_);
#endif
        const auto consumed = render_loop_.executeReady(budget);
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        test::completionDrainEnd(drain_sample, consumed);
#endif
        return consumed;
    }
} // namespace lux::render
