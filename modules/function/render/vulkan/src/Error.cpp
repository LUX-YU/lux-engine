#include <lux/engine/render/vulkan/Error.hpp>

#include <array>

namespace lux::render::vulkan
{
    std::span<const error::ErrorDescriptor> vulkanErrorDescriptors() noexcept
    {
        static constexpr std::array descriptors{
            error::ErrorDescriptor{
                "lux.render.vulkan.native_failure",
                "Vulkan failure (VkResult bits, operation)",
                error::ERecovery::PERMANENT,
                {error::EArgument::HEX, error::EArgument::UNSIGNED}
            },
            error::ErrorDescriptor{
                "lux.render.vulkan.invalid_argument",
                "Invalid native configuration or range",
                error::ERecovery::PERMANENT,
                {}
            },
            error::ErrorDescriptor{
                "lux.render.vulkan.unsupported",
                "Required native capability is unavailable (VkResult bits)",
                error::ERecovery::PERMANENT,
                {error::EArgument::HEX}
            },
            error::ErrorDescriptor{
                "lux.render.vulkan.capacity", "Bounded native capacity exhausted", error::ERecovery::PERMANENT, {}
            },
            error::ErrorDescriptor{
                "lux.render.vulkan.wrong_owner",
                "Foreign submission or native resource owner",
                error::ERecovery::PERMANENT,
                {}
            },
            error::ErrorDescriptor{
                "lux.render.vulkan.busy", "Native work or recording still active", error::ERecovery::PERMANENT, {}
            }
        };
        return descriptors;
    }
} // namespace lux::render::vulkan
