#include "GraphNative.hpp"
#include <algorithm>
#include <sstream>

namespace lux::render::vulkan
{
    namespace
    {
        enum class EBarrierPart
        {
            COMPLETE,
            RELEASE,
            ACQUIRE
        };

        void barrier(
            VkCommandBuffer command,
            const detail::ResourceCell& cell,
            const detail::VResourceBacking& backing,
            NativeResourceState source,
            NativeResourceState destination,
            EBarrierPart part = EBarrierPart::COMPLETE,
            VkDeviceSize buffer_offset = 0
        ) noexcept
        {
            const auto source_family = source.family;
            const auto destination_family = destination.family;
            if (part == EBarrierPart::RELEASE)
            {
                destination.stages = 0;
                destination.access = 0;
            }
            if (part == EBarrierPart::ACQUIRE)
            {
                source.stages = 0;
                source.access = 0;
            }
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            VkImageMemoryBarrier2 image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            VkBufferMemoryBarrier2 buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            if (const auto* native = detail::imageOf(backing))
            {
                image.srcStageMask = source.stages;
                image.srcAccessMask = source.access;
                image.dstStageMask = destination.stages;
                image.dstAccessMask = destination.access;
                image.oldLayout = source.layout;
                image.newLayout = destination.layout;
                image.srcQueueFamilyIndex = part == EBarrierPart::COMPLETE ? VK_QUEUE_FAMILY_IGNORED : source_family;
                image.dstQueueFamilyIndex =
                    part == EBarrierPart::COMPLETE ? VK_QUEUE_FAMILY_IGNORED : destination_family;
                image.image = native->native();
                image.subresourceRange = detail::imageRange(std::get<ImageRange>(cell.range));
                dependency.imageMemoryBarrierCount = 1;
                dependency.pImageMemoryBarriers = &image;
            }
            else
            {
                const auto* native_buffer = detail::bufferOf(backing);
                const auto range = std::get<BufferRange>(cell.range);
                buffer.srcStageMask = source.stages;
                buffer.srcAccessMask = source.access;
                buffer.dstStageMask = destination.stages;
                buffer.dstAccessMask = destination.access;
                buffer.srcQueueFamilyIndex = part == EBarrierPart::COMPLETE ? VK_QUEUE_FAMILY_IGNORED : source_family;
                buffer.dstQueueFamilyIndex =
                    part == EBarrierPart::COMPLETE ? VK_QUEUE_FAMILY_IGNORED : destination_family;
                buffer.buffer = native_buffer->native();
                buffer.offset = range.byte_offset + buffer_offset;
                buffer.size = range.byte_count;
                dependency.bufferMemoryBarrierCount = 1;
                dependency.pBufferMemoryBarriers = &buffer;
            }
            vkCmdPipelineBarrier2(command, &dependency);
        }

        void addWait(
            std::array<std::optional<SubmissionWait>, 3>& waits,
            SubmissionTicket ticket,
            VkPipelineStageFlags2 stages
        ) noexcept
        {
            for (auto& existing : waits)
            {
                if (existing && &existing->ticket.owner() == &ticket.owner())
                {
                    if (ticket.serial() > existing->ticket.serial())
                    {
                        existing->ticket = ticket;
                    }
                    existing->stages |= stages;
                    return;
                }
            }
            for (auto& existing : waits)
            {
                if (!existing)
                {
                    existing.emplace(SubmissionWait{ticket, stages});
                    return;
                }
            }
            // Cold queue closure guarantees at most three distinct owners.
            std::terminate();
        }

        RenderResult<SubmissionTicket> submitWaits(
            CommandBatch batch,
            const std::array<std::optional<SubmissionWait>, 3>& waits
        ) noexcept
        {
            const SubmissionWait* first = nullptr;
            for (const auto& wait : waits)
            {
                if (wait)
                {
                    first = &*wait;
                    break;
                }
            }
            if (!first)
            {
                return std::move(batch).submit();
            }
            std::array<SubmissionWait, 3> values{*first, *first, *first};
            std::uint32_t count = 0;
            for (const auto& wait : waits)
            {
                if (wait)
                {
                    values[count++] = *wait;
                }
            }
            return std::move(batch).submit(std::span<const SubmissionWait>{values.data(), count});
        }

