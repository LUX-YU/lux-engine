#pragma once

#include <lux/engine/render/extensions/graph/GraphError.hpp>
#include <cstdint>
#include <memory>

namespace lux::render
{
    namespace detail { class RenderGraphInstance; }

    class FramePreparation;
    class ProducerFactory;

    // One Job owns the workspace. The selected device variant outlives the Job and owns its linked inputs.
    class ProducerWorkspace final
    {
        friend class detail::RenderGraphInstance;
        friend class ProducerFactory;

        [[nodiscard]] RenderResult<void> prepare(FramePreparation& frame, const void* draw_mapping) noexcept
        {
            return prepare_(linked_, frame, workspace_.get(), draw_mapping);
        }

        using Prepare = RenderResult<void> (*)(const void*, FramePreparation&, void*, const void*) noexcept;
        using Storage = std::unique_ptr<void, void (*)(void*) noexcept>;
        ProducerWorkspace(const void* linked, Prepare prepare, Storage workspace) noexcept
            : linked_(linked), prepare_(prepare), workspace_(std::move(workspace))
        {
        }

        const void* linked_;
        Prepare prepare_;
        Storage workspace_;
    };
}
