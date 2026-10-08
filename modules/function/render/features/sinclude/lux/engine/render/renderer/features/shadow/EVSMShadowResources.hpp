#pragma once
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/render/features/visibility.h>
#include <lux/engine/render/gpu/lifecycle/FifOwned.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace lux::render
{
    class DeviceContext;
    class DescriptorService;

    /// Complete two-image EVSM backing. Rejected native candidates release immediately;
    /// accepted resources retire through the original queue, which must outlive this owner.
    class LUX_ENGINE_FUNCTION_RENDER_FEATURES_PUBLIC EVSMShadowResources final
    {
    public:
        /// Matches EvsmConfigUBO in shadow_evsm.glsl.
        struct alignas(16) ConfigGPU
        {
            float pos_exponent{5.0f};
            float neg_exponent{5.0f};
            float bleed_reduction{0.2f};
            float padding{};
        };

        struct CreateInfo
        {
            DeviceContext& device;
            DescriptorService& descriptors;
            DeferredDestroyQueue& retirement;
            std::span<const VkDescriptorSet> domain_sets;
            uint32_t domain_binding_offset{};
            uint32_t atlas_page_resolution{4096};
            uint32_t atlas_page_count{4};
            uint32_t frames_in_flight{2};
            ConfigGPU config{};
        };

        using CreateResult = Expected<std::unique_ptr<EVSMShadowResources>>;

        [[nodiscard]] static CreateResult create(const CreateInfo& info) noexcept;
        ~EVSMShadowResources() noexcept = default;

        EVSMShadowResources(const EVSMShadowResources&) = delete;
        EVSMShadowResources& operator=(const EVSMShadowResources&) = delete;
        EVSMShadowResources(EVSMShadowResources&&) = delete;
        EVSMShadowResources& operator=(EVSMShadowResources&&) = delete;

        [[nodiscard]] VkImage momentImage() const noexcept
        {
            return moment_.image.get();
        }

        [[nodiscard]] VkImage scratchImage() const noexcept
        {
            return scratch_.image.get();
        }

        /// Separable blur ping-pongs moment -> scratch -> moment.
        [[nodiscard]] VkImage blurredImage() const noexcept
        {
            return momentImage();
        }

        [[nodiscard]] VkImageView momentView() const noexcept
        {
            return moment_.view.get();
        }

        [[nodiscard]] VkImageView scratchView() const noexcept
        {
            return scratch_.view.get();
        }

        [[nodiscard]] VkImageView blurredView() const noexcept
        {
            return momentView();
        }

        [[nodiscard]] VkSampler sampler() const noexcept
        {
            return sampler_;
        }

        [[nodiscard]] VkBuffer configUBO(uint32_t frame_slot) const noexcept
        {
            return config_ubos_[frame_slot % config_ubos_.size()].get();
        }

        [[nodiscard]] uint32_t pageResolution() const noexcept
        {
            return atlas_page_resolution_;
        }

        [[nodiscard]] uint32_t pageCount() const noexcept
        {
            return atlas_page_count_;
        }

        [[nodiscard]] uint32_t framesInFlight() const noexcept
        {
            return static_cast<uint32_t>(config_ubos_.size());
        }

        [[nodiscard]] VkFormat format() const noexcept
        {
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        }

    private:
        struct Atlas
        {
            TFifOwnedAllocated<VkImage> image;
            TFifOwned<VkImageView> view; // Queued before the image on destruction.
        };

        using ConfigBuffer = TFifOwnedAllocated<VkBuffer>;

        EVSMShadowResources(
            const CreateInfo& info,
            Atlas moment,
            Atlas scratch,
            VkSampler sampler,
            std::vector<ConfigBuffer> config
        ) noexcept;

        Atlas moment_;
        Atlas scratch_;
        VkSampler sampler_; // DescriptorService owns the shared sampler.
        std::vector<ConfigBuffer> config_ubos_;
        uint32_t atlas_page_resolution_;
        uint32_t atlas_page_count_;
    };
} // namespace lux::render