        void clearValue(
            VkRenderingAttachmentInfo& attachment,
            const GraphPassInvocation& invocation,
            std::uint32_t field
        ) noexcept
        {
            if (const auto* color = std::get_if<ColorClearValue>(&invocation.fields[field]))
            {
                attachment.clearValue.color = nativeColorClear(*color);
            }
            else if (const auto* depth = std::get_if<DepthStencilClearValue>(&invocation.fields[field]))
            {
                attachment.clearValue.depthStencil = {depth->depth, depth->stencil};
            }
        }

        void recordDraw(
            VkCommandBuffer command,
            const DrawCommand& draw,
            const detail::NativePassRecipe& recipe,
            const detail::NativeGraphBacking& plan,
            detail::SlotPass& bound,
            const GraphPassInvocation& invocation
        ) noexcept
        {
            for (std::size_t i = 0; i < bound.colors.size(); ++i)
            {
                bound.color_scratch[i] = bound.colors[i].native;
                bound.color_scratch[i].imageView = bound.current_views[bound.colors[i].view]->native();
                if (bound.colors[i].resolve_view)
                {
                    bound.color_scratch[i].resolveImageView =
                        bound.current_views[*bound.colors[i].resolve_view]->native();
                }
                clearValue(bound.color_scratch[i], invocation, bound.colors[i].field);
            }
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.viewMask =
                std::get<GraphicsDescription>(plan.programs[*recipe.program].identity().pipeline).view_mask;
            rendering.renderArea.extent = bound.extent;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = static_cast<std::uint32_t>(bound.color_scratch.size());
            rendering.pColorAttachments = bound.color_scratch.data();
            VkRenderingAttachmentInfo depth{}, stencil{};
            if (bound.depth)
            {
                depth = bound.depth->native;
                depth.imageView = bound.current_views[bound.depth->view]->native();
                clearValue(depth, invocation, bound.depth->field);
                rendering.pDepthAttachment = &depth;
            }
            if (bound.stencil)
            {
                stencil = bound.stencil->native;
                stencil.imageView = bound.current_views[bound.stencil->view]->native();
                clearValue(stencil, invocation, bound.stencil->field);
                rendering.pStencilAttachment = &stencil;
            }
            vkCmdBeginRendering(command, &rendering);
            const auto& graphics = std::get<GraphicsDescription>(plan.programs[*recipe.program].identity().pipeline);
            if (!graphics.color_input_indices.empty())
            {
                VkRenderingInputAttachmentIndexInfoKHR indices{
                    VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO_KHR
                };
                indices.colorAttachmentCount = static_cast<std::uint32_t>(graphics.color_input_indices.size());
                indices.pColorAttachmentInputIndices = graphics.color_input_indices.data();
                plan.set_input_indices(command, &indices);
            }
            VkViewport
                viewport{0, 0, static_cast<float>(bound.extent.width), static_cast<float>(bound.extent.height), 0, 1};
            VkRect2D scissor{{0, 0}, bound.extent};
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdSetScissor(command, 0, 1, &scissor);
            vkCmdDraw(command, draw.vertices, draw.instances, draw.first_vertex, draw.first_instance);
            vkCmdEndRendering(command);
        }
    } // namespace

    ExecutableGraphPlan::ExecutableGraphPlan(std::unique_ptr<detail::NativeGraphBacking> backing) noexcept
        : backing_(std::move(backing))
    {
    }

    ExecutableGraphPlan::~ExecutableGraphPlan() noexcept = default;
    ExecutableGraphPlan::ExecutableGraphPlan(ExecutableGraphPlan&&) noexcept = default;
    ExecutableGraphPlan& ExecutableGraphPlan::operator=(ExecutableGraphPlan&&) noexcept = default;

    const LogicalGraphPlan& ExecutableGraphPlan::logical() const noexcept
    {
        return backing_->logical;
    }

    VkDevice ExecutableGraphPlan::device() const noexcept
    {
        return backing_->device;
    }

    RenderResult<std::optional<SubmissionTicket>> ExecutableGraphPlan::lastUse() const noexcept
    {
        if (backing_->terminal)
        {
            return cxx::unexpected(*backing_->terminal);
        }
        std::optional<SubmissionTicket> result;
        for (const auto& slot : backing_->slots)
        {
            if (slot.completion && (!result || slot.completion->serial() > result->serial()))
            {
                result = slot.completion;
            }
        }
        return result;
    }

