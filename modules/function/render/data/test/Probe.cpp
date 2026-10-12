#include "Golden.data.hpp"
#if defined(REJECT_GPU)
static_assert(lux::render::GpuData<CpuOnly>);
#elif defined(REJECT_WIRE)
static_assert(lux::render::WireData<CpuOnly>);
#elif defined(REJECT_AUTHORITY)
static_assert(lux::render::CpuAuthoredGpuData<ParticleGpu>);
#elif defined(REJECT_METADATA)
struct Unknown
{
};

static_assert(lux::render::DescribedData<Unknown>);
#elif defined(REJECT_VULKAN)
#include <vulkan/vulkan.h>
#elif defined(REJECT_RUNTIME)
#include <lux/engine/render/runtime/Runtime.hpp>
#elif defined(REJECT_GRAPH)
#include <lux/engine/render/graph/Plan.hpp>
#elif defined(REJECT_TRANSPORT)
#include <lux/engine/render/transport/Transport.hpp>
#endif
int main() {}
