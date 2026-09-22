#pragma once

#include <cstdint>
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace lux::render
{
// Followed by width * height * 4 owned RGBA bytes. No CPU Context pointer.
struct LUX_TYPE_INFO(both) LUX_COMM_CONFIG(prefix = UiRender, id = lux.render.ui.v1, display = UiRender,
                                           custom_create = true) UiRenderCommConfig
{
    std::uint32_t width{};
    std::uint32_t height{};
};

struct LUX_OP(lane = program, kind = stream, name = UiRenderFrame, method = frame) UiRenderFramePayload
{
    RenderSceneId scene_id{};
    FeatureHandle feature{};
    std::uint32_t attachment_index{};
};
// Stop presenting the previous snapshot without destroying the UI Scene.
// Ordered with admitted frame Programs; GPU images retain their completion guards.
struct LUX_OP(lane = program, kind = stream, name = UiRenderClear, method = clear) UiRenderClearPayload
{
    RenderSceneId scene_id{};
    FeatureHandle feature{};
};

} // namespace lux::render
