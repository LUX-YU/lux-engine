#pragma once

#include <cstdint>
#include <lux/engine/render/extensions/graph/GraphError.hpp>

namespace lux::render
{
    class FramePreparation;
    class RasterProgramSelection;
    struct TargetDesc;

    // The context and its borrowed target are valid only during prepare().
    class InstanceData
    {
    public:
        virtual ~InstanceData() noexcept = default;
        // Advances whenever the selected program set changes. Queried only on the Server owner.
        [[nodiscard]] virtual std::uint64_t rasterProgramRevision() const noexcept { return 0; }
        [[nodiscard]] virtual RenderResult<void> selectRasterPrograms(RasterProgramSelection&) noexcept
        {
            return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        // Called on the owner after a selection succeeds or is abandoned. Publish any candidate producer data here.
        virtual void settleRasterPrograms(bool) noexcept {}
        [[nodiscard]] virtual RenderResult<void> prepare(FramePreparation&) noexcept = 0;
    };
} // namespace lux::render
