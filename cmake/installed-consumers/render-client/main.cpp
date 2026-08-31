#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/core/RenderEntityId.hpp>

#include <cstdint>

int main()
{
    constexpr auto entity = static_cast<lux::render::RenderEntityId>(0x1234ULL);
    auto channel = lux::render::RenderProgramChannel<>::create(1U);
    return static_cast<std::uint64_t>(entity) == 0x1234ULL && channel ? 0 : 1;
}
