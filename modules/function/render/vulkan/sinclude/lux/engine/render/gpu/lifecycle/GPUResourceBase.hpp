#pragma once
#include <lux/engine/render/gpu/lifecycle/GPUResourceTypes.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp> // kMaxFramesInFlight
#include <lux/engine/function/render/client/core/FrameStamp.hpp>
#include <string>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace lux::render
{
    /// Per-FIF descriptor revision tracker.  Bump the revision when the
    /// underlying VkBuffer/VkImage handle changes; at upload time, compare
    /// per-slot written revisions to decide whether to re-write.
    struct DescriptorRevision
    {
        uint64_t revision{0};
        uint64_t written[kMaxFramesInFlight]{};

        void bump() noexcept
        {
            ++revision;
        }

        /// Returns true if slot `fi` has not yet been written at the
        /// current revision.  Automatically marks it written.
        bool needsWrite(uint32_t fi) noexcept
        {
            if (written[fi] != revision)
            {
                written[fi] = revision;
                return true;
            }
            return false;
        }
    };

    /**
     * @brief CRTP base for GPU resources — zero virtual overhead.
     *
     * @tparam Derived      The concrete resource class (CRTP self-type).
     * @tparam ResourceType Compile-time resource type tag.
     *
     * Provides:
     *   - `isInitialized()` flag shared by all resources.
     *   - Default no-op implementations for optional hooks (`update`,
     *     `uploadData`, `getDescriptorSet`, etc.).  Derived classes shadow
     *     any method they actually need.
     *
     * ResourceRegistry stores resources as type-erased Slot entries
     * and dispatches shutdown/upload via stored function pointers — no
     * virtual call needed.
     */
    template <typename Derived, EGPUResourceType ResourceType> class GPUResourceBase
    {
    public:
        static constexpr EGPUResourceType resource_type = ResourceType;

        [[nodiscard]] bool isInitialized() const noexcept
        {
            return initialized_;
        }

        // ── Optional hooks — shadowed by Derived when needed ────────────────

        void uploadData(VkCommandBuffer /*cb*/, const FrameStamp& /*stamp*/)
        {
        }

        VkDescriptorSet getDescriptorSet() const
        {
            return VK_NULL_HANDLE;
        }
        VkDescriptorSetLayout getDescriptorSetLayout() const
        {
            return VK_NULL_HANDLE;
        }

        //(已删三个钩子:
        //  · getDebugInfo() —— 基类默认实现 + 8 个子类覆写,全仓零调用点,
        //    且没有任何 detection idiom 探测它(不像 getDescriptorSet 有
        //    ResourceRegistry::HasGetDescriptorSet 那样的间接分发);
        //  · recordBind() —— "资源自己绑自己"的前渲染图机制。绑定现在统一走
        //    渲染图的 useEngineSet / binding plan,LightResources 的那份已在
        //    删除,这是同一机制漏下的部分;
        //  · update() —— 空实现,全仓零覆写、零调用点(render 模块内
        //    `.update()` / `->update()` 均零命中)。每帧维护现在由**安装点**经
        //    addBeginFrameHook 显式登记,资源自己不再继承帧接口 —— 这个钩子是
        //    那次改动漏下的最后一块。)

    protected:
        bool initialized_{false};
        uint32_t frames_in_flight_{2};

        /// Per-FIF descriptor revision tracker.  Subclasses call
        /// `ds_revision_.bump()` when the underlying buffer/image handle
        /// changes, and `ds_revision_.needsWrite(slot)` at upload time
        /// to decide whether to re-write the descriptor set.
        DescriptorRevision ds_revision_;
    };

} // namespace lux::render
