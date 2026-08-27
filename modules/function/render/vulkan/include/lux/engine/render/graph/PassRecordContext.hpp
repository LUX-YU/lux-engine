#pragma once
/**
 * @file PassRecordContext.hpp
 * @brief Context struct and callback type for feature-driven pass recording.
 *
 * Replaces PassProcessContext / PassProcessorBase. Features set a recorder
 * callback in addPasses() via RGPassBuilder::setRecorder(). The recorder
 * invokes the callback after binding the pass-level pipeline and descriptor
 * sets.
 */

#include <type_traits>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>
#include <lux/engine/function/visibility.h>

namespace lux::cxx
{
    template <typename Key, typename Value, Key Offset> class OffsetAutoSparseSet;
}

namespace lux::render
{
    struct RGResourceHandle;
    struct RGCompiledPass;
    struct RGCompiledGraph;
    struct RGFrameContext;
    class PipelineManager;
    class ResourceRegistry;
    struct View;
    struct RGPhysicalResource;
    using RGPhysicalResourceTable = lux::cxx::OffsetAutoSparseSet<uint32_t, RGPhysicalResource, 0>;

    // ─────────────────────────────────────────────────────────────────────
    //  推送常量的共享视图前缀 —— 全引擎不变量
    // ─────────────────────────────────────────────────────────────────────
    //
    //  约定:recorder 在 **offset 0** 写这个前缀,特性从 **offset
    //  kViewPushPrefixSize** 起写自己的参数。GLSL 侧每个用到推送常量的
    //  着色器都以同名同序的两个 uint 起手。
    //
    //  此前这个约定只活在散文注释里,两侧共 30+ 处硬写字面量 8:C++ 有
    //  3 处 `range.offset < 8u` 判定、5 处 `{stage, 0, 8}` range 声明、
    //  8 处从 offset 8 推送、2 份匿名结构;GLSL 有 19 份手抄。
    //
    //  **漏改一处的后果不是 validation 错误,是推送常量整体错位** ——
    //  着色器读到相邻字段,画面错但没有任何一层会报。给它一个名字,
    //  就是把"改前缀要同步 30 处"压成"改这里"。
    struct ViewPushPrefix
    {
        uint32_t scene_index;
        uint32_t view_index;
    };
    static_assert(
        sizeof(ViewPushPrefix) == 8,
        "视图前缀的大小是 C++ 与 GLSL 之间的 ABI:改它必须同步所有"
        "着色器的推送常量块起手两字段,以及本文件下方的 kViewPushPrefixSize");

    /// 共享前缀占据的字节数。特性自己的推送参数从这个偏移起。
    inline constexpr uint32_t kViewPushPrefixSize = static_cast<uint32_t>(sizeof(ViewPushPrefix));

    /// Context passed to the per-pass recording lambda.
    ///
    /// Before the lambda is called, the framework has already:
    ///   1. Called vkCmdBeginRenderPass (or dynamic rendering equivalent)
    ///   2. Called vkCmdBindPipeline for the pass-level pipeline (if declared)
    ///   3. Called vkCmdBindDescriptorSets per DescriptorBindingStrategy
    ///
    /// The lambda is free to:
    ///   - Re-bind pipelines (per-material variants)
    ///   - Bind additional descriptor sets
    ///   - Push constants
    ///   - Issue any draw/dispatch commands
    struct LUX_FUNCTION_PUBLIC PassRecordContext
    {
        using BindPipelineVariantFn = bool (*)(void* user, const RGCompiledPass& cpass, uint32_t variant_index);

        VkCommandBuffer cmd;
        const RGCompiledPass& cpass;
        const RGCompiledGraph& graph;
        const RGFrameContext& frame;
        PipelineManager& pipeline_manager;
        ResourceRegistry* gpu_resource_manager = nullptr;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        const View* view = nullptr;

        /// Render-area extent for the current pass group.
        /// Filled by the framework before the recorder lambda is invoked.
        /// Features should use these instead of storing their own extent_/viewport_/scissor_.
        VkExtent2D extent{};
        VkViewport viewport{};
        VkRect2D scissor{};

        /// Union of all VkPushConstantRange::stageFlags for ranges overlapping the
        /// shared view prefix (see ViewPushPrefix) in the pass pipeline layout.
        /// Use as the stageFlags argument in every vkCmdPushConstants call within
        /// this pass (both the shared prefix and per-feature pushes at
        /// kViewPushPrefixSize+) to satisfy VUID-vkCmdPushConstants-offset-01795/01796.
        VkShaderStageFlags pc_stage_flags = 0;

        /// Pre-created per-frame attachment views [resource_index][frame_index].
        /// Filled by the recorder before the lambda is invoked. For mip_levels>1
        /// resources this is the FULL-mip view.
        const std::vector<std::vector<VkImageView>>* per_frame_views_ = nullptr;

        /// Pre-created per-frame PER-MIP views [resource_index][frame_index][mip].
        /// Only populated for mip_levels>1 resources.
        const std::vector<std::vector<std::vector<VkImageView>>>* per_frame_views_by_mip_ = nullptr;

        /// Physical resource table — used by resolveBufferHandle.
        const RGPhysicalResourceTable* physical_resources_ = nullptr;
        void* pipeline_bind_user = nullptr;
        BindPipelineVariantFn bind_pipeline_variant_fn = nullptr;

