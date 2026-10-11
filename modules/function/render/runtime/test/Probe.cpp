#include <lux/engine/render/runtime/Runtime.hpp>
#include <type_traits>

using namespace lux::render;
static_assert(!std::is_copy_constructible_v<RuntimeRequest>);
static_assert(std::is_nothrow_move_constructible_v<RuntimeRequest>);
static_assert(!std::is_convertible_v<RuntimeScene, RuntimeView>);
#ifdef VK_VERSION_1_0
#error Neutral Runtime header leaked Vulkan
#endif

#ifdef LUX_PROBE_REQUEST_COPY
void rejected(const RuntimeRequest& source)
{
    RuntimeRequest copy(source);
}
#endif

int main()
{
#ifdef LUX_PROBE_ID
    RuntimeScene scene{};
    RuntimeView view = scene;
#endif
    return 0;
}
