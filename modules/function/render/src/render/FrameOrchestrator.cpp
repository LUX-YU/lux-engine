#include <lux/engine/render/FrameOrchestrator.hpp>

#include <lux/engine/render/renderer/Renderer.hpp>

namespace lux::render
{

void FrameOrchestrator::beginFrame(Renderer& renderer) const
{
    renderer.beginFrame(current_stamp_);
}

void FrameOrchestrator::endFrame(Renderer& renderer) const
{
    renderer.endFrame(current_stamp_.serial);
}

} // namespace lux::render
