#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>

#include <cstddef>
#include <type_traits>

int main()
{
    using namespace lux::render;

    static_assert(sizeof(CreateScenePayload) == 104u);
    static_assert(offsetof(CreateScenePayload, lit_color_format) == 68u);
    static_assert(offsetof(CreateScenePayload, coordinate_page_size) == 72u);
    static_assert(offsetof(CreateScenePayload, scene_origin_page) == 80u);

    static_assert(sizeof(AddViewPayload) == 80u);
    static_assert(offsetof(AddViewPayload, extent) == 8u);
    static_assert(offsetof(AddViewPayload, name) == 16u);

    static_assert(sizeof(CreateOffscreenTargetPayload) == 12u);
    static_assert(offsetof(CreateOffscreenTargetPayload, flags) == 8u);
    static_assert(sizeof(CreateSurfaceTargetPayload) == 16u);
    static_assert(offsetof(CreateSurfaceTargetPayload, extent) == 8u);
    static_assert(sizeof(ResizeTargetPayload) == 16u);
    static_assert(offsetof(ResizeTargetPayload, new_extent) == 8u);

    static_assert(sizeof(RenderTargetSlotDesc) == 16u);
    static_assert(offsetof(RenderTargetSlotDesc, format) == 0u);
    static_assert(offsetof(RenderTargetSlotDesc, usage) == 4u);
    static_assert(offsetof(RenderTargetSlotDesc, aspect) == 8u);
    static_assert(offsetof(RenderTargetSlotDesc, preserve_content) == 12u);

    static_assert(std::is_trivially_copyable_v<CreateScenePayload>);
    static_assert(std::is_trivially_copyable_v<AddViewPayload>);
    static_assert(std::is_trivially_copyable_v<RenderTargetSlotDesc>);
}