    NativeGraphStatistics ExecutableGraphPlan::statistics() const noexcept
    {
        return backing_->statistics;
    }

    std::span<const NativeViewRequirement> ExecutableGraphPlan::importViews(GraphResourceId resource) const noexcept
    {
        if (!resource.isValid() || resource.value() > backing_->import_views.size())
        {
            return {};
        }
        return backing_->import_views[resource.value() - 1];
    }

    RenderResult<GraphSubmission> ExecutableGraphPlan::submit(
        const FrameGraphBindings& bindings,
        std::span<const NativeImportBinding> imports,
        std::span<NativeTraceEvent> trace
    ) noexcept
    {
        auto& plan = *backing_;
        if (plan.terminal)
        {
            return cxx::unexpected(*plan.terminal);
        }
        const bool wrong_bindings = &bindings.plan() != &plan.logical || !bindings.values() ||
                                    bindings.frame().frame_slot >= plan.slots.size() ||
                                    imports.size() != plan.imports.size();
        if (wrong_bindings)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (!trace.empty() && trace.size() < traceCapacity())
        {
            return cxx::unexpected(RenderError{kCapacity});
        }
        std::uint32_t trace_count = 0;
        const auto observe = [&](NativeTraceEvent event)
        {
            if (!trace.empty())
            {
                if (trace_count == trace.size())
                {
                    std::terminate();
                }
                trace[trace_count++] = event;
            }
        };
        auto& slot = plan.slots[bindings.frame().frame_slot];
        for (auto* queue : plan.queues)
        {
            auto completion = queue->poll();
            if (!completion)
            {
                plan.terminal = completion.error();
                return cxx::unexpected(completion.error());
            }
            if (queue->recording())
            {
                return cxx::unexpected(RenderError{kBusy});
            }
        }
        for (const auto& ticket : slot.queue_tickets)
        {
            if (ticket && ticket->serial() > ticket->owner().completed())
            {
                return cxx::unexpected(RenderError{kBusy});
            }
        }
        // Admission is bounded before accepting any GPU work.
        for (auto* queue : plan.queues)
        {
            std::uint64_t needed = 0;
            for (unsigned q = 0; q < 3; ++q)
            {
                if (plan.queues[q] == queue)
                {
                    needed += plan.admission[q];
                }
            }
            if (needed > queue->capacity() - (queue->submitted() - queue->completed()))
            {
                return cxx::unexpected(RenderError{kCapacity});
            }
        }
        auto prepared = detail::prepareSlot(plan, slot, bindings, imports);
        if (!prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        slot.external_use = false;
        std::fill(slot.states.begin(), slot.states.end(), NativeResourceState{});
        std::fill(slot.cell_tickets.begin(), slot.cell_tickets.end(), std::nullopt);
        for (const auto& input : imports)
        {
            for (std::uint32_t c = 0; c < plan.cells.size(); ++c)
            {
                if (plan.cells[c].resource == input.resource)
                {
                    const auto initial = detail::importState(input, plan.cells[c]);
                    slot.states[c] = initial.state;
                    slot.cell_tickets[c] = initial.ready;
                }
            }
        }
        for (auto& ticket : slot.pass_tickets)
        {
            ticket.reset();
        }
        slot.queue_tickets = {};
        for (const auto& recipe : plan.recipes)
        {
            const auto pass_index = recipe.command.pass.value() - 1;
            const auto& invocation = *bindings.invocation(recipe.command.pass);
            const auto pass_key = plan.logical.identity().passes[pass_index].key;
            observe(
                {invocation.enabled ? ENativeTrace::PASS : ENativeTrace::SKIP,
                 recipe.command.pass,
                 pass_key,
                 {},
                 ~0u,
                 WholeResource{},
                 recipe.command.queue,
                 {},
                 {}}
            );
            if (!invocation.enabled)
            {
                continue;
            }
            auto& queue = *plan.queues[static_cast<unsigned>(recipe.command.queue)];
            std::array<std::optional<SubmissionWait>, 3> waits{};
            VkPipelineStageFlags2 first_stages = 0;
            for (const auto& use : recipe.uses)
            {
                first_stages |= use.destination.stages;
            }
            if (first_stages == 0)
            {
                first_stages = std::holds_alternative<DispatchCommand>(recipe.command.command)
                                   ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                   : VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
            }
            for (const auto predecessor : recipe.predecessors)
            {
                const auto& ticket = slot.pass_tickets[predecessor];
                if (ticket && ticket->owner().nativeQueue().handle != queue.nativeQueue().handle)
                {
                    addWait(waits, *ticket, first_stages);
                }
            }
            // Different-family ownership is released on its actual last-use
            // queue, then acquired below with identical layouts and exact ranges.
            for (unsigned q = 0; q < 3; ++q)
            {
                if (std::find(plan.queues.begin(), plan.queues.begin() + q, plan.queues[q]) != plan.queues.begin() + q)
                {
                    continue;
                }
                auto& source_queue = *plan.queues[q];
                std::optional<CommandBatch> release;
                VkPipelineStageFlags2 destinations = 0;
                for (const auto& use : recipe.uses)
                {
                    const auto selected = bindings.resourceFor(recipe.command.pass, use.use_index);
                    for (auto c : use.cells)
                    {
                        const auto& cell = plan.cells[c];
                        const auto& ticket = slot.cell_tickets[c];
                        const bool needs_release = cell.resource == selected && ticket &&
                                                   &ticket->owner() == &source_queue &&
                                                   slot.states[c].family != use.destination.family;
                        if (!needs_release)
                        {
                            continue;
                        }
                        if (!release)
                        {
                            auto batch = source_queue.begin();
                            if (!batch)
                            {
                                plan.terminal = batch.error();
                                return cxx::unexpected(batch.error());
                            }
                            release.emplace(std::move(*batch));
                        }
                        barrier(
                            release->native(),
                            cell,
                            slot.resources[cell.resource.value() - 1],
                            slot.states[c],
                            use.destination,
                            EBarrierPart::RELEASE,
                            slot.buffer_offsets[cell.resource.value() - 1]
                        );
                        observe(
                            {ENativeTrace::RELEASE,
                             recipe.command.pass,
                             pass_key,
                             cell.resource,
                             use.use_index,
                             cell.range,
                             static_cast<EQueueRole>(q),
                             slot.states[c],
                             use.destination}
                        );
                        destinations |= use.destination.stages;
                    }
                }
                if (release)
                {
                    auto ticket = std::move(*release).submit();
                    if (!ticket)
                    {
                        plan.terminal = ticket.error();
                        return cxx::unexpected(ticket.error());
                    }
                    slot.queue_tickets[q] = *ticket;
                    // Without maintenance8, a queue-family acquire is not
                    // scoped to the consumer shader/copy stage. The semaphore
                    // must cover the ownership operation itself. Resource
                    // barrier access masks above remain exact.
                    addWait(waits, *ticket, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
                }
            }
            auto batch = queue.begin();
            if (!batch)
            {
                plan.terminal = batch.error();
                return cxx::unexpected(batch.error());
            }
            const auto command = batch->native();
            if (!recipe.alias_barriers.empty())
            {
                VkDependencyInfo alias{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                alias.memoryBarrierCount = static_cast<std::uint32_t>(recipe.alias_barriers.size());
                alias.pMemoryBarriers = recipe.alias_barriers.data();
                vkCmdPipelineBarrier2(command, &alias);
                for (const auto& memory : recipe.alias_barriers)
                {
                    observe(
                        {ENativeTrace::ALIAS,
                         recipe.command.pass,
                         pass_key,
                         {},
                         ~0u,
                         WholeResource{},
                         recipe.command.queue,
                         {memory.srcStageMask, memory.srcAccessMask},
                         {memory.dstStageMask, memory.dstAccessMask}}
                    );
                }
            }
            for (const auto& use : recipe.uses)
            {
                const auto selected = bindings.resourceFor(recipe.command.pass, use.use_index);
                for (const auto c : use.cells)
                {
                    const auto& cell = plan.cells[c];
                    if (cell.resource != selected)
                    {
                        continue;
                    }
                    auto source = slot.states[c];
                    // The new image's UNDEFINED transition is itself a write
                    // to aliased memory. Chain it after the previous image's
                    // accesses as well as making old writes available globally.
                    if (source.stages == 0 && use.alias_source_stages != 0)
                    {
                        source.stages = use.alias_source_stages;
                    }
                    const bool acquire =
                        source.family != VK_QUEUE_FAMILY_IGNORED && source.family != use.destination.family;
                    if (acquire && !slot.cell_tickets[c])
                    {
                        plan.terminal = RenderError{kUnsupported};
                        return cxx::unexpected(*plan.terminal);
                    }
                    const auto& ticket = slot.cell_tickets[c];
                    if (ticket && ticket->owner().nativeQueue().handle != queue.nativeQueue().handle)
                    {
                        addWait(waits, *ticket, use.destination.stages);
                        if (!acquire)
                        {
                            // The layout transition must execute after the
                            // semaphore wait, even without a family transfer.
                            // Availability is supplied by the semaphore; its
                            // destination stage anchors this barrier's source.
                            source.stages = use.destination.stages;
                            source.access = 0;
                        }
                    }
                    barrier(
                        command,
                        cell,
                        slot.resources[cell.resource.value() - 1],
                        source,
                        use.destination,
                        acquire ? EBarrierPart::ACQUIRE : EBarrierPart::COMPLETE,
                        slot.buffer_offsets[cell.resource.value() - 1]
                    );
                    observe(
                        {acquire ? ENativeTrace::ACQUIRE : ENativeTrace::TRANSITION,
                         recipe.command.pass,
                         pass_key,
                         cell.resource,
                         use.use_index,
                         cell.range,
                         recipe.command.queue,
                         source,
                         use.destination}
                    );
                    slot.states[c] = use.destination;
                }
            }
            auto& bound = slot.passes[pass_index];
            if (recipe.program)
            {
                const auto& program = plan.programs[*recipe.program];
                const auto point = std::holds_alternative<DrawCommand>(recipe.command.command)
                                       ? VK_PIPELINE_BIND_POINT_GRAPHICS
                                       : VK_PIPELINE_BIND_POINT_COMPUTE;
                vkCmdBindPipeline(command, point, program.native());
                auto bound_result = bound.descriptors->bind(command, bound.dynamic_offsets, invocation.scalars);
                if (!bound_result)
                {
                    plan.terminal = bound_result.error();
                    return cxx::unexpected(bound_result.error());
                }
            }
            if (const auto* dispatch = std::get_if<DispatchCommand>(&recipe.command.command))
            {
                vkCmdDispatch(command, dispatch->x, dispatch->y, dispatch->z);
            }
            else if (const auto* draw = std::get_if<DrawCommand>(&recipe.command.command))
            {
                recordDraw(command, *draw, recipe, plan, bound, invocation);
            }
            else
            {
                detail::recordCopies(command, recipe, slot, bindings);
            }
            // Explicit host visibility is part of an output contract, never inferred from fence polling alone.
            for (const auto& output : plan.logical.identity().outputs)
            {
                if (output.kind != EGraphOutput::READBACK)
                {
                    continue;
                }
                for (std::uint32_t c = 0; c < plan.cells.size(); ++c)
                {
                    const auto& cell = plan.cells[c];
                    const bool written_here = std::any_of(
                        recipe.uses.begin(),
                        recipe.uses.end(),
                        [&](const auto& use)
                        {
                            return bindings.resourceFor(recipe.command.pass, use.use_index) == cell.resource &&
                                   (use.destination.access & VK_ACCESS_2_TRANSFER_WRITE_BIT) != 0 &&
                                   std::find(use.cells.begin(), use.cells.end(), c) != use.cells.end();
                        }
                    );
                    if (written_here && cell.resource == output.resource &&
                        detail::bufferOf(slot.resources[cell.resource.value() - 1]))
                    {
                        const auto state = slot.states[c];
                        if (state.access == VK_ACCESS_2_TRANSFER_WRITE_BIT)
                        {
                            NativeResourceState host{
                                VK_PIPELINE_STAGE_2_HOST_BIT,
                                VK_ACCESS_2_HOST_READ_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED,
                                state.family
                            };
                            barrier(
                                command,
                                cell,
                                slot.resources[cell.resource.value() - 1],
                                state,
                                host,
                                EBarrierPart::COMPLETE,
                                slot.buffer_offsets[cell.resource.value() - 1]
                            );
                            observe(
                                {ENativeTrace::TRANSITION,
                                 recipe.command.pass,
                                 pass_key,
                                 cell.resource,
                                 ~0u,
                                 cell.range,
                                 recipe.command.queue,
                                 state,
                                 host}
                            );
                            slot.states[c] = host;
                        }
                    }
                }
            }
            auto submitted = submitWaits(std::move(*batch), waits);
            if (!submitted)
            {
                plan.terminal = submitted.error();
                return cxx::unexpected(submitted.error());
            }
            slot.pass_tickets[pass_index] = *submitted;
            slot.queue_tickets[static_cast<unsigned>(recipe.command.queue)] = *submitted;
            for (const auto& use : recipe.uses)
            {
                const auto selected = bindings.resourceFor(recipe.command.pass, use.use_index);
                for (auto c : use.cells)
                {
                    if (plan.cells[c].resource == selected)
                    {
                        slot.cell_tickets[c] = *submitted;
                    }
                }
            }
        }
        std::array<std::optional<SubmissionWait>, 3> final_waits{};
        for (const auto& ticket : slot.queue_tickets)
        {
            if (ticket && ticket->owner().nativeQueue().handle != plan.queues[0]->nativeQueue().handle)
            {
                addWait(final_waits, *ticket, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            }
        }
        auto join = plan.queues[0]->begin();
        if (!join)
        {
            plan.terminal = join.error();
            return cxx::unexpected(join.error());
        }
        auto last = submitWaits(std::move(*join), final_waits);
        if (!last)
        {
            plan.terminal = last.error();
            return cxx::unexpected(last.error());
        }
        slot.queue_tickets[0] = *last;
        slot.completion = *last;
        slot.frame_serial = bindings.frame().frame_serial;
        return GraphSubmission{*last, bindings.frame().frame_slot, trace_count};
    }

    RenderResult<bool> ExecutableGraphPlan::completed(GraphSubmission receipt) noexcept
    {
        if (receipt.slot >= backing_->slots.size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto& slot = backing_->slots[receipt.slot];
        const bool matches = slot.completion && &slot.completion->owner() == &receipt.completion.owner() &&
                             slot.completion->serial() == receipt.completion.serial();
        if (!matches)
        {
            return cxx::unexpected(RenderError{kWrongOwner});
        }
        bool done = true;
        for (const auto& ticket : slot.queue_tickets)
        {
            if (ticket)
            {
                auto result = const_cast<SubmissionQueue&>(ticket->owner()).poll();
                if (!result)
                {
                    return cxx::unexpected(result.error());
                }
                done = done && *result >= ticket->serial();
            }
        }
        return done;
    }

    RenderResult<void> ExecutableGraphPlan::readback(
        GraphSubmission receipt,
        GraphResourceId resource,
        VkDeviceSize offset,
        std::span<std::byte> destination
    ) noexcept
    {
        auto done = completed(receipt);
        if (!done)
        {
            return cxx::unexpected(done.error());
        }
        if (!*done)
        {
            return cxx::unexpected(RenderError{kBusy});
        }
        const auto& resources = backing_->slots[receipt.slot].resources;
        if (!resource.isValid() || resource.value() > resources.size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto* buffer = detail::bufferOf(resources[resource.value() - 1]);
        if (!buffer)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto& outputs = backing_->logical.identity().outputs;
        const bool is_readback = std::any_of(
            outputs.begin(),
            outputs.end(),
            [&](const auto& output) { return output.resource == resource && output.kind == EGraphOutput::READBACK; }
        );
        const auto base = backing_->slots[receipt.slot].buffer_offsets[resource.value() - 1];
        const auto size = backing_->logical.identity().resources[resource.value() - 1].buffer().byte_size;
        if (!is_readback || offset > size || destination.size() > size - offset)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        return buffer->read(offset + base, destination);
    }

    RenderResult<void> ExecutableGraphPlan::readbackPass(
        GraphSubmission receipt,
        GraphPassId pass,
        std::uint32_t input,
        VkDeviceSize offset,
        std::span<std::byte> destination
    ) noexcept
    {
        auto done = completed(receipt);
        if (!done)
        {
            return cxx::unexpected(done.error());
        }
        if (!*done)
        {
            return cxx::unexpected(RenderError{kBusy});
        }
        const auto& passes = backing_->slots[receipt.slot].passes;
        const bool invalid_pass = !pass.isValid() || pass.value() > passes.size();
        if (invalid_pass || input >= passes[pass.value() - 1].readbacks.size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        return passes[pass.value() - 1].readbacks[input].read(offset, destination);
    }
} // namespace lux::render::vulkan
