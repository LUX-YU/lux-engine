#include "Support.hpp"

#include <array>
#include <type_traits>

using namespace foundation_test;

int main()
{
    static_assert(!std::is_copy_constructible_v<VulkanInstance>);
    static_assert(!std::is_default_constructible_v<VulkanDevice>);
    static_assert(std::is_nothrow_move_constructible_v<VulkanAllocator>);
    std::array<VkQueueFamilyProperties, 3> families{};
    CHECK(!selectQueueFamily(families));
    families[0].queueCount = 1;
    families[0].queueFlags = VK_QUEUE_TRANSFER_BIT;
    CHECK(!selectQueueFamily(families));
    families[2].queueCount = 1;
    families[2].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    CHECK(take(selectQueueFamily(families)) == 2);
    const std::array bad_extensions{"VK_LUX_nonexistent_test_extension"};
    CHECK(!VulkanInstance::create({bad_extensions}));
    CHECK(!VulkanInstance::create({{}, true}));
    Validation validation;
    {
        auto original = instance(validation);
        auto instance_owner = std::move(original);
        CHECK(!original.native() && instance_owner.native());
        CHECK(!VulkanDevice::create(instance_owner, {bad_extensions}));
        CHECK(!VulkanDevice::create(instance_owner, {{}, 9999}));
        auto first = take(VulkanDevice::create(instance_owner));
        auto device = std::move(first);
        CHECK(!first.native() && device.native());
        CHECK(device.caps().max_image_dimension_2d > 0);
        const auto &info = device.properties();
        std::printf(
            "DEVICE name=%s vendor=%u device=%u driver_raw=%u api=%u.%u.%u queue_family=%u\n",
            info.deviceName,
            info.vendorID,
            info.deviceID,
            info.driverVersion,
            VK_API_VERSION_MAJOR(info.apiVersion),
            VK_API_VERSION_MINOR(info.apiVersion),
            VK_API_VERSION_PATCH(info.apiVersion),
            device.queueFamily()
        );
        std::puts(
            "ENABLED api=1.3 device_extensions=[] features=[synchronization2]"
            " layers=[VK_LAYER_KHRONOS_validation] instance_extensions=[VK_EXT_debug_utils,VK_EXT_validation_features]"
            " validation=[synchronization_validation]"
        );
        auto first_allocator = take(VulkanAllocator::create(device));
        auto allocator = std::move(first_allocator);
        CHECK(!first_allocator.native() && allocator.native());
    }
    CHECK(validation.errors == 0);
    std::printf(
        "PASS device validation_errors=%u validation_warnings=%u\n",
        validation.errors.load(),
        validation.warnings.load()
    );
}
