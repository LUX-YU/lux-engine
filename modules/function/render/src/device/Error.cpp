#include <lux/engine/render/device/Error.hpp>

#include <array>

namespace lux::render
{
    std::span<const error::ErrorDescriptor> deviceErrorDescriptors() noexcept
    {
        static constexpr std::array descriptors{
            error::ErrorDescriptor{
                "lux.render.device.native_failure",
                "Native failure (backend result, operation; Vulkan image qualification=1)",
                error::ERecovery::PERMANENT,
                {error::EArgument::HEX, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.device.invalid_argument",
                "Invalid native configuration or range",
                error::ERecovery::PERMANENT,
                {}
            },
            error::ErrorDescriptor{
                "lux.render.device.unsupported",
                "Required native capability is unavailable (NRI Result)",
                error::ERecovery::PERMANENT,
                {error::EArgument::HEX}
            },
            error::ErrorDescriptor{
                "lux.render.device.capacity",
                "Bounded native capacity exhausted",
                error::ERecovery::PERMANENT,
                {}
            },
            error::ErrorDescriptor{
                "lux.render.device.busy",
                "Native work or recording still active",
                error::ERecovery::PERMANENT,
                {}
            },
            error::ErrorDescriptor{
                "lux.render.device.work_stopped",
                "Native work publication stopped; accepted submissions still drain",
                error::ERecovery::PERMANENT,
                {}
            }
        };
        return descriptors;
    }
} // namespace lux::render
