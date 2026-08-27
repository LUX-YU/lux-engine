#pragma once
/**
 * @file DeviceCaps.hpp
 * @brief Snapshot of what the created VkDevice actually enabled.
 *
 * Fills the gap documented in the mobile-adaptation investigation (§1.1
 * fact 2): the feature structs assembled at device creation used to be
 * init() locals, discarded right after vkCreateDevice — nothing could ever
 * ask "is feature X enabled on this device". DeviceContext now records the
 * enabled state plus the limits that gate our pipeline architecture here;
 * render features read it at attach time to negotiate their EFeatureLevel
 * variant (investigation topic ①, work item 1-1).
 *
 * Semantics: every `bool` reflects what was ENABLED at vkCreateDevice (not
 * merely what the physical device supports). Today the desktop gate hard-
 * requires all of them, so they read true on any device that passed init;
 * once the gate becomes a whitelist (work item 1-2), optional entries start
 * carrying real information. Plain data only — no Vulkan handle ownership.
 */
#include <cstdint>
#include <type_traits>

namespace lux::render
{
    /// `DeviceCaps` 的线布局版本。
    ///
    /// 本结构从「渲染线程内部的一份快照」变成了**跨 comm 的契约**(QueryDeviceCaps 用
    /// `dst_ptr` 直接 memcpy 整块过去),所以布局纪律必须写死:
    ///
    ///   1. 只在**尾部追加**成员 —— 中间插一个 bool 会平移它后面所有成员的偏移,
    ///      而对面是按自己编译出的偏移读的,读到的每一个字段都会错位;
    ///   2. 改动布局(追加、删除、改类型、改顺序)**必须** bump 这个常量;
    ///   3. 客户端拿到的 `DeviceCapsReply::version` 与自己编译期的常量不符时,
    ///      按错误处理,而不是照读一堆错位的 bool。
    /// v2:尾部追加 `max_color_attachments`。
    inline constexpr std::uint32_t kDeviceCapsVersion = 2u;

    struct DeviceCaps
    {
        // ── Features enabled on the VkDevice ─────────────────────────────
        bool synchronization2 = false;
        bool dynamic_rendering = false;
        /// runtimeDescriptorArray + partiallyBound + variableDescriptorCount
        /// + sampledImage UPDATE_AFTER_BIND (the bindless bundle — enabled
        /// and disabled as one unit).
        bool descriptor_indexing = false;
        bool storage_buffer_uab = false;    ///< descriptorBindingStorageBufferUpdateAfterBind
        bool uniform_buffer_uab = false;    ///< descriptorBindingUniformBufferUpdateAfterBind
        bool draw_indirect_count = false;   ///< GPU-driven indirect-count draws
        bool shader_output_layer = false;   ///< VS writes gl_Layer (shadow atlas caster)
        bool buffer_device_address = false; ///< BDA (cull active-mask, future page tables)
        bool shader_int64 = false;          ///< required by buffer_reference SPIR-V
        bool shader_draw_parameters = false;
        bool timeline_semaphore = false;
        bool sampler_anisotropy = false;
        bool multi_draw_indirect = false;
        bool draw_indirect_first_instance = false;
        bool wide_lines = false; ///< editor gizmo/grid line width
        bool shader_clip_distance = false;
        bool null_descriptor = false;         ///< VK_EXT_robustness2 (validation-bug workaround)
        bool external_memory_interop = false; ///< CUDA zero-copy platform handle path
        /// KHR_dynamic_rendering_local_read (Vulkan 1.4 core): input-attachment
        /// style tile-local reads under dynamic rendering — the mobile deferred
        /// G-buffer read path (line-B variant selector, not a tier gate).
        bool dynamic_rendering_local_read = false;

        // ── Limits that gate our pipeline architecture ───────────────────
        uint32_t max_bound_descriptor_sets = 0;     ///< merged layouts need >= 4 (Mali floor)
        uint32_t max_per_stage_storage_buffers = 0; ///< clustered lighting uses 5-8 (verify on device)
        uint32_t max_per_stage_sampled_images = 0;
        uint32_t max_push_constants_size = 0;
        uint32_t max_image_dimension_2d = 0; ///< shadow atlas page ceiling
        uint32_t max_image_array_layers = 0; ///< shadow atlas page count ceiling
        uint64_t max_storage_buffer_range = 0;

        // ── Queue topology ───────────────────────────────────────────────
        bool has_async_compute = false;
        bool has_dedicated_transfer = false;

        /// Vulkan 只保证 4。之前从未查询过(全仓 grep maxColorAttachments 零命中),
        /// 而 RenderPassPlanner 的 local_read 合并预算写死 8 —— 也就是说合并出的组
        /// 最宽可能达到 8 个 color attachment,在只支持 4 的合规设备上
        /// vkCmdBeginRendering 会直接失败。
        ///
        /// 注意 `RenderPassKey::kMaxColorAttachments` **不该**跟着改:那是数组容量
        /// (编译期,只花内存),这里是**策略上限**(运行期,必须让步于设备)。两者
        /// 混为一谈正是这条漏掉的原因。
        uint32_t max_color_attachments = 0;

        // 新成员一律追加在这一行之上;追加后 bump kDeviceCapsVersion。
    };

    static_assert(std::is_trivially_copyable_v<DeviceCaps>, "DeviceCaps 直接 memcpy 过 comm 通道,必须可平凡拷贝。");

} // namespace lux::render
