#include "ServerImpl.hpp"
#include <lux/engine/render/native/vulkan/Readback.hpp>

namespace lux::render
{
    auto RenderServer::Impl::readBackOutput(Readback::Request&& input) noexcept
    {
        auto prepare = [&]() noexcept -> vulkan::ReadbackSender
        {
            const auto& attachment = input.attachment;
            auto* effect = effects.find(attachment.effect);
            if (!effect)
            {
                return vulkan::ReadbackSender{RenderError{kRuntimeIdentity}};
            }
            const auto found = std::ranges::find_if(
                effect->groups,
                [&](const auto& group)
                {
                    return group.scene == attachment.scene && group.view == attachment.view &&
                           group.target == attachment.target;
                }
            );
            if (found == effect->groups.end())
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

    std::size_t RenderServer::Impl::drainCompletions(std::size_t budget) noexcept
    {
        std::size_t consumed = 0;
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        auto* drain_sample = test::completionDrainBegin(completion_events);
#endif
        for (std::size_t i = 0; i < budget; ++i)
        {
            auto* slot = completion_events.tryPop();
            if (!slot)
            {
                break;
            }
            ++consumed;
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
            auto* sample = test::completionPopped(slot->measurement);
            test::completionAdoptBegin(sample);
#endif
            if (const auto resume = std::exchange(slot->resume, nullptr))
            {
                resume(slot->continuation, std::move(slot->continuation_result));
                continue; // The continuation may publish its final event; no post-callback slot access.
            }
            response_outbox_.enqueue(slot->response_route_, std::move(slot->result_message_));
            delete slot;
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
            test::completionAdoptEnd(sample);
#endif
        }
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        test::completionDrainEnd(drain_sample, consumed);
#endif
        return consumed;
    }
} // namespace lux::render
