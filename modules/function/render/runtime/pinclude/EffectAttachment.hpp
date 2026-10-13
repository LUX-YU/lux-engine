#pragma once

#include <lux/engine/render/runtime/DeviceSession.hpp>

namespace lux::render
{
    // Unique editable use relation, owned only by Backend's EffectId sparse index.
    // Graph is an immutable code pin. Context/slot are stable, non-owning Native references.
    struct EffectAttachment
    {
        RenderEffectId effect;
        EffectBinding binding;
        RenderGraphId graph;
        vulkan::Graph code;
        std::vector<vulkan::ResourceLease<vulkan::Buffer>> leases;
        std::vector<GraphImportBinding> imports;
        std::vector<vulkan::NativeImportBinding> native_imports;
        vulkan::GraphExecution* context{};
        std::uint32_t execution{};

        EffectAttachment(RenderEffectId id, EffectBinding relation, RenderGraphId compiled, vulkan::Graph pin) noexcept
            : effect(id), binding(relation), graph(compiled), code(std::move(pin))
        {
        }

        EffectAttachment(const EffectAttachment&) = delete;
        EffectAttachment& operator=(const EffectAttachment&) = delete;

        EffectAttachment(EffectAttachment&& other) noexcept
            : effect(other.effect), binding(other.binding), graph(other.graph), code(std::move(other.code)),
              leases(std::move(other.leases)), imports(std::move(other.imports)),
              native_imports(std::move(other.native_imports)), context(std::exchange(other.context, nullptr)),
              execution(other.execution)
        {
        }

        EffectAttachment& operator=(EffectAttachment&&) = delete;

        ~EffectAttachment() noexcept
        {
            if (context)
            {
                context->bindings[execution].occupied = false;
            }
            // Leases release next. Store retains backing until all actual queue tickets complete.
        }

        void prepareImports() noexcept
        {
            for (std::size_t i = 0; i < leases.size(); ++i)
            {
                // Shared immutable buffers may still have accepted readers in another slot.
                // Supply their real graphics completion join; initial creation is not a proof
                // that those later readers finished. No resource lookup or new submission.
                native_imports[i].ready = leases[i].lastGraphicsUse();
                native_imports[i].initial.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                native_imports[i].initial.access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            }
        }

        void retainAccepted(const std::array<vulkan::SubmissionQueue*, 3>& queues) noexcept
        {
            for (auto& lease : leases)
            {
                lease.retainAccepted(queues);
            }
        }
    };

    // Ordered, cold-derived borrows. No IDs, ownership, or independently editable relation.
    struct PreparedExecution
    {
        EffectAttachment* attachment;
        const View* view;
    };
} // namespace lux::render
