#include "GraphNative.hpp"
#include <algorithm>

namespace lux::render::vulkan
{
    bool detail::matchesReceipt(const NativeGraphBacking& plan, GraphSubmission receipt) noexcept
    {
        if (receipt.slot >= plan.slots.size())
        {
            return false;
        }
        const auto& current = plan.slots[receipt.slot].completion;
        return current && &current->owner() == &receipt.completion.owner() &&
               current->serial() == receipt.completion.serial();
    }

    RenderResult<VNativeImport> ExecutableGraphPlan::exportedBacking(GraphSubmission receipt, GraphResourceId resource)
        const noexcept
    {
        if (!detail::matchesReceipt(*backing_, receipt))
        {
            return cxx::unexpected(RenderError{kWrongOwner});
        }
        const auto& outputs = backing_->logical.identity().outputs;
        const auto declared = std::find_if(
            outputs.begin(),
            outputs.end(),
            [&](const auto& output) { return output.resource == resource; }
        );
        if (declared == outputs.end())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto& native = backing_->slots[receipt.slot].resources[resource.value() - 1];
        if (const auto* image = detail::imageOf(native))
        {
            return VNativeImport{std::cref(*image)};
        }
        if (const auto* buffer = detail::bufferOf(native))
        {
            return VNativeImport{std::cref(*buffer)};
        }
        return cxx::unexpected(RenderError{kInvalidArgument});
    }

    RenderResult<std::uint32_t> ExecutableGraphPlan::resourceStates(
        GraphSubmission receipt,
        GraphResourceId resource,
        std::span<NativeRangeState> destination
    ) const noexcept
    {
        if (!detail::matchesReceipt(*backing_, receipt))
        {
            return cxx::unexpected(RenderError{kWrongOwner});
        }
        const auto count = std::count_if(
            backing_->cells.begin(),
            backing_->cells.end(),
            [&](const auto& cell) { return cell.resource == resource; }
        );
        if (count == 0)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (static_cast<std::size_t>(count) > destination.size())
        {
            return cxx::unexpected(RenderError{kCapacity, {static_cast<std::uint64_t>(count)}});
        }
        const auto& slot = backing_->slots[receipt.slot];
        if (slot.external_use)
        {
            // The external recorder owns the new state facts; Foundation cannot
            // infer its barriers from a completion ticket.
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        std::uint32_t written = 0;
        for (std::uint32_t c = 0; c < backing_->cells.size(); ++c)
        {
            if (backing_->cells[c].resource == resource)
            {
                destination[written++] = {backing_->cells[c].range, slot.states[c], slot.cell_tickets[c]};
            }
        }
        return written;
    }

    RenderResult<GraphSubmission> ExecutableGraphPlan::retainExternalUse(
        GraphSubmission receipt,
        SubmissionTicket last_use
    ) noexcept
    {
        auto& plan = *backing_;
        if (plan.terminal)
        {
            return cxx::unexpected(*plan.terminal);
        }
        const auto queue = std::find(plan.queues.begin(), plan.queues.end(), &last_use.owner());
        const bool wrong_ticket =
            !detail::matchesReceipt(plan, receipt) || queue == plan.queues.end() || !last_use.owner().owns(last_use);
        if (wrong_ticket)
        {
            return cxx::unexpected(RenderError{kWrongOwner});
        }
        auto& slot = plan.slots[receipt.slot];
        auto& retained = slot.queue_tickets[queue - plan.queues.begin()];
        if (retained && retained->serial() > last_use.serial())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        retained = last_use;
        slot.external_use = true;
        for (auto& ticket : slot.cell_tickets)
        {
            if (ticket)
            {
                ticket = last_use;
            }
        }
        // Retain first: even if admission fails, teardown still waits for every
        // external borrower. No failed join can make the old receipt safe to recycle.
        auto join = plan.queues[0]->begin();
        if (!join)
        {
            plan.terminal = join.error();
            return cxx::unexpected(join.error());
        }
        const SubmissionWait wait{last_use, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        const auto waits = last_use.owner().nativeQueue().handle == plan.queues[0]->nativeQueue().handle
                               ? std::span<const SubmissionWait>{}
                               : std::span<const SubmissionWait>{&wait, 1};
        auto completion = std::move(*join).submit(waits);
        if (!completion)
        {
            plan.terminal = completion.error();
            return cxx::unexpected(completion.error());
        }
        slot.completion = slot.queue_tickets[0] = *completion;
        return GraphSubmission{*completion, receipt.slot};
    }
} // namespace lux::render::vulkan
