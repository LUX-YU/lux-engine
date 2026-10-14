#include <lux/engine/render/runtime/RenderClient.hpp>
#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)
#error Native closure leaked into RenderClient
#endif
int main()
{
    auto client = lux::render::RenderClient::create("installed.absent");
    if (!client) return 1;
    if ((*client)->hello()) return 2;
    return (*client)->retainedBytes() == 0 ? 0 : 3;
}
