#include <lux/engine/function/render/Capacity.hpp>

// wingdi.h/winspool.h defines DeviceCapabilities as an A/W selector macro.
// The capacity API must remain usable when that macro appears after the header
// was parsed through another include path.
#define DeviceCapabilities DeviceCapabilitiesW

int
main()
{
    lux::render::CapacityDeviceFacts facts{};
    return facts.buffer_device_address ? 1 : 0;
}
