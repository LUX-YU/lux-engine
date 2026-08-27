// ============================================================================
// Swapchain construction transaction — CPU-only Vulkan failure injection.
//
// Every Vulkan entry point used by the factory is supplied through a function
// table. No device is opened: deterministic fake handles prove exact VkResult
// propagation, all-or-nothing publication, and RAII cleanup.
// ============================================================================

#include <lux/engine/gapi/vk/Swapchain.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <utility>

using namespace lux::render;
namespace gvk = lux::gapi::vk;

static_assert(
    !std::is_constructible_v<gvk::Swapchain, VkDevice, const VkSwapchainCreateInfoKHR&, VkAllocationCallbacks*>);
static_assert(std::is_move_constructible_v<gvk::Swapchain>);
static_assert(std::is_move_assignable_v<gvk::Swapchain>);
static_assert(std::is_move_constructible_v<detail::SwapchainImageViews>);
static_assert(std::is_move_assignable_v<detail::SwapchainImageViews>);
static_assert(!std::is_copy_constructible_v<detail::SwapchainImageViews>);
static_assert(noexcept(std::declval<gvk::Swapchain&>().reset()));
static_assert(noexcept(std::declval<gvk::SwapchainBuilder&>().build(std::declval<VkDevice>())));

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{
    template <typename Handle> [[nodiscard]] Handle fakeHandle(std::uintptr_t value) noexcept
    {
        if constexpr (std::is_pointer_v<Handle>)
            return reinterpret_cast<Handle>(value);
        else
            return static_cast<Handle>(value);
    }

    template <typename Handle> [[nodiscard]] std::uintptr_t handleValue(Handle handle) noexcept
    {
        if constexpr (std::is_pointer_v<Handle>)
            return reinterpret_cast<std::uintptr_t>(handle);
        else
            return static_cast<std::uintptr_t>(handle);
    }

    constexpr std::uint32_t kNever = (std::numeric_limits<std::uint32_t>::max)();
    constexpr std::uintptr_t kPhysicalDevice = 0x1000;
    constexpr std::uintptr_t kSurface = 0x2000;
    constexpr std::uintptr_t kDevice = 0x3000;
    constexpr std::uintptr_t kSwapchainBase = 0x4000;
    constexpr std::uintptr_t kImageBase = 0x5000;
    constexpr std::uintptr_t kImageViewBase = 0x6000;

    struct FakeWsi final
    {
        VkResult capabilities_result{VK_SUCCESS};
        VkResult formats_count_result{VK_SUCCESS};
        VkResult formats_values_result{VK_SUCCESS};
        VkResult present_modes_count_result{VK_SUCCESS};
        VkResult present_modes_values_result{VK_SUCCESS};
        VkResult create_swapchain_result{VK_SUCCESS};
        VkResult images_count_result{VK_SUCCESS};
        VkResult images_values_result{VK_SUCCESS};
        VkResult create_image_view_result{VK_SUCCESS};
        std::uint32_t format_count{1};
        std::uint32_t present_mode_count{1};
        std::uint32_t image_count{3};
        std::uint32_t null_swapchain_at{kNever};
        std::uint32_t fail_image_view_at{kNever};
        std::uint32_t null_image_view_at{kNever};
        std::uint32_t create_swapchain_calls{0};
        std::uint32_t destroy_swapchain_calls{0};
        std::uint32_t create_image_view_calls{0};
        std::uint32_t destroy_image_view_calls{0};
        std::array<std::uintptr_t, 8> destroyed_swapchains{};
        std::array<std::uintptr_t, 8> destroyed_image_views{};
    };

    FakeWsi g_fake{};

    void resetFake() noexcept
    {
        g_fake = FakeWsi{};
    }

    VKAPI_ATTR VkResult VKAPI_CALL
    fakeGetSurfaceCapabilities(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* capabilities)
    {
        if (g_fake.capabilities_result != VK_SUCCESS)
            return g_fake.capabilities_result;

        *capabilities = VkSurfaceCapabilitiesKHR{};
        capabilities->minImageCount = 2;
        capabilities->maxImageCount = 4;
        capabilities->currentExtent = {640, 480};
        capabilities->minImageExtent = {1, 1};
        capabilities->maxImageExtent = {4096, 4096};
        capabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL
    fakeGetSurfaceFormats(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t* count, VkSurfaceFormatKHR* formats)
    {
        if (formats == nullptr)
        {
            if (g_fake.formats_count_result != VK_SUCCESS)
                return g_fake.formats_count_result;
            *count = g_fake.format_count;
            return VK_SUCCESS;
        }

        if (g_fake.formats_values_result != VK_SUCCESS)
            return g_fake.formats_values_result;
        if (*count > 0)
        {
            formats[0] = {
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            };
            *count = 1;
        }
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL
    fakeGetPresentModes(VkPhysicalDevice, VkSurfaceKHR, std::uint32_t* count, VkPresentModeKHR* modes)
    {
        if (modes == nullptr)
        {
            if (g_fake.present_modes_count_result != VK_SUCCESS)
                return g_fake.present_modes_count_result;
            *count = g_fake.present_mode_count;
            return VK_SUCCESS;
        }

        if (g_fake.present_modes_values_result != VK_SUCCESS)
            return g_fake.present_modes_values_result;
        if (*count > 0)
        {
            modes[0] = VK_PRESENT_MODE_FIFO_KHR;
            *count = 1;
        }
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL fakeCreateSwapchain(
        VkDevice,
        const VkSwapchainCreateInfoKHR*,
        const VkAllocationCallbacks*,
        VkSwapchainKHR* swapchain
    )
    {
        const std::uint32_t call = g_fake.create_swapchain_calls++;
        if (g_fake.create_swapchain_result != VK_SUCCESS)
            return g_fake.create_swapchain_result;

        *swapchain =
            call == g_fake.null_swapchain_at ? VK_NULL_HANDLE : fakeHandle<VkSwapchainKHR>(kSwapchainBase + call);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL fakeDestroySwapchain(VkDevice, VkSwapchainKHR swapchain, const VkAllocationCallbacks*)
    {
        g_fake.destroyed_swapchains[g_fake.destroy_swapchain_calls++] = handleValue(swapchain);
    }

    VKAPI_ATTR VkResult VKAPI_CALL
    fakeGetSwapchainImages(VkDevice, VkSwapchainKHR, std::uint32_t* count, VkImage* images)
    {
        if (images == nullptr)
        {
            if (g_fake.images_count_result != VK_SUCCESS)
                return g_fake.images_count_result;
            *count = g_fake.image_count;
            return VK_SUCCESS;
        }

        if (g_fake.images_values_result != VK_SUCCESS)
            return g_fake.images_values_result;
        const std::uint32_t requested = *count;
        *count = g_fake.image_count;
        for (std::uint32_t index = 0; index < requested && index < g_fake.image_count; ++index)
        {
            images[index] = fakeHandle<VkImage>(kImageBase + index);
        }
        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL
    fakeCreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* view)
    {
        const std::uint32_t call = g_fake.create_image_view_calls++;
        if (call == g_fake.fail_image_view_at)
            return g_fake.create_image_view_result;

        *view = call == g_fake.null_image_view_at ? VK_NULL_HANDLE : fakeHandle<VkImageView>(kImageViewBase + call);
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL fakeDestroyImageView(VkDevice, VkImageView view, const VkAllocationCallbacks*)
    {
        g_fake.destroyed_image_views[g_fake.destroy_image_view_calls++] = handleValue(view);
    }

    [[nodiscard]] gvk::SwapchainBuildOps fakeOps() noexcept
    {
        return gvk::SwapchainBuildOps{
            gvk::SurfaceQueryOps{
                &fakeGetSurfaceCapabilities,
                &fakeGetSurfaceFormats,
                &fakeGetPresentModes,
            },
            &fakeCreateSwapchain,
            &fakeDestroySwapchain,
            &fakeGetSwapchainImages,
        };
    }

    [[nodiscard]] gvk::SwapchainBuilder fakeBuilder() noexcept
    {
        gvk::SwapchainBuilder builder(
            fakeHandle<VkPhysicalDevice>(kPhysicalDevice),
            fakeHandle<VkSurfaceKHR>(kSurface),
            fakeOps()
        );
        builder.setExtent({640, 480});
        builder.setQueueFamilyIndices({0, 0});
        return builder;
    }

    [[nodiscard]] bool
    hasExactError(const gvk::SwapchainBuildError& error, gvk::ESwapchainBuildStage stage, VkResult result) noexcept
    {
        return error.stage == stage && error.vk_result.has_value() && *error.vk_result == result;
    }

    bool testSurfaceFailurePreservesResult()
    {
        resetFake();
        g_fake.capabilities_result = VK_ERROR_SURFACE_LOST_KHR;

        auto builder = fakeBuilder();
        auto built = builder.build(fakeHandle<VkDevice>(kDevice));
        CHECK(!built);
        CHECK(hasExactError(built.error(), gvk::ESwapchainBuildStage::SURFACE_CAPABILITIES, VK_ERROR_SURFACE_LOST_KHR));
        CHECK(g_fake.create_swapchain_calls == 0);

        const RenderError mapped = detail::mapSwapchainBuildError(built.error());
        CHECK(isError<err::device::SwapchainSurfaceChanged>(mapped));
        CHECK(mapped.args[0] == gvk::encodeSwapchainBuildStage(gvk::ESwapchainBuildStage::SURFACE_CAPABILITIES));
        CHECK(mapped.args[1] == encodeVkResult(VK_ERROR_SURFACE_LOST_KHR));
        CHECK(detail::isRetryableSwapchainFailure(mapped));
        return true;
    }

    bool testEmptyFormatsDoNotInventVkSuccess()
    {
        resetFake();
        g_fake.format_count = 0;

        auto builder = fakeBuilder();
        auto built = builder.build(fakeHandle<VkDevice>(kDevice));
        CHECK(!built);
        CHECK(built.error().stage == gvk::ESwapchainBuildStage::SURFACE_FORMATS);
        CHECK(!built.error().vk_result.has_value());

        const RenderError mapped = detail::mapSwapchainBuildError(built.error());
        CHECK(isError<err::device::SwapchainUnavailable>(mapped));
        CHECK(mapped.args[0] == gvk::encodeSwapchainBuildStage(gvk::ESwapchainBuildStage::SURFACE_FORMATS));
        CHECK(mapped.args[1] == 0);
        CHECK(detail::isRetryableSwapchainFailure(mapped));
        return true;
    }

    bool testCreateFailureIsPermanentAndExact()
    {
        resetFake();
        g_fake.create_swapchain_result = VK_ERROR_INITIALIZATION_FAILED;

        auto builder = fakeBuilder();
        auto built = builder.build(fakeHandle<VkDevice>(kDevice));
        CHECK(!built);
        CHECK(hasExactError(built.error(), gvk::ESwapchainBuildStage::CREATE, VK_ERROR_INITIALIZATION_FAILED));

        const RenderError mapped = detail::mapSwapchainBuildError(built.error());
        CHECK(isError<err::device::SwapchainVulkanCallFailed>(mapped));
        CHECK(mapped.args[1] == encodeVkResult(VK_ERROR_INITIALIZATION_FAILED));
        CHECK(!detail::isRetryableSwapchainFailure(mapped));
        CHECK(g_fake.destroy_swapchain_calls == 0);
        return true;
    }

    bool testOutOfDateCreateIsRetryableAndExact()
    {
        resetFake();
        g_fake.create_swapchain_result = VK_ERROR_OUT_OF_DATE_KHR;

        auto builder = fakeBuilder();
        auto built = builder.build(fakeHandle<VkDevice>(kDevice));
        CHECK(!built);
        CHECK(hasExactError(built.error(), gvk::ESwapchainBuildStage::CREATE, VK_ERROR_OUT_OF_DATE_KHR));

        const RenderError mapped = detail::mapSwapchainBuildError(built.error());
        CHECK(isError<err::device::SwapchainSurfaceChanged>(mapped));
        CHECK(mapped.args[1] == encodeVkResult(VK_ERROR_OUT_OF_DATE_KHR));
        CHECK(detail::isRetryableSwapchainFailure(mapped));
        return true;
    }

    bool testImageEnumerationFailureStillDestroysSwapchain()
    {
        resetFake();
        g_fake.images_values_result = VK_ERROR_DEVICE_LOST;

        {
            auto builder = fakeBuilder();
            auto built = builder.build(fakeHandle<VkDevice>(kDevice));
            CHECK(built.has_value());

            auto images = built->images(builder.imageEnumerationFn());
            CHECK(!images);
            CHECK(hasExactError(images.error(), gvk::ESwapchainBuildStage::ENUMERATE_IMAGES, VK_ERROR_DEVICE_LOST));
            CHECK(g_fake.destroy_swapchain_calls == 0);
        }

        CHECK(g_fake.destroy_swapchain_calls == 1);
        CHECK(g_fake.destroyed_swapchains[0] == kSwapchainBase);
        return true;
    }

    bool testMoveAssignmentReleasesOldOwnerExactlyOnce()
    {
        resetFake();
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};

        {
            auto first = gvk::Swapchain::create(
                fakeHandle<VkDevice>(kDevice),
                info,
                nullptr,
                &fakeCreateSwapchain,
                &fakeDestroySwapchain
            );
            auto second = gvk::Swapchain::create(
                fakeHandle<VkDevice>(kDevice),
                info,
                nullptr,
                &fakeCreateSwapchain,
                &fakeDestroySwapchain
            );
            CHECK(first.has_value());
            CHECK(second.has_value());

            *first = std::move(*second);
            CHECK(g_fake.destroy_swapchain_calls == 1);
            CHECK(g_fake.destroyed_swapchains[0] == kSwapchainBase);
            CHECK(second->handle() == VK_NULL_HANDLE);

            first->reset();
            first->reset();
            CHECK(g_fake.destroy_swapchain_calls == 2);
            CHECK(g_fake.destroyed_swapchains[1] == kSwapchainBase + 1);
        }

        CHECK(g_fake.destroy_swapchain_calls == 2);
        return true;
    }

    bool testPartialImageViewsRollbackSuccessfulPrefix()
    {
        resetFake();
        g_fake.fail_image_view_at = 2;
        g_fake.create_image_view_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        const std::array images{
            fakeHandle<VkImage>(kImageBase),
            fakeHandle<VkImage>(kImageBase + 1),
            fakeHandle<VkImage>(kImageBase + 2),
        };

        {
            auto builder = fakeBuilder();
            auto swapchain = builder.build(fakeHandle<VkDevice>(kDevice));
            CHECK(swapchain.has_value());

            auto views = detail::createSwapchainImageViews(
                fakeHandle<VkDevice>(kDevice),
                images,
                VK_FORMAT_B8G8R8A8_SRGB,
                detail::SwapchainImageViewOps{
                    &fakeCreateImageView,
                    &fakeDestroyImageView,
                }
            );
            CHECK(!views);
            CHECK(isError<err::device::SwapchainVulkanCallFailed>(views.error()));
            CHECK(
                views.error().args[0] == gvk::encodeSwapchainBuildStage(gvk::ESwapchainBuildStage::CREATE_IMAGE_VIEWS)
            );
            CHECK(views.error().args[1] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
            CHECK(g_fake.create_image_view_calls == 3);
            CHECK(g_fake.destroy_image_view_calls == 2);
            CHECK(g_fake.destroyed_image_views[0] == kImageViewBase);
            CHECK(g_fake.destroyed_image_views[1] == kImageViewBase + 1);
            CHECK(g_fake.destroy_swapchain_calls == 0);
        }

        CHECK(g_fake.destroy_swapchain_calls == 1);
        CHECK(g_fake.destroyed_swapchains[0] == kSwapchainBase);
        return true;
    }

    bool testSuccessWithNullImageViewIsContractFailure()
    {
        resetFake();
        g_fake.null_image_view_at = 1;
        const std::array images{
            fakeHandle<VkImage>(kImageBase),
            fakeHandle<VkImage>(kImageBase + 1),
        };

        auto views = detail::createSwapchainImageViews(
            fakeHandle<VkDevice>(kDevice),
            images,
            VK_FORMAT_B8G8R8A8_SRGB,
            detail::SwapchainImageViewOps{
                &fakeCreateImageView,
                &fakeDestroyImageView,
            }
        );
        CHECK(!views);
        CHECK(isError<err::device::SwapchainBuildContractViolated>(views.error()));
        CHECK(g_fake.destroy_image_view_calls == 1);
        CHECK(g_fake.destroyed_image_views[0] == kImageViewBase);
        return true;
    }
} // namespace

int
main()
{
    if (!testSurfaceFailurePreservesResult())
        return 1;
    if (!testEmptyFormatsDoNotInventVkSuccess())
        return 1;
    if (!testCreateFailureIsPermanentAndExact())
        return 1;
    if (!testOutOfDateCreateIsRetryableAndExact())
        return 1;
    if (!testImageEnumerationFailureStillDestroysSwapchain())
        return 1;
    if (!testMoveAssignmentReleasesOldOwnerExactlyOnce())
        return 1;
    if (!testPartialImageViewsRollbackSuccessfulPrefix())
        return 1;
    if (!testSuccessWithNullImageViewIsContractFailure())
        return 1;

    std::puts("swapchain_factory_test: OK");
    return 0;
}
