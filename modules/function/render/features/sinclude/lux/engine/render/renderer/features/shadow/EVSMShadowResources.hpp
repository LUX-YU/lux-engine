#pragma once
/**
 * @file EVSMShadowResources.hpp
 * @brief RGBA16F atlas pair (moment + scratch + blurred) + sampler owned
 *        by EVSMShadowTechnique.
 *
 * Three images sit at the same resolution as the PCF atlas (`atlas_page_resolution
 * × atlas_page_count`), all VK_FORMAT_R16G16B16A16_SFLOAT:
 *
 *   1. moment_   — caster output. Color attachment + sampled (blur input).
 *   2. scratch_  — separable blur intermediate (horizontal pass output,
 *                  vertical pass input). Storage + sampled.
 *   3. blurred_  — separable blur final output. Storage + sampled (lighting
 *                  input in C4).
 *
 * Sampler is linear, clamp-to-edge, NO compare op (EVSM uses Chebyshev /
 * exponential ratio on regular sampled values — depth-compare is meaningless
 * for moments).
 *
 * Lifecycle: owned by EVSMShadowTechnique. Created lazily on first
 * `EVSMShadowTechnique::buildResources()`; destroyed in `destroyResources()`.
 * Both calls are driven by `ShadowMapFeature::initAndAttachTo` once we have
 * a device + VmaAllocator + frames-in-flight count.
 *
 * Plan reference: .internal/plan/evsm-shadow-implementation-guide.md §2.3
 * (resource layout) and §3 / C3 (caster + blur compute).
 */

#include <lux/engine/render/gpu/VmaFwd.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>   // shared moment sampler
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC EVSMShadowResources
    {
    public:
        struct InitInfo
        {
            VkDevice     device                = VK_NULL_HANDLE;
            VmaAllocator allocator             = VK_NULL_HANDLE;
            uint32_t     atlas_page_resolution = 4096;
            uint32_t     atlas_page_count      = 4;
            uint32_t     frames_in_flight      = 2;
            /// Supplies the shared moment sampler. Required.
            DescriptorService* descriptor_svc  = nullptr;
        };

        /// CPU-side mirror of the config UBO read by shadow_evsm.glsl at
        /// binding LUX_LIGHT_SET/10. Matches the struct layout declared in
        /// shadow_evsm.glsl's `EvsmConfigUBO`.
        struct alignas(16) ConfigGPU
        {
            float pos_exponent;
            float neg_exponent;
            float bleed_reduction;
            float _pad;
        };

        EVSMShadowResources() = default;
        ~EVSMShadowResources() { if (initialized_) shutdown(); }

        EVSMShadowResources(const EVSMShadowResources&)            = delete;
        EVSMShadowResources& operator=(const EVSMShadowResources&) = delete;

        void init(const InitInfo& info);
        void shutdown();

        [[nodiscard]] bool isInitialized() const noexcept { return initialized_; }

        // ── Atlas image accessors ──────────────────────────────────────────
        // Separable blur ping-pongs moment → scratch → moment, so the *final*
        // blurred result lives back in the moment image. `blurredImage/View`
        // therefore alias the moment image: lighting samples the same physical
        // image the caster wrote, after blur_v overwrites it. This drops the
        // EVSM atlas from 3 images to 2 (−⅓ VRAM) with zero quality change —
        // the separable blur never needs all three live at once.
        [[nodiscard]] VkImage     momentImage()  const noexcept { return moment_image_; }
        [[nodiscard]] VkImage     scratchImage() const noexcept { return scratch_image_; }
        [[nodiscard]] VkImage     blurredImage() const noexcept { return moment_image_; }
        [[nodiscard]] VkImageView momentView()   const noexcept { return moment_view_; }
        [[nodiscard]] VkImageView scratchView()  const noexcept { return scratch_view_; }
        [[nodiscard]] VkImageView blurredView()  const noexcept { return moment_view_; }
        [[nodiscard]] VkSampler   sampler()      const noexcept { return sampler_; }

        // ── Per-FIF config UBO (read by shadow_evsm.glsl) ────────────────
        [[nodiscard]] VkBuffer configUBO(uint32_t frame_slot) const noexcept;
        /// Update the config for all FIF slots. Cheap — writes ≤32 bytes
        /// to persistently-mapped UBOs.
        void writeConfig(const ConfigGPU& cfg);

        // ── Dimensions (mirrors PCF atlas layout for tile-index sharing) ──
        [[nodiscard]] uint32_t pageResolution() const noexcept { return atlas_page_resolution_; }
        [[nodiscard]] uint32_t pageCount()      const noexcept { return atlas_page_count_; }
        [[nodiscard]] uint32_t framesInFlight() const noexcept { return frames_in_flight_; }

        [[nodiscard]] VkFormat format() const noexcept { return VK_FORMAT_R16G16B16A16_SFLOAT; }

    private:
        // Allocates one of the two images with COLOR_ATTACHMENT + STORAGE +
        // SAMPLED usage. `is_color_attachment_target` toggles between the moment
        // image (which needs COLOR_ATTACHMENT for the caster output, and is also
        // the final blur target) and the scratch image (blur intermediate,
        // STORAGE + SAMPLED only).
        void createImage(VkImage& image, VmaAllocation& alloc, VkImageView& view,
                         bool is_color_attachment_target);
        void createSampler();

        bool         initialized_           = false;
        VkDevice     device_                = VK_NULL_HANDLE;
        VmaAllocator allocator_             = VK_NULL_HANDLE;
        uint32_t     atlas_page_resolution_ = 0;
        uint32_t     atlas_page_count_      = 0;

        // Two physical atlases (was three): the separable blur ping-pongs
        //   caster → moment, blur_h: moment → scratch, blur_v: scratch → moment
        // so the final blurred result is read back from `moment_image_`.
        VkImage       moment_image_  = VK_NULL_HANDLE;
        VmaAllocation moment_alloc_  = VK_NULL_HANDLE;
        VkImageView   moment_view_   = VK_NULL_HANDLE;

        VkImage       scratch_image_ = VK_NULL_HANDLE;
        VmaAllocation scratch_alloc_ = VK_NULL_HANDLE;
        VkImageView   scratch_view_  = VK_NULL_HANDLE;

        // Borrowed from the DescriptorService cache, which owns it until device
        // teardown — this class must not destroy it.
        DescriptorService* descriptor_svc_ = nullptr;
        VkSampler     sampler_       = VK_NULL_HANDLE;

        // Per-FIF EvsmConfigUBO (persistently mapped). Sized small (32 B).
        uint32_t                    frames_in_flight_ = 0;
        std::array<VkBuffer,      8> config_ubos_       {};
        std::array<VmaAllocation, 8> config_ubo_allocs_ {};
        std::array<void*,         8> config_ubo_mapped_ {};
    };

} // namespace lux::render
