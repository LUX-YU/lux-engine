#pragma once

#include <lux/engine/render/extensions/graph/GraphError.hpp>
#include <cstdint>
#include <memory>

namespace lux::render
{
    class FramePreparation;
    class RasterProgramSelection;
    class ProducerFactory;

    // One Job owns the workspace. The selected device variant outlives the Job and owns its linked inputs.
    class ProducerWorkspace final
    {
    public:
        [[nodiscard]] RenderResult<void> prepare(FramePreparation& frame) noexcept
        {
            return ops_->prepare(linked_, frame, workspace_.get());
        }

        [[nodiscard]] std::uint64_t rasterProgramRevision() const noexcept
        {
            return ops_->revision(linked_, workspace_.get());
        }

        [[nodiscard]] RenderResult<void> selectRasterPrograms(RasterProgramSelection& selection) noexcept
        {
            return ops_->select(linked_, selection, workspace_.get());
        }

        void settleRasterPrograms(bool adopted) noexcept
        {
            ops_->settle(linked_, adopted, workspace_.get());
        }

    private:
        friend class ProducerFactory;
        struct Ops final
        {
            RenderResult<void> (*prepare)(const void*, FramePreparation&, void*) noexcept;
            std::uint64_t (*revision)(const void*, const void*) noexcept;
            RenderResult<void> (*select)(const void*, RasterProgramSelection&, void*) noexcept;
            void (*settle)(const void*, bool, void*) noexcept;
        };

        using Storage = std::unique_ptr<void, void (*)(void*) noexcept>;
        ProducerWorkspace(const void* linked, const Ops& ops, Storage workspace) noexcept
            : linked_(linked), ops_(&ops), workspace_(std::move(workspace))
        {
        }

        const void* linked_;
        const Ops* ops_;
        Storage workspace_;
    };
}