        /// Resolve the VkImageView for a render-graph texture at the current frame.
        /// Only valid for transient / imported textures whose views are pre-created
        /// by the recorder.  Returns VK_NULL_HANDLE if unavailable.
        VkImageView resolveTextureView(RGResourceHandle handle) const noexcept;

        /// Resolve a single-mip VkImageView for a mip_levels>1 texture (e.g. HZB
        /// pyramid level). Returns VK_NULL_HANDLE if the resource has no per-mip
        /// views or @p mip is out of range.
        VkImageView resolveTextureView(RGResourceHandle handle, uint32_t mip) const noexcept;

        /// Resolve the VkBuffer for a render-graph buffer at the current frame.
        /// Returns VK_NULL_HANDLE if unavailable.
        VkBuffer resolveBufferHandle(RGResourceHandle handle) const noexcept;

        /// Bind pass default pipeline variant (index 0).
        /// Returns false when the pass has no graphics pipeline.
        bool bindPassPipeline() const noexcept
        {
            return bindPipelineVariant(0u);
        }

        /// Bind a precompiled pipeline variant declared via setPipeline/addPipeline.
        /// Variant index 0 is setPipeline(); 1..N follow addPipeline() order.
        bool bindPipelineVariant(uint32_t variant_index) const noexcept
        {
            if (bind_pipeline_variant_fn == nullptr)
                return false;
            return bind_pipeline_variant_fn(pipeline_bind_user, cpass, variant_index);
        }
    };

    /// Move-only owning callback wrapper backed by (fn_ptr + user_ptr).
    /// Captured lambdas are heap-owned once at graph build time.
    template <typename R, typename... Args> class OwningDelegate
    {
    public:
        using InvokeFn = R (*)(void* user, Args... args);
        using DestroyFn = void (*)(void* user);
        using CloneFn = void* (*)(const void* user);

        OwningDelegate() = default;
        OwningDelegate(std::nullptr_t)
        {
        }

        template <
            typename F,
            typename D = std::decay_t<F>,
            typename = std::enable_if_t<!std::is_same_v<D, OwningDelegate>>>
        OwningDelegate(F&& fn)
        {
            bind(std::forward<F>(fn));
        }

        OwningDelegate(const OwningDelegate& other)
        {
            copyFrom(other);
        }

        OwningDelegate& operator=(const OwningDelegate& other)
        {
            if (this != &other)
            {
                reset();
                copyFrom(other);
            }
            return *this;
        }

        OwningDelegate(OwningDelegate&& other) noexcept
        {
            moveFrom(std::move(other));
        }

        OwningDelegate& operator=(OwningDelegate&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                moveFrom(std::move(other));
            }
            return *this;
        }

        ~OwningDelegate()
        {
            reset();
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return invoke_ != nullptr;
        }

        R operator()(Args... args) const
        {
            if constexpr (std::is_void_v<R>)
            {
                if (invoke_)
                    invoke_(user_, std::forward<Args>(args)...);
            }
            else
            {
                if (invoke_)
                    return invoke_(user_, std::forward<Args>(args)...);
                return R{};
            }
        }

    private:
        template <typename F> void bind(F&& fn)
        {
            using Fn = std::decay_t<F>;
            static_assert(std::is_invocable_r_v<R, Fn&, Args...>, "OwningDelegate: callable signature mismatch");

            Fn* obj = new Fn(std::forward<F>(fn));
            user_ = obj;
            invoke_ = [](void* user, Args... args) -> R {
                if constexpr (std::is_void_v<R>)
                {
                    (*static_cast<Fn*>(user))(std::forward<Args>(args)...);
                }
                else
                {
                    return (*static_cast<Fn*>(user))(std::forward<Args>(args)...);
                }
            };
            destroy_ = [](void* user) { delete static_cast<Fn*>(user); };

            if constexpr (std::is_copy_constructible_v<Fn>)
            {
                clone_ = [](const void* user) -> void* { return new Fn(*static_cast<const Fn*>(user)); };
            }
            else
            {
                clone_ = nullptr;
            }
        }

        void copyFrom(const OwningDelegate& other)
        {
            invoke_ = other.invoke_;
            destroy_ = other.destroy_;
            clone_ = other.clone_;

            if (other.user_ && other.clone_)
            {
                user_ = other.clone_(other.user_);
            }
            else
            {
                user_ = nullptr;
                invoke_ = nullptr;
                destroy_ = nullptr;
                clone_ = nullptr;
            }
        }

        void moveFrom(OwningDelegate&& other) noexcept
        {
            user_ = other.user_;
            invoke_ = other.invoke_;
            destroy_ = other.destroy_;
            clone_ = other.clone_;
            other.user_ = nullptr;
            other.invoke_ = nullptr;
            other.destroy_ = nullptr;
            other.clone_ = nullptr;
        }

        void reset() noexcept
        {
            if (destroy_ && user_)
                destroy_(user_);
            user_ = nullptr;
            invoke_ = nullptr;
            destroy_ = nullptr;
            clone_ = nullptr;
        }

        void* user_ = nullptr;
        InvokeFn invoke_ = nullptr;
        DestroyFn destroy_ = nullptr;
        CloneFn clone_ = nullptr;
    };

    using PassRecordFn = OwningDelegate<void, const PassRecordContext&>;
    using PassConditionFn = OwningDelegate<bool>;

} // namespace lux::render
