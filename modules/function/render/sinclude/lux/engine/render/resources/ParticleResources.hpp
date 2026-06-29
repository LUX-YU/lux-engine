#pragma once
#include <lux/engine/render/resources/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/resources/lifecycle/ResourceCapacityMonitor.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/core/VmaFwd.hpp>
#include <lux/engine/function/visibility.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>
#include <Eigen/Dense>

namespace lux::render
{
    class DeviceContext;

    // =========================================================================
    // ParticleGpu — per-particle data stored in the SSBO.
    // LAYOUT MUST MATCH particle.vert / particle_update.comp (std430, 64 bytes).
    // =========================================================================
    struct alignas(16) ParticleGpu
    {
        float    position[3];   ///< World-space position
        float    lifetime;      ///< Remaining lifetime in seconds (< 0 = dead)
        float    velocity[3];   ///< World-space velocity
        float    max_lifetime;  ///< Initial/max lifetime (denominator for alpha fade)
        float    color[4];      ///< RGBA tint
        float    size;          ///< World-space billboard half-extent
        uint32_t flags;         ///< Bit 0 = alive
        float    _pad[2];
    };
    static_assert(sizeof(ParticleGpu) == 64, "ParticleGpu must be 64 bytes");

    // =========================================================================
    // ParticleEmitParams — CPU-side description of a single particle to spawn.
    // =========================================================================
    struct ParticleEmitParams
    {
        Eigen::Vector3f position{0.f, 0.f, 0.f};
        Eigen::Vector3f velocity{0.f, 1.f, 0.f};
        float           lifetime{2.f};
        float           size{0.15f};
        Eigen::Vector4f color{1.f, 0.6f, 0.1f, 1.f};  // orange-fire default
    };

    // =========================================================================
    // ParticleResources — GPU buffer management for the particle system.
    //
    // Architecture (staging-based upload):
    //   • One GPU_ONLY SSBO (particle_buf_) shared between
    //     compute (read+write) and graphics (readonly).
    //   • compute pass: particle_update.comp reads & writes the same SSBO.
    //   • graphics pass: particle.vert reads the SSBO, draws 6 verts/particle.
    // =========================================================================
    class LUX_FUNCTION_PUBLIC ParticleResources final
        : public GPUResourceBase<ParticleResources, EGPUResourceType::Particle>
    {
    public:
        struct InitInfo
        {
            DeviceContext*        device_ctx        = nullptr;
            uint32_t              max_particles     = 8192;
            VkDescriptorPool      descriptor_pool   = VK_NULL_HANDLE;
            VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
        };

        ParticleResources()  = default;
        ~ParticleResources() { destroy(); }

        // Non-copyable
        ParticleResources(const ParticleResources&) = delete;
        ParticleResources& operator=(const ParticleResources&) = delete;

        // ---- Resource interface ----
        static inline constexpr EGPUResourceType resource_type = EGPUResourceType::Particle;

        void shutdown()                                  { destroy(); initialized_ = false; }
        bool isInitialized() const                       { return initialized_; }
        VkDescriptorSet       getDescriptorSet()       const { return desc_set_;    }
        VkDescriptorSetLayout getDescriptorSetLayout() const { return desc_layout_; }

        void recordBind(VkCommandBuffer cb,
                        VkPipelineBindPoint bp,
                        VkPipelineLayout layout,
                        uint32_t set_index = UINT32_MAX) const
        {
            uint32_t idx = (set_index == UINT32_MAX)
                ? get_binding_set<EParticleSetBindings>::value
                : set_index;
            vkCmdBindDescriptorSets(cb, bp, layout, idx, 1, &desc_set_, 0, nullptr);
        }

        // -------------------------------------------------------------------
        // Lifecycle
        // -------------------------------------------------------------------
        bool init(const InitInfo& info);
        void destroy();

        // -------------------------------------------------------------------
        // Dynamic expansion — call at frame boundary.
        // Records vkCmdCopyBuffer into |cmd| to preserve live particle data.
        // Old buffer is retired and destroyed after |retire_frames| frames.
        // -------------------------------------------------------------------
        [[nodiscard]] bool tryExpand(uint32_t new_max, VkCommandBuffer cmd);

        // -------------------------------------------------------------------
        // Accessors
        // -------------------------------------------------------------------
        [[nodiscard]] VkBuffer              particleBuffer()      const noexcept { return particle_buf_.buf; }
        [[nodiscard]] VkDescriptorSetLayout descriptorLayout()    const noexcept { return desc_layout_;   }
        [[nodiscard]] VkDescriptorSet       descriptorSet()       const noexcept { return desc_set_;      }
        [[nodiscard]] uint32_t              maxParticles()        const noexcept { return max_particles_;  }
        [[nodiscard]] bool                  initialized()         const noexcept { return initialized_;    }

        /// Capacity snapshot for monitoring (used = active_count is unknown here, report 0).
        [[nodiscard]] CapacityStatus capacityStatus() const noexcept
        {
            return { 0, max_particles_, max_particles_ };
        }

        /// Late-bind centralized deferred destroy queue.
        void setDeferredQueue(DeferredDestroyQueue* q) noexcept { deferred_queue_ = q; }

    private:
        struct ParticleBuffer
        {
            VkBuffer      buf{VK_NULL_HANDLE};
            VmaAllocation alloc{VK_NULL_HANDLE};
            VkDeviceSize  size{0};
        };

        DeviceContext* dev_ctx_       = nullptr;
        DeferredDestroyQueue* deferred_queue_ = nullptr;
        uint32_t       max_particles_ = 0;

        ParticleBuffer particle_buf_{};         ///< GPU_ONLY SSBO (staging upload)

        VkDescriptorSetLayout desc_layout_{VK_NULL_HANDLE};
        VkDescriptorSet       desc_set_   {VK_NULL_HANDLE};
        VkDescriptorPool      desc_pool_  {VK_NULL_HANDLE}; ///< borrowed, not owned

        bool allocateDescriptorSet();
        void updateDescriptorSet();
    };

} // namespace lux::render

